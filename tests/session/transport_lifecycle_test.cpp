// SPDX-License-Identifier: GPL-3.0-or-later
// Transport lifetime across a link drop and synchronous open (task 17 followup).
// A dropped link retires the old handle once; a reconnect needs a fresh one and
// never reuses a closed transport. The connect decision is armed before the open
// and elapsed/cancel is enforced after it returns, so a slow or failing open
// cannot slip past a budget or leak a half-open resource.

#include "Fakes.hpp"

#include <aa/session/Lifecycle.hpp>
#include <aa/transport/Frames.hpp>
#include <aa/transport/TcpTransport.hpp>

#include <array>
#include <cstddef>

#include <gtest/gtest.h>

namespace {

namespace ss = aa::session;
namespace test = aa::session::test;
namespace core = aa::core;
namespace tr = aa::transport;
using aa::Error;
using aa::ErrorCode;

TEST(TransportLifecycle, LinkLossRetiresOldTransport) {
    // Given: an active session holding an open transport.
    test::Harness h;
    ASSERT_TRUE(h.drive_to(ss::State::active));
    ASSERT_EQ(h.transport.open_calls(), 1);
    ASSERT_EQ(h.transport.close_calls(), 0);
    // When: the link drops.
    ASSERT_TRUE(h.lc.apply(ss::Event::link_lost).has_value());
    // Then: the old handle is closed once and not left open for reuse.
    EXPECT_EQ(h.transport.close_calls(), 1);
    EXPECT_FALSE(h.transport.is_open());
}

TEST(TransportLifecycle, ReconnectRequiresFreshTransportNotOldHandle) {
    // Given: a lost session whose transport was retired on the drop.
    test::Harness h;
    ASSERT_TRUE(h.drive_to(ss::State::active));
    ASSERT_TRUE(h.lc.apply(ss::Event::link_lost).has_value());
    ASSERT_EQ(h.transport.close_calls(), 1);
    // When: reconnect is attempted with no fresh transport supplied.
    const auto result = h.lc.apply(ss::Event::reconnect);
    // Then: it is refused rather than silently succeeding on the closed handle.
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code(), ErrorCode::transport_unavailable);
    EXPECT_EQ(h.lc.state(), ss::State::reconnecting);
}

TEST(TransportLifecycle, ReplacementFreshTcpReconnectsAndRoundTrips) {
    // Given: an approved session on a real TCP loopback transport.
    auto first = aa::transport::make_tcp_loopback();
    ASSERT_TRUE(first.has_value());
    test::ManualClock clock;
    test::MapTrustStore trust;
    trust.set(test::kApprovedId, aa::trust::Decision::approved);
    ss::Lifecycle lc({}, clock, trust, first.value().client.get());
    ASSERT_TRUE(lc.apply(ss::Event::start_discovery).has_value());
    ASSERT_TRUE(lc.phone_discovered(test::kApprovedId).has_value());
    ASSERT_TRUE(lc.apply(ss::Event::transport_ready).has_value());
    ASSERT_TRUE(lc.apply(ss::Event::negotiation_succeeded).has_value());
    // When: the link drops (retiring the old handle) and a fresh TCP pair is supplied.
    ASSERT_TRUE(lc.apply(ss::Event::link_lost).has_value());
    auto second = aa::transport::make_tcp_loopback();
    ASSERT_TRUE(second.has_value());
    lc.attach_transport(second.value().client.get());
    ASSERT_TRUE(lc.apply(ss::Event::reconnect).has_value());
    // Then: the fresh handle is bound/open and a frame round-trips on it.
    EXPECT_EQ(lc.state(), ss::State::connecting);
    const std::array<std::byte, 3> payload{std::byte{0x0A}, std::byte{0x0B}, std::byte{0x0C}};
    const auto frame = tr::encode_frame(
        tr::FrameHeader{0x01, tr::FrameType::bulk, tr::MessageKind::specific,
                        tr::Encryption::plain},
        payload, 0);
    ASSERT_TRUE(frame.has_value());
    ASSERT_TRUE(second.value().client->send(frame.value()).has_value());
    const auto got = second.value().server->receive();
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got.value(), frame.value());
}

TEST(TransportLifecycle, FakeOpenAdvancingClockBeyondBudgetFails) {
    // Given: a connect budget of 100 ms and an open that consumes 200 ms.
    ss::LifecycleConfig config{ss::PhaseBudget{core::Milliseconds{0}, core::Milliseconds{100},
                                               core::Milliseconds{0}, core::Milliseconds{0}},
                               ss::LinkKind::wired};
    test::Harness h(config);
    h.transport.on_open = [&h] {
        h.clock.advance(core::Milliseconds{200});
        return core::Result<void>{};
    };
    ASSERT_TRUE(h.lc.apply(ss::Event::start_discovery).has_value());
    // When: the synchronous open runs past the already-armed connect deadline.
    const auto result = h.lc.phone_discovered(test::kApprovedId);
    // Then: elapsed time is enforced after the open, terminally.
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code(), ErrorCode::timeout);
    EXPECT_EQ(h.lc.state(), ss::State::failed);
}

TEST(TransportLifecycle, FakeOpenErrorStillCleansUpTransport) {
    // Given: an open that fails while still owning resources.
    test::Harness h;
    h.transport.on_open = [] { return core::Result<void>{Error{ErrorCode::transport_io}}; };
    ASSERT_TRUE(h.lc.apply(ss::Event::start_discovery).has_value());
    // When: the connect open returns an error.
    const auto result = h.lc.phone_discovered(test::kApprovedId);
    // Then: the failure is terminal and the half-open resource is closed once.
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code(), ErrorCode::transport_io);
    EXPECT_EQ(h.lc.state(), ss::State::failed);
    EXPECT_EQ(h.transport.close_calls(), 1);
}

TEST(TransportLifecycle, FakeOpenCancelledDuringOpenFailsClosed) {
    // Given: an open that triggers cancellation while it runs.
    test::Harness h;
    h.transport.on_open = [&h] {
        h.lc.request_stop();
        return core::Result<void>{};
    };
    ASSERT_TRUE(h.lc.apply(ss::Event::start_discovery).has_value());
    // When: the connect open cancels the session mid-open.
    const auto result = h.lc.phone_discovered(test::kApprovedId);
    // Then: cancellation is observed after the open and closes the resource.
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code(), ErrorCode::cancelled);
    EXPECT_EQ(h.lc.state(), ss::State::failed);
    EXPECT_EQ(h.transport.close_calls(), 1);
}

} // namespace
