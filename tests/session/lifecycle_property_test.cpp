// SPDX-License-Identifier: GPL-3.0-or-later
// Lifecycle property coverage (task 17): unauthorized events are refused from
// every state without moving it, mandated and caller-supplied deadlines hit
// their boundaries on a synthetic clock (no sleeps), and cross-thread
// cancellation only request_stop()s while the owner thread drives teardown.

#include "Fakes.hpp"

#include <aa/session/Lifecycle.hpp>

#include <thread>

#include <gtest/gtest.h>

namespace {

namespace ss = aa::session;
namespace test = aa::session::test;
using aa::ErrorCode;

TEST(PolicyProperty, UnauthorizedEventFromAnyStateIsRejectedAndHoldsState) {
    // Given: a battery of (state, illegal-event) pairs across non-terminal states.
    struct Case {
        ss::State state;
        ss::Event illegal;
    };
    const Case cases[] = {
        {ss::State::disconnected, ss::Event::transport_ready},
        {ss::State::disconnected, ss::Event::negotiation_succeeded},
        {ss::State::disconnected, ss::Event::reset},
        {ss::State::discovering, ss::Event::start_discovery},
        {ss::State::discovering, ss::Event::negotiation_succeeded},
        {ss::State::discovering, ss::Event::reconnect},
        {ss::State::pairing, ss::Event::start_discovery},
        {ss::State::pairing, ss::Event::transport_ready},
        {ss::State::pairing, ss::Event::reconnect},
        {ss::State::connecting, ss::Event::start_discovery},
        {ss::State::connecting, ss::Event::negotiation_succeeded},
        {ss::State::connecting, ss::Event::degrade},
        {ss::State::negotiating, ss::Event::transport_ready},
        {ss::State::negotiating, ss::Event::degrade},
        {ss::State::negotiating, ss::Event::reconnect},
        {ss::State::active, ss::Event::start_discovery},
        {ss::State::active, ss::Event::transport_ready},
        {ss::State::active, ss::Event::recover},
        {ss::State::degraded, ss::Event::degrade},
        {ss::State::degraded, ss::Event::negotiation_succeeded},
        {ss::State::degraded, ss::Event::start_discovery},
        {ss::State::reconnecting, ss::Event::start_discovery},
        {ss::State::reconnecting, ss::Event::transport_ready},
        {ss::State::reconnecting, ss::Event::link_lost},
    };
    // When/Then: each illegal event is typed-refused and the state is held.
    for (const Case& c : cases) {
        test::Harness h;
        ASSERT_TRUE(h.drive_to(c.state)) << ss::to_string(c.state);
        const auto result = h.lc.apply(c.illegal);
        ASSERT_FALSE(result.has_value())
            << ss::to_string(c.state) << " / " << ss::to_string(c.illegal);
        EXPECT_EQ(result.error().code(), ErrorCode::session_illegal_transition);
        EXPECT_EQ(h.lc.state(), c.state);
    }
}

TEST(PolicyProperty, PhoneIdentityEventsAreRefusedOutsideTheirSourceState) {
    // Given/When/Then: identity events only fire from their one source state.
    test::Harness active;
    ASSERT_TRUE(active.drive_to(ss::State::active));
    const auto discovered = active.lc.phone_discovered(test::kApprovedId);
    ASSERT_FALSE(discovered.has_value());
    EXPECT_EQ(discovered.error().code(), ErrorCode::session_illegal_transition);
    const auto authorize = active.lc.authorize_pairing(test::kUnknownId);
    ASSERT_FALSE(authorize.has_value());
    EXPECT_EQ(authorize.error().code(), ErrorCode::session_illegal_transition);
    const auto confirm = active.lc.confirm_pairing(test::kUnknownId);
    ASSERT_FALSE(confirm.has_value());
    EXPECT_EQ(confirm.error().code(), ErrorCode::session_illegal_transition);
    EXPECT_EQ(active.lc.state(), ss::State::active);
}

TEST(PolicyProperty, CallerSuppliedPhaseBudgetBoundsDiscoveryAtTheDeadline) {
    // Given: a caller that bounds discovery at 1 s (not an invented budget).
    ss::LifecycleConfig config{
        ss::PhaseBudget{aa::core::Milliseconds{1'000}, aa::core::Milliseconds{},
                        aa::core::Milliseconds{}, aa::core::Milliseconds{}},
        ss::LinkKind::wired};
    test::Harness h(config);
    ASSERT_TRUE(h.drive_to(ss::State::discovering));
    h.clock.advance(aa::core::Milliseconds{999});
    h.lc.tick();
    EXPECT_EQ(h.lc.state(), ss::State::discovering);
    // When: the caller-supplied budget is crossed.
    h.clock.advance(aa::core::Milliseconds{1});
    h.lc.tick();
    // Then: discovery times out on the supplied budget.
    EXPECT_EQ(h.lc.state(), ss::State::failed);
    ASSERT_TRUE(h.lc.failure().has_value());
    EXPECT_EQ(h.lc.failure()->code(), ErrorCode::timeout);
}

TEST(PolicyProperty, ZeroPhaseBudgetMeansNoInventedDeadline) {
    // Given: no caller-supplied discovery budget (zero = unbounded).
    test::Harness h;
    ASSERT_TRUE(h.drive_to(ss::State::discovering));
    // When: a long synthetic time passes with no budget to expire.
    h.clock.advance(aa::core::Milliseconds{3'600'000});
    h.lc.tick();
    // Then: the controller invents no hidden contract budget.
    EXPECT_EQ(h.lc.state(), ss::State::discovering);
}

TEST(PolicyProperty, WiredReconnectBudgetSpansLossToActiveWithoutReset) {
    // Given: a wired session whose whole-cycle budget is 5 s.
    test::Harness h(ss::LifecycleConfig{{}, ss::LinkKind::wired});
    ASSERT_TRUE(h.drive_to(ss::State::active));
    ASSERT_TRUE(h.lc.apply(ss::Event::link_lost).has_value());
    // When: it hops reconnecting -> connecting -> negotiating mid-budget.
    h.clock.advance(aa::core::Milliseconds{2'000});
    h.attach_fresh();
    ASSERT_TRUE(h.lc.apply(ss::Event::reconnect).has_value());
    ASSERT_TRUE(h.lc.apply(ss::Event::transport_ready).has_value());
    h.clock.advance(aa::core::Milliseconds{2'999});
    h.lc.tick();
    EXPECT_EQ(h.lc.state(), ss::State::negotiating);
    // Then: the contract budget (not a per-phase one) fires at 5 s total.
    h.clock.advance(aa::core::Milliseconds{1});
    h.lc.tick();
    EXPECT_EQ(h.lc.state(), ss::State::failed);
    ASSERT_TRUE(h.lc.failure().has_value());
    EXPECT_EQ(h.lc.failure()->code(), ErrorCode::timeout);
}

TEST(PolicyProperty, WirelessReconnectBudgetIsFifteenSeconds) {
    // Given: a wireless session whose whole-cycle budget is 15 s.
    test::Harness h(ss::LifecycleConfig{{}, ss::LinkKind::wireless});
    ASSERT_TRUE(h.drive_to(ss::State::active));
    ASSERT_TRUE(h.lc.apply(ss::Event::link_lost).has_value());
    h.clock.advance(aa::core::Milliseconds{14'999});
    h.lc.tick();
    EXPECT_EQ(h.lc.state(), ss::State::reconnecting);
    // Then: it fails only after the 15 s wireless budget.
    h.clock.advance(aa::core::Milliseconds{1});
    h.lc.tick();
    EXPECT_EQ(h.lc.state(), ss::State::failed);
}

TEST(PolicyProperty, ConsumerGraceRecoversWithinTenSecondsAndExpiresAfter) {
    // Given: a degraded session with the 10 s consumer grace.
    test::Harness h;
    ASSERT_TRUE(h.drive_to(ss::State::active));
    ASSERT_TRUE(h.lc.apply(ss::Event::degrade).has_value());
    h.clock.advance(aa::core::Milliseconds{9'999});
    h.lc.tick();
    EXPECT_EQ(h.lc.state(), ss::State::degraded);
    // When: the grace is crossed with no consumer recovery.
    h.clock.advance(aa::core::Milliseconds{1});
    h.lc.tick();
    // Then: the lifecycle moves into bounded reconnect.
    EXPECT_EQ(h.lc.state(), ss::State::reconnecting);
}

TEST(PolicyProperty, ConsumerGraceRecoveryWithinWindowReturnsToActive) {
    // Given: a degraded session that recovers before the grace expires.
    test::Harness h;
    ASSERT_TRUE(h.drive_to(ss::State::active));
    ASSERT_TRUE(h.lc.apply(ss::Event::degrade).has_value());
    h.clock.advance(aa::core::Milliseconds{5'000});
    ASSERT_TRUE(h.lc.apply(ss::Event::recover).has_value());
    EXPECT_EQ(h.lc.state(), ss::State::active);
}

TEST(PolicyProperty, CrossThreadCancellationOnlyRequestStopAndTeardownCompletes) {
    // Given: an active session with an open transport.
    test::Harness h;
    ASSERT_TRUE(h.drive_to(ss::State::active));
    EXPECT_EQ(h.transport.close_calls(), 0);
    // When: another thread cancels (only request_stop) and the owner observes it.
    std::thread stopper([&h] { h.lc.request_stop(); });
    stopper.join();
    EXPECT_TRUE(h.lc.token().stop_requested());
    h.lc.tick();
    ASSERT_EQ(h.lc.state(), ss::State::stopping);
    EXPECT_EQ(h.transport.close_calls(), 0);
    // Then: progress is refused once cancelled, teardown still closes cleanly.
    const auto refused = h.lc.apply(ss::Event::start_discovery);
    ASSERT_FALSE(refused.has_value());
    EXPECT_EQ(refused.error().code(), ErrorCode::cancelled);
    ASSERT_TRUE(h.lc.apply(ss::Event::cleanup_finished).has_value());
    EXPECT_EQ(h.lc.state(), ss::State::disconnected);
    EXPECT_EQ(h.transport.close_calls(), 1);
}

} // namespace
