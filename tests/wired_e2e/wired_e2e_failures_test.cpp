// SPDX-License-Identifier: GPL-3.0-or-later
//
// Task 30 wired end-to-end adversarial seams (host-side): mid-projection stop
// tears the session down cleanly (cancel/resume), and malformed wire input
// fails typed - never a crash and never a delivery.

#include "harness.hpp"

#include "../session/Fakes.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <vector>

namespace {

namespace app = aa::app;
namespace diag = aa::diagnostics;
namespace ss = aa::session;

using wiredtest::CaptureSink;
using wiredtest::RecordingSink;
using wiredtest::ScriptedTransport;

const aa::trust::PhoneIdentity kApproved{"phone-approved"};

void drive_to_active(app::WiredSession &session, ScriptedTransport &transport) {
    ASSERT_TRUE(session.apply(ss::Event::start_discovery).has_value());
    ASSERT_TRUE(session.phone_discovered(kApproved).has_value());
    ASSERT_TRUE(session.apply(ss::Event::transport_ready).has_value());
    transport.push_inbound(wiredtest::discovery_request_frame());
    auto discovery = session.poll_transport();
    ASSERT_TRUE(discovery.has_value());
    transport.push_inbound(wiredtest::open_request_frame(99));
    auto opened = session.poll_transport();
    ASSERT_TRUE(opened.has_value());
    ASSERT_TRUE(session.apply(ss::Event::negotiation_succeeded).has_value());
    ASSERT_EQ(session.state(), ss::State::active);
}

TEST(WiredE2eFailures, StopMidProjectionTearsDownCleanlyAndResumes) {
    ss::test::MapTrustStore trust;
    trust.set(kApproved, aa::trust::Decision::approved);
    ss::test::ManualClock clock;
    CaptureSink capture;
    diag::Diagnostics diagnostics{capture};
    RecordingSink video_sink;

    ScriptedTransport transport;
    app::WiredSession session{{trust, clock, diagnostics}, app::WiredSessionConfig{}};
    ASSERT_TRUE(session.configure(wiredtest::golden_services()).has_value());
    ASSERT_TRUE(session.register_sink(aa::protocol::ChannelRole::video, video_sink).has_value());
    session.attach_transport(&transport);
    drive_to_active(session, transport);

    session.request_stop();
    session.tick();
    EXPECT_EQ(session.state(), ss::State::stopping);
    ASSERT_TRUE(session.apply(ss::Event::cleanup_finished).has_value());
    EXPECT_EQ(session.state(), ss::State::disconnected);
    EXPECT_GE(transport.close_calls(), 1);
    EXPECT_FALSE(transport.is_open());

    ScriptedTransport transport2;
    session.attach_transport(&transport2);
    ASSERT_TRUE(session.apply(ss::Event::start_discovery).has_value());
    ASSERT_TRUE(session.phone_discovered(kApproved).has_value());
    ASSERT_TRUE(session.apply(ss::Event::transport_ready).has_value());
    EXPECT_EQ(session.state(), ss::State::negotiating);
    EXPECT_TRUE(transport2.is_open());
}

TEST(WiredE2eFailures, MalformedFrameFailsTypedAndNeverDelivers) {
    ss::test::MapTrustStore trust;
    trust.set(kApproved, aa::trust::Decision::approved);
    ss::test::ManualClock clock;
    CaptureSink capture;
    diag::Diagnostics diagnostics{capture};
    RecordingSink video_sink;

    ScriptedTransport transport;
    app::WiredSession session{{trust, clock, diagnostics}, app::WiredSessionConfig{}};
    ASSERT_TRUE(session.configure(wiredtest::golden_services()).has_value());
    ASSERT_TRUE(session.register_sink(aa::protocol::ChannelRole::video, video_sink).has_value());
    session.attach_transport(&transport);
    drive_to_active(session, transport);

    const std::size_t delivered_before = video_sink.payloads.size();
    const std::array<std::byte, 4> malformed{std::byte{0x00}, std::byte{0x13}, std::byte{0x00},
                                             std::byte{0x00}};
    transport.push_inbound(std::vector<std::byte>(malformed.begin(), malformed.end()));
    auto outcome = session.poll_transport();
    ASSERT_FALSE(outcome.has_value());
    EXPECT_NE(outcome.error().code(), aa::ErrorCode::internal);
    EXPECT_EQ(video_sink.payloads.size(), delivered_before);
}

TEST(WiredE2eFailures, ShortControlPayloadFailsTyped) {
    ss::test::MapTrustStore trust;
    trust.set(kApproved, aa::trust::Decision::approved);
    ss::test::ManualClock clock;
    CaptureSink capture;
    diag::Diagnostics diagnostics{capture};
    RecordingSink video_sink;

    ScriptedTransport transport;
    app::WiredSession session{{trust, clock, diagnostics}, app::WiredSessionConfig{}};
    ASSERT_TRUE(session.configure(wiredtest::golden_services()).has_value());
    ASSERT_TRUE(session.register_sink(aa::protocol::ChannelRole::video, video_sink).has_value());
    session.attach_transport(&transport);
    drive_to_active(session, transport);

    transport.push_inbound(wiredtest::control_frame({std::byte{0x00}}));
    auto outcome = session.poll_transport();
    ASSERT_FALSE(outcome.has_value());
    EXPECT_EQ(outcome.error().code(), aa::ErrorCode::invalid_argument);
}

} // namespace
