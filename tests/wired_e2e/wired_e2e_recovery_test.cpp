// SPDX-License-Identifier: GPL-3.0-or-later
//
// Receiver lifecycle (finding F4): the receiver drives the session's deadlines,
// typed link-loss recovery, and teardown. These tests exercise exactly the
// lifecycle operations tools/wired-receiver now performs: tick() fires a phase
// deadline into a typed failure; link_lost -> reconnect (fresh transport) ->
// re-negotiate recovers to active; and request_stop() -> tick() -> stopping ->
// cleanup_finished reaches disconnected.

#include "harness.hpp"

#include "../session/Fakes.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>

namespace {

namespace app = aa::app;
namespace core = aa::core;
namespace ss = aa::session;
using wiredtest::ScriptedTransport;

const aa::trust::PhoneIdentity kApproved{"phone-approved"};

struct Fixture {
    ss::test::MapTrustStore trust;
    ss::test::ManualClock clock;
    wiredtest::CaptureSink capture;
    aa::diagnostics::Diagnostics diagnostics{capture};
    ScriptedTransport transport;
    ScriptedTransport transport2;
    std::unique_ptr<app::WiredSession> session;

    explicit Fixture(const app::WiredSessionConfig &config = app::WiredSessionConfig{}) {
        trust.set(kApproved, aa::trust::Decision::approved);
        session = std::make_unique<app::WiredSession>(
            app::WiredSessionDeps{trust, clock, diagnostics}, config);
        session->attach_transport(&transport);
    }

    void drive_to_active() {
        ASSERT_TRUE(session->apply(ss::Event::start_discovery).has_value());
        ASSERT_TRUE(session->phone_discovered(kApproved).has_value());
        ASSERT_TRUE(session->apply(ss::Event::transport_ready).has_value());
        ASSERT_TRUE(session->apply(ss::Event::negotiation_succeeded).has_value());
        ASSERT_EQ(session->state(), ss::State::active);
    }
};

TEST(WiredRecovery, DeadlineFiringFailsTyped) {
    app::WiredSessionConfig config{};
    config.lifecycle.phases.negotiate = core::Milliseconds{100};
    Fixture f{config};

    ASSERT_TRUE(f.session->apply(ss::Event::start_discovery).has_value());
    ASSERT_TRUE(f.session->phone_discovered(kApproved).has_value());
    ASSERT_TRUE(f.session->apply(ss::Event::transport_ready).has_value());
    ASSERT_EQ(f.session->state(), ss::State::negotiating);

    f.clock.advance(core::Milliseconds{250});
    f.session->tick();
    EXPECT_EQ(f.session->state(), ss::State::failed);
    ASSERT_TRUE(f.session->failure().has_value());
    EXPECT_EQ(f.session->failure()->code(), aa::ErrorCode::timeout);
}

TEST(WiredRecovery, LinkLossThenReconnectRecoversToActive) {
    Fixture f;
    f.drive_to_active();

    ASSERT_TRUE(f.session->apply(ss::Event::link_lost).has_value());
    EXPECT_EQ(f.session->state(), ss::State::reconnecting);
    EXPECT_FALSE(f.transport.is_open());

    f.session->attach_transport(&f.transport2);
    ASSERT_TRUE(f.session->apply(ss::Event::reconnect).has_value());
    EXPECT_EQ(f.session->state(), ss::State::connecting);
    ASSERT_TRUE(f.session->apply(ss::Event::transport_ready).has_value());
    ASSERT_TRUE(f.session->apply(ss::Event::negotiation_succeeded).has_value());
    EXPECT_EQ(f.session->state(), ss::State::active);
    EXPECT_TRUE(f.transport2.is_open());
}

TEST(WiredRecovery, LinkLossRetiresOldTransport) {
    Fixture f;
    f.drive_to_active();
    ASSERT_TRUE(f.transport.is_open());
    ASSERT_TRUE(f.session->apply(ss::Event::link_lost).has_value());
    EXPECT_FALSE(f.transport.is_open());
    EXPECT_GT(f.transport.close_calls(), 0);
}

TEST(WiredRecovery, CleanShutdownReachesStoppingThenDisconnected) {
    Fixture f;
    f.drive_to_active();

    f.session->request_stop();
    f.session->tick();
    EXPECT_EQ(f.session->state(), ss::State::stopping);

    ASSERT_TRUE(f.session->apply(ss::Event::cleanup_finished).has_value());
    EXPECT_EQ(f.session->state(), ss::State::disconnected);
}

TEST(WiredRecovery, CleanupBeforeStoppingIsRefused) {
    Fixture f;
    f.drive_to_active();
    auto outcome = f.session->apply(ss::Event::cleanup_finished);
    ASSERT_FALSE(outcome.has_value());
    EXPECT_EQ(outcome.error().code(), aa::ErrorCode::session_illegal_transition);
    EXPECT_EQ(f.session->state(), ss::State::active);
}

} // namespace
