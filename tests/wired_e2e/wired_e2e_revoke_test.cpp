// SPDX-License-Identifier: GPL-3.0-or-later
//
// Task 30 wired end-to-end failure path (host-side, no hardware): revoking the
// approved phone mid-session MUST tear the session down (transport closed) and
// MUST prevent a reconnect from resuming cached state - the next connect attempt
// with the same identity fails closed until the phone is re-approved.

#include "harness.hpp"

#include "../session/Fakes.hpp"

#include <gtest/gtest.h>

namespace {

namespace app = aa::app;
namespace diag = aa::diagnostics;
namespace ss = aa::session;

using wiredtest::CaptureSink;
using wiredtest::ScriptedTransport;

const aa::trust::PhoneIdentity kApproved{"phone-approved"};

TEST(WiredE2eRevoke, RevokeDisconnectsAndBlocksReconnectUntilReapproved) {
    ss::test::MapTrustStore trust;
    trust.set(kApproved, aa::trust::Decision::approved);
    ss::test::ManualClock clock;
    CaptureSink capture;
    diag::Diagnostics diagnostics{capture};

    ScriptedTransport transport;
    ScriptedTransport transport2;
    app::WiredSession session{{trust, clock, diagnostics}, app::WiredSessionConfig{}};
    session.attach_transport(&transport);

    ASSERT_TRUE(session.apply(ss::Event::start_discovery).has_value());
    ASSERT_TRUE(session.phone_discovered(kApproved).has_value());
    ASSERT_TRUE(session.apply(ss::Event::transport_ready).has_value());
    ASSERT_TRUE(session.apply(ss::Event::negotiation_succeeded).has_value());
    ASSERT_EQ(session.state(), ss::State::active);
    ASSERT_TRUE(transport.is_open());

    // Revoke mid-session: the approved identity is removed from the store.
    ASSERT_TRUE(trust.forget(kApproved).has_value());

    // Link loss: the session retires the (closed) transport, no active session.
    ASSERT_TRUE(session.apply(ss::Event::link_lost).has_value());
    EXPECT_EQ(session.state(), ss::State::reconnecting);
    EXPECT_GE(transport.close_calls(), 1);

    // Reconnect with a fresh transport: re-admission fails closed (unknown now).
    session.attach_transport(&transport2);
    auto reconnect = session.apply(ss::Event::reconnect);
    ASSERT_FALSE(reconnect.has_value());
    EXPECT_EQ(reconnect.error().code(), aa::ErrorCode::session_unknown_phone);
    EXPECT_EQ(session.state(), ss::State::failed);
    EXPECT_GE(transport2.close_calls(), 1);

    // No resurrection from cached state: a renewed discovery is still refused.
    ASSERT_TRUE(session.apply(ss::Event::reset).has_value());
    ASSERT_EQ(session.state(), ss::State::disconnected);
    ASSERT_TRUE(session.apply(ss::Event::start_discovery).has_value());
    auto discovered = session.phone_discovered(kApproved);
    ASSERT_FALSE(discovered.has_value());
    EXPECT_EQ(discovered.error().code(), aa::ErrorCode::session_unknown_phone);
    EXPECT_NE(session.state(), ss::State::connecting);
    EXPECT_NE(session.state(), ss::State::active);

    // Re-approval re-enables the connect path (the block was the absent trust).
    // The failed cycle retired the transport, so the reconnect supplies a fresh
    // handle (the documented lifecycle replacement seam).
    ASSERT_TRUE(trust.approve_once(kApproved).has_value());
    ASSERT_EQ(session.state(), ss::State::discovering);
    session.attach_transport(&transport2);
    ASSERT_TRUE(session.phone_discovered(kApproved).has_value());
    EXPECT_EQ(session.state(), ss::State::connecting);
    EXPECT_TRUE(transport2.is_open());

    EXPECT_GE(session.diagnostics_emitted(), 3U);
    EXPECT_FALSE(capture.contains("phone-approved"));
}

} // namespace
