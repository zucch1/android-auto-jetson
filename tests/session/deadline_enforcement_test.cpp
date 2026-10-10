// SPDX-License-Identifier: GPL-3.0-or-later
// Deadline enforcement at progress entrypoints (task 17 followup). Deadlines are
// enforced when the entrypoint runs, not only when a caller remembers tick(): at
// the exact boundary a progress move is refused with the timeout reason, and one
// nanosecond before it still succeeds. The stop budget stays observable after
// request_stop() so teardown cannot run unbounded.

#include "Fakes.hpp"

#include <aa/session/Lifecycle.hpp>

#include <functional>
#include <vector>

#include <gtest/gtest.h>

namespace {

namespace ss = aa::session;
namespace test = aa::session::test;
namespace core = aa::core;
using aa::ErrorCode;

bool step(const core::Result<void>& result) { return result.has_value(); }

struct DeadlineCase final {
    const char* name;
    ss::LifecycleConfig config;
    core::Nanoseconds exact;
    std::function<void(test::Harness&)> reach;
    std::function<core::Result<void>(test::Harness&)> action;
    ErrorCode fail_code;
    ss::State fail_state;
    ss::State ok_state;
};

[[nodiscard]] std::vector<DeadlineCase> cases() {
    return {
        {"pairing-60s",
         {},
         core::Nanoseconds{60'000'000'000},
         [](test::Harness& h) {
             step(h.lc.apply(ss::Event::start_discovery));
             step(h.lc.authorize_pairing(test::kUnknownId));
             h.trust.set(test::kUnknownId, aa::trust::Decision::approved);
         },
         [](test::Harness& h) { return h.lc.confirm_pairing(test::kUnknownId); },
         ErrorCode::session_pairing_timeout, ss::State::failed, ss::State::connecting},
        {"reconnect-wired-5s",
         {},
         core::Nanoseconds{5'000'000'000},
         [](test::Harness& h) {
             (void)h.drive_to(ss::State::active);
             step(h.lc.apply(ss::Event::link_lost));
             h.attach_fresh();
             step(h.lc.apply(ss::Event::reconnect));
             step(h.lc.apply(ss::Event::transport_ready));
         },
         [](test::Harness& h) { return h.lc.apply(ss::Event::negotiation_succeeded); },
         ErrorCode::timeout, ss::State::failed, ss::State::active},
        {"reconnect-wireless-15s",
         {{}, ss::LinkKind::wireless},
         core::Nanoseconds{15'000'000'000},
         [](test::Harness& h) {
             (void)h.drive_to(ss::State::active);
             step(h.lc.apply(ss::Event::link_lost));
             h.attach_fresh();
             step(h.lc.apply(ss::Event::reconnect));
             step(h.lc.apply(ss::Event::transport_ready));
         },
         [](test::Harness& h) { return h.lc.apply(ss::Event::negotiation_succeeded); },
         ErrorCode::timeout, ss::State::failed, ss::State::active},
        {"connect-phase",
         {ss::PhaseBudget{core::Milliseconds{0}, core::Milliseconds{100},
                          core::Milliseconds{0}, core::Milliseconds{0}},
          ss::LinkKind::wired},
         core::Nanoseconds{100'000'000},
         [](test::Harness& h) {
             step(h.lc.apply(ss::Event::start_discovery));
             step(h.lc.phone_discovered(test::kApprovedId));
         },
         [](test::Harness& h) { return h.lc.apply(ss::Event::transport_ready); },
         ErrorCode::timeout, ss::State::failed, ss::State::negotiating},
        {"negotiate-phase",
         {ss::PhaseBudget{core::Milliseconds{0}, core::Milliseconds{0},
                          core::Milliseconds{100}, core::Milliseconds{0}},
          ss::LinkKind::wired},
         core::Nanoseconds{100'000'000},
         [](test::Harness& h) {
             step(h.lc.apply(ss::Event::start_discovery));
             step(h.lc.phone_discovered(test::kApprovedId));
             step(h.lc.apply(ss::Event::transport_ready));
         },
         [](test::Harness& h) { return h.lc.apply(ss::Event::negotiation_succeeded); },
         ErrorCode::timeout, ss::State::failed, ss::State::active},
        {"discovery-phase",
         {ss::PhaseBudget{core::Milliseconds{100}, core::Milliseconds{0},
                          core::Milliseconds{0}, core::Milliseconds{0}},
          ss::LinkKind::wired},
         core::Nanoseconds{100'000'000},
         [](test::Harness& h) { step(h.lc.apply(ss::Event::start_discovery)); },
         [](test::Harness& h) { return h.lc.phone_discovered(test::kApprovedId); },
         ErrorCode::timeout, ss::State::failed, ss::State::connecting},
        {"consumer-grace-10s",
         {},
         core::Nanoseconds{10'000'000'000},
         [](test::Harness& h) {
             (void)h.drive_to(ss::State::active);
             step(h.lc.apply(ss::Event::degrade));
         },
         [](test::Harness& h) { return h.lc.apply(ss::Event::recover); },
         ErrorCode::timeout, ss::State::reconnecting, ss::State::active},
        {"stop-phase",
         {ss::PhaseBudget{core::Milliseconds{0}, core::Milliseconds{0},
                          core::Milliseconds{0}, core::Milliseconds{100}},
          ss::LinkKind::wired},
         core::Nanoseconds{100'000'000},
         [](test::Harness& h) {
             (void)h.drive_to(ss::State::active);
             h.lc.request_stop();
             h.lc.tick();
         },
         [](test::Harness& h) { return h.lc.apply(ss::Event::cleanup_finished); },
         ErrorCode::timeout, ss::State::failed, ss::State::disconnected},
    };
}

} // namespace

TEST(DeadlineEnforcement, ExactDeadlineRejectsProgressEntry) {
    // Given: each phase armed at t=0; When: the entrypoint runs at the boundary.
    for (const DeadlineCase& c : cases()) {
        test::Harness h(c.config);
        c.reach(h);
        h.clock.set(c.exact);
        const auto result = c.action(h);
        // Then: expiry is enforced here, not deferred to a tick.
        ASSERT_FALSE(result.has_value()) << c.name;
        EXPECT_EQ(result.error().code(), c.fail_code) << c.name;
        EXPECT_EQ(h.lc.state(), c.fail_state) << c.name;
    }
}

TEST(DeadlineEnforcement, JustBeforeDeadlineAllowsProgressEntry) {
    // Given: each phase armed at t=0; When: the entrypoint runs 1 ns early.
    for (const DeadlineCase& c : cases()) {
        test::Harness h(c.config);
        c.reach(h);
        h.clock.set(core::Nanoseconds{c.exact.count - 1});
        const auto result = c.action(h);
        // Then: the boundary is exclusive and the move still succeeds.
        ASSERT_TRUE(result.has_value()) << c.name;
        EXPECT_EQ(h.lc.state(), c.ok_state) << c.name;
    }
}

TEST(DeadlineEnforcement, StopBudgetRemainsObservableAfterRequestStop) {
    // Given: an active session stopped into teardown with a 100 ms stop budget.
    ss::LifecycleConfig config{ss::PhaseBudget{core::Milliseconds{0}, core::Milliseconds{0},
                                               core::Milliseconds{0}, core::Milliseconds{100}},
                               ss::LinkKind::wired};
    test::Harness h(config);
    ASSERT_TRUE(h.drive_to(ss::State::active));
    h.lc.request_stop();
    h.lc.tick();
    ASSERT_EQ(h.lc.state(), ss::State::stopping);
    // When: the stop budget elapses and the owner observes it via tick.
    h.clock.set(core::Nanoseconds{100'000'000});
    h.lc.tick();
    // Then: teardown cannot run past the budget even though the token is stopped.
    EXPECT_EQ(h.lc.state(), ss::State::failed);
    ASSERT_TRUE(h.lc.failure().has_value());
    EXPECT_EQ(h.lc.failure()->code(), ErrorCode::timeout);
}
