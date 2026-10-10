// SPDX-License-Identifier: GPL-3.0-or-later
// Lifecycle behaviour (task 17): happy connect, all-ten-state reachability,
// fail-closed admission, pairing-intent gating, external-approval confirmation,
// pairing timeout, and real Transport open/close orchestration on the shared
// cancellation token.

#include "Fakes.hpp"

#include <aa/session/Lifecycle.hpp>

#include <array>

#include <gtest/gtest.h>

namespace {

namespace ss = aa::session;
namespace test = aa::session::test;
using aa::ErrorCode;

constexpr std::array kStates{
    ss::State::disconnected, ss::State::discovering, ss::State::pairing,
    ss::State::connecting,   ss::State::negotiating, ss::State::active,
    ss::State::degraded,     ss::State::reconnecting, ss::State::stopping,
    ss::State::failed,
};

TEST(Lifecycle, HappyPathReachesActive) {
    // Given: a fresh lifecycle over the narrow seams.
    test::Harness h;
    // When: discovery -> connect -> negotiate -> active.
    ASSERT_TRUE(h.drive_to(ss::State::active));
    // Then: the session is active and the transport was opened once.
    EXPECT_EQ(h.lc.state(), ss::State::active);
    EXPECT_EQ(h.transport.open_calls(), 1);
}

TEST(Lifecycle, AllTenStatesAreReachable) {
    // Given/When/Then: a canonical legal path reaches every one of the ten states.
    for (ss::State target : kStates) {
        test::Harness h;
        ASSERT_TRUE(h.drive_to(target)) << ss::to_string(target);
        EXPECT_EQ(h.lc.state(), target);
    }
}

TEST(Lifecycle, UnknownPhoneFailsClosedOnDiscovery) {
    // Given: a discovery in progress.
    test::Harness h;
    ASSERT_TRUE(h.drive_to(ss::State::discovering));
    // When: an unknown phone is discovered.
    const auto result = h.lc.phone_discovered(test::kUnknownId);
    // Then: admission fails closed and the session never connects.
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code(), ErrorCode::session_unknown_phone);
    EXPECT_EQ(h.lc.state(), ss::State::discovering);
}

TEST(Lifecycle, RejectedPhoneFailsClosedOnDiscovery) {
    // Given: a discovery in progress.
    test::Harness h;
    ASSERT_TRUE(h.drive_to(ss::State::discovering));
    // When: a rejected phone is discovered.
    const auto result = h.lc.phone_discovered(test::kRejectedId);
    // Then: admission fails closed and the session never connects.
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code(), ErrorCode::trust_rejected);
    EXPECT_EQ(h.lc.state(), ss::State::discovering);
}

TEST(Lifecycle, DiscoveryIsNotApprovalAndPairingNeedsIntent) {
    // Given: an unknown phone discovered without pairing intent.
    test::Harness h;
    ASSERT_TRUE(h.drive_to(ss::State::discovering));
    ASSERT_FALSE(h.lc.phone_discovered(test::kUnknownId).has_value());
    EXPECT_EQ(h.lc.state(), ss::State::discovering);
    // When: an explicit authorized pairing intent arrives.
    ASSERT_TRUE(h.lc.authorize_pairing(test::kUnknownId).has_value());
    // Then: the session enters pairing, not connecting.
    EXPECT_EQ(h.lc.state(), ss::State::pairing);
}

TEST(Lifecycle, ConfirmPairingFailsClosedUntilStoreConfirmsApproval) {
    // Given: a pairing window with no confirmed external approval.
    test::Harness h;
    ASSERT_TRUE(h.drive_to(ss::State::pairing));
    // When: confirmation arrives but the store still shows unknown.
    const auto result = h.lc.confirm_pairing(test::kUnknownId);
    // Then: it fails closed into failed and persists nothing.
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code(), ErrorCode::session_unknown_phone);
    EXPECT_EQ(h.lc.state(), ss::State::failed);
    EXPECT_EQ(h.trust.approve_calls(), 0);
}

TEST(Lifecycle, ExternalApprovalConfirmedViaStoreBeforeConnecting) {
    // Given: a pairing window.
    test::Harness h;
    ASSERT_TRUE(h.drive_to(ss::State::pairing));
    // When: external approval lands in the store, then confirmation is sent.
    h.trust.set(test::kUnknownId, aa::trust::Decision::approved);
    ASSERT_TRUE(h.lc.confirm_pairing(test::kUnknownId).has_value());
    // Then: the TrustStore lookup is the gate into connecting.
    EXPECT_EQ(h.lc.state(), ss::State::connecting);
}

TEST(Lifecycle, PairingTimesOutAtSixtySecondsWithoutPersistedApproval) {
    // Given: a pairing window that never gets confirmed.
    test::Harness h;
    ASSERT_TRUE(h.drive_to(ss::State::pairing));
    h.clock.advance(aa::core::Milliseconds{59'999});
    h.lc.tick();
    EXPECT_EQ(h.lc.state(), ss::State::pairing);
    // When: the 60 s budget is crossed.
    h.clock.advance(aa::core::Milliseconds{1});
    h.lc.tick();
    // Then: the session fails with the pairing timeout and approves nothing.
    EXPECT_EQ(h.lc.state(), ss::State::failed);
    ASSERT_TRUE(h.lc.failure().has_value());
    EXPECT_EQ(h.lc.failure()->code(), ErrorCode::session_pairing_timeout);
    EXPECT_EQ(h.trust.approve_calls(), 0);
}

TEST(Lifecycle, OpensTransportWithSharedTokenAndClosesOnCleanup) {
    // Given: a session with an attached transport bound to the shared token.
    test::Harness h;
    ASSERT_TRUE(h.drive_to(ss::State::connecting));
    EXPECT_EQ(h.transport.open_calls(), 1);
    EXPECT_TRUE(h.transport.is_open());
    // When: a stop is requested from off the owner thread and observed.
    h.lc.request_stop();
    EXPECT_TRUE(h.transport.bound_token().stop_requested());
    h.lc.tick();
    ASSERT_EQ(h.lc.state(), ss::State::stopping);
    ASSERT_TRUE(h.lc.apply(ss::Event::cleanup_finished).has_value());
    // Then: cleanup closed the transport exactly once.
    EXPECT_EQ(h.lc.state(), ss::State::disconnected);
    EXPECT_EQ(h.transport.close_calls(), 1);
    EXPECT_FALSE(h.transport.is_open());
}

TEST(Lifecycle, UnknownDirectReconnectFailsClosed) {
    // Given: a lost session whose phone identity is later revoked.
    test::Harness h;
    ASSERT_TRUE(h.drive_to(ss::State::reconnecting));
    h.trust.set(test::kApprovedId, aa::trust::Decision::unknown);
    // When: reconnect is attempted for the now-unknown phone.
    const auto result = h.lc.apply(ss::Event::reconnect);
    // Then: it fails closed into failed and never reaches connecting.
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code(), ErrorCode::session_unknown_phone);
    EXPECT_EQ(h.lc.state(), ss::State::failed);
}

TEST(Lifecycle, EveryRemainingTransitionEdgeIsTraversed) {
    // Given: the edges not already walked by the happy/timeout tests above.
    enum Action { stop_via_cancel, fail_via_error, reset_via_event };
    struct Edge {
        ss::State from;
        ss::State to;
        Action action;
    };
    const Edge edges[]{
        {ss::State::discovering, ss::State::stopping, stop_via_cancel},
        {ss::State::pairing, ss::State::stopping, stop_via_cancel},
        {ss::State::connecting, ss::State::failed, fail_via_error},
        {ss::State::negotiating, ss::State::stopping, stop_via_cancel},
        {ss::State::degraded, ss::State::stopping, stop_via_cancel},
        {ss::State::degraded, ss::State::failed, fail_via_error},
        {ss::State::reconnecting, ss::State::stopping, stop_via_cancel},
        {ss::State::stopping, ss::State::failed, fail_via_error},
        {ss::State::failed, ss::State::disconnected, reset_via_event},
    };
    // When/Then: each edge is driven from its source state to its target.
    for (const Edge& edge : edges) {
        test::Harness h;
        ASSERT_TRUE(h.drive_to(edge.from)) << ss::to_string(edge.from);
        switch (edge.action) {
        case stop_via_cancel:
            ASSERT_TRUE(h.stop_now()) << ss::to_string(edge.from);
            break;
        case fail_via_error:
            ASSERT_TRUE(h.lc.fail(aa::Error{ErrorCode::internal}).has_value())
                << ss::to_string(edge.from);
            break;
        case reset_via_event:
            ASSERT_TRUE(h.lc.apply(ss::Event::reset).has_value()) << ss::to_string(edge.from);
            break;
        }
        EXPECT_EQ(h.lc.state(), edge.to)
            << ss::to_string(edge.from) << " -> " << ss::to_string(edge.to);
    }
}

} // namespace
