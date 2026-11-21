// SPDX-License-Identifier: GPL-3.0-or-later
#include <aa/app/HandoffEligibility.hpp>
#include "../../tools/wireless-receiver/AdmissionEvents.hpp"
#include "../session/Fakes.hpp"
#include <aa/trust/Identity.hpp>
#include <gtest/gtest.h>
#include <atomic>
#include <thread>

using namespace std::chrono_literals;
using EligibilityClock = aa::app::HandoffEligibilityCoordinator::Clock;
using Retirement = aa::app::HandoffEligibilityCoordinator::Retirement;
EligibilityClock::time_point real_eligibility_now() noexcept
    asm("__real__ZNSt6chrono3_V212steady_clock3nowEv");
EligibilityClock::time_point wrapped_eligibility_now() noexcept
    asm("__wrap__ZNSt6chrono3_V212steady_clock3nowEv");

namespace {
thread_local std::optional<EligibilityClock::time_point> virtual_now;

struct VirtualClock {
    VirtualClock() { virtual_now = real_eligibility_now(); }
    ~VirtualClock() { virtual_now.reset(); }
    void advance(std::chrono::milliseconds elapsed) { *virtual_now += elapsed; }
};
}

EligibilityClock::time_point wrapped_eligibility_now() noexcept {
    return virtual_now ? *virtual_now : real_eligibility_now();
}

namespace {
struct EligibilityFixture {
    aa::session::test::MapTrustStore trust;
    const aa::helper::Source source{7, 0};
    const aa::helper::ObservedPeer peer{41, {"12:34:56:78:9A:BC", "12:34:56:78:9A:BD"}};
    aa::helper::ApSnapshot snapshot{{"fixture", "test-password", "12:34:56:78:9a:bd", "127.0.0.1", 5288, 1}};
    std::atomic<bool> helper_live{true};
    std::atomic<bool> stalled{false};
    std::atomic<bool> slow_refresh{false};
    std::atomic<bool> moderate_refresh{false}, moderate_ops{false};
    std::atomic<bool> slow_setup{false};
    std::atomic<bool> early_sample{false};
    std::atomic<bool> sampled{false};
    std::atomic<bool> authorize_wait{false}, authorizing{false};
    std::atomic<unsigned> denied{};
    std::atomic<bool> fail_next_denial{false};
    std::atomic<bool> reject_denials{false};
    std::atomic<bool> deny_wait{false};
    std::atomic<unsigned> denial_attempts{};
    std::atomic<unsigned> denial_workers{}, denials_in_flight{};
    const std::thread::id owner{std::this_thread::get_id()};
    std::atomic<std::uint64_t> denied_handle{};
    std::atomic<std::uint64_t> observed_handle{41};
    std::uint64_t replacement_after_denial{};
    std::atomic<unsigned> allowed{};
    std::atomic<unsigned> initial_observations{};
    std::atomic<bool> bootstrap_complete{true};
    std::atomic<unsigned> withdrawn{};
    bool listening{};
    bool ordering_ok{true};
    aa::trust::PhoneIdentity identity{aa::trust::identity_key(aa::trust::TransportIdentity{
        aa::trust::WirelessIdentity{peer.identity.address, peer.identity.adapter}})};
    aa::app::HandoffTiming timing;
    aa::app::HandoffEligibilityCoordinator coordinator{trust,
        {[this](aa::app::HandoffOperations::Validation request) -> aa::core::Result<aa::app::HandoffObservation> {
            const bool initial = !request.handle;
            if (initial) ++initial_observations;
            auto observed = peer; observed.handle = observed_handle.load();
            const bool live_at_start = helper_live.load();
            const bool delayed = stalled.load();
            if (!initial) sampled = true;
            if (initial && slow_setup.load()) std::this_thread::sleep_for(1100ms);
            if (delayed) std::this_thread::sleep_for(1200ms);
            if (!initial && slow_refresh.load()) std::this_thread::sleep_for(850ms);
            if (!initial && moderate_refresh.load()) std::this_thread::sleep_for(350ms);
            if (!(early_sample.load() ? live_at_start : helper_live.load())) return aa::Error{aa::ErrorCode::helper_rejected};
            return aa::app::HandoffObservation{observed, request.credentials_allowed ? std::optional{snapshot} : std::nullopt};
        }, [this](aa::helper::Source, std::uint64_t handle, bool allow) -> aa::core::Result<void> {
            if (allow) {
                ordering_ok = listening; authorizing = true;
                while (authorize_wait.load()) std::this_thread::yield();
                if (moderate_ops.load()) std::this_thread::sleep_for(350ms);
                ++allowed;
            } else {
                if (std::this_thread::get_id() != owner) ++denial_workers;
                ++denial_attempts;
                EXPECT_EQ(denials_in_flight.fetch_add(1), 0U);
                while (deny_wait.load()) std::this_thread::yield();
                --denials_in_flight;
                if (fail_next_denial.exchange(false)) return aa::Error{aa::ErrorCode::timeout};
                if (reject_denials) return aa::Error{aa::ErrorCode::helper_rejected};
                denied_handle = handle;
                if (replacement_after_denial) observed_handle = replacement_after_denial;
                ++denied;
            }
            return {};
        }, [this](aa::helper::Source, std::uint64_t) -> aa::core::Result<void> {
            ++withdrawn;
            if (moderate_ops.load()) std::this_thread::sleep_for(350ms);
            if (!bootstrap_complete.load()) return aa::Error{aa::ErrorCode::helper_rejected};
            return {};
        }},
        {[this](const aa::helper::ApSnapshot&) -> aa::core::Result<void> { listening = true; return {}; },
         [this] { listening = false; }}, timing};
    explicit EligibilityFixture(aa::app::HandoffTiming bounds = {}) : timing(bounds) {
        trust.set(identity, aa::trust::Decision::approved);
    }
    ~EligibilityFixture() { authorize_wait = false; deny_wait = false; }
    bool eventually(const std::function<bool()>& predicate, std::chrono::milliseconds budget = 500ms,
                    bool active = false) {
        const auto until = real_eligibility_now() + budget;
        do {
            coordinator.tick(active);
            if (predicate()) return true;
            std::this_thread::sleep_for(5ms);
        } while (real_eligibility_now() < until);
        return false;
    }
    void start() { coordinator.consumer(aa::core::ConsumerId{7}); coordinator.begin(source); }
    bool confirm() {
        coordinator.confirm_completion();
        return eventually([this] { return coordinator.bootstrap_completed(); });
    }
};
TEST(WirelessEligibility, ListenerReadyBeforeAuthorizationAndReconnectNeedsNoAddPhone) {
    EligibilityFixture f;
    f.start();
    ASSERT_TRUE(f.eventually([&] { return f.coordinator.admitting(); }));
    EXPECT_TRUE(f.ordering_ok);
    EXPECT_EQ(f.allowed.load(), 1U);
}
TEST(WirelessEligibility, InitialPeerAbsenceRetriesWithoutAuthorityOrEpochChange) {
    EligibilityFixture f;
    f.helper_live = false;
    f.start();
    const auto epoch = f.coordinator.epoch();
    ASSERT_TRUE(f.eventually([&] { return f.initial_observations.load() >= 2U; }));
    EXPECT_EQ(f.coordinator.epoch(), epoch);
    EXPECT_FALSE(f.listening);
    EXPECT_EQ(f.allowed.load(), 0U);
    EXPECT_FALSE(f.coordinator.phone().has_value());
    EXPECT_EQ(f.coordinator.snapshot_generation(), 0U);
    EXPECT_FALSE(f.coordinator.admitting());
    EXPECT_FALSE(f.coordinator.valid_session());
    f.helper_live = true;
    ASSERT_TRUE(f.eventually([&] { return f.coordinator.admitting(); }));
    EXPECT_EQ(f.coordinator.epoch(), epoch);
    EXPECT_TRUE(f.ordering_ok);
    EXPECT_EQ(f.allowed.load(), 1U);
}
TEST(WirelessEligibility, HelperLossClosesWithinOneSecondAndCannotRevive) {
    EligibilityFixture f;
    f.start();
    ASSERT_TRUE(f.eventually([&] { return f.coordinator.admitting(); }));
    const auto began = std::chrono::steady_clock::now();
    f.helper_live = false;
    ASSERT_TRUE(f.eventually([&] { return !f.listening; }, 1s));
    EXPECT_LT(std::chrono::steady_clock::now() - began, 1s);
    f.helper_live = true;
    EXPECT_FALSE(f.coordinator.admitting());
}
TEST(WirelessEligibility, OwnerExpiryDiscardsLateResults) {
    EligibilityFixture f;
    f.start();
    ASSERT_TRUE(f.eventually([&] { return f.coordinator.admitting(); }));
    f.stalled = true;
    const auto epoch = f.coordinator.epoch();
    ASSERT_TRUE(f.eventually([&] { return f.coordinator.epoch() != epoch; }, 1s));
    std::this_thread::sleep_for(500ms);
    f.coordinator.tick();
    EXPECT_FALSE(f.listening);
    EXPECT_EQ(f.allowed.load(), 1U);
}
TEST(WirelessEligibility, TrustRevocationClosesAdmissionImmediately) {
    EligibilityFixture f;
    f.start();
    ASSERT_TRUE(f.eventually([&] { return f.coordinator.admitting(); }));
    f.trust.set(f.identity, aa::trust::Decision::rejected);
    f.coordinator.tick();
    EXPECT_FALSE(f.listening);
    EXPECT_FALSE(f.coordinator.valid_session());
}
TEST(WirelessEligibility, ConsumerLossClosesAdmissionButPreservesActiveAuthority) {
    EligibilityFixture f;
    f.start();
    ASSERT_TRUE(f.eventually([&] { return f.coordinator.admitting(); }));
    ASSERT_TRUE(f.confirm());
    f.coordinator.tick(true);
    f.coordinator.consumer(std::nullopt);
    f.coordinator.tick(true);
    EXPECT_FALSE(f.listening);
    EXPECT_FALSE(f.coordinator.admitting());
    EXPECT_TRUE(f.coordinator.valid_session());
    EXPECT_EQ(f.withdrawn.load(), 1U);
}
TEST(WirelessEligibility, ConfirmingIncompleteBootstrapRetiresBeforeActive) {
    EligibilityFixture f;
    f.bootstrap_complete = false;
    f.start();
    ASSERT_TRUE(f.eventually([&] { return f.coordinator.admitting(); }));
    f.coordinator.confirm_completion();
    ASSERT_TRUE(f.eventually([&] { return !f.coordinator.valid_session(); }, 1s));
    EXPECT_FALSE(f.coordinator.bootstrap_completed());
    EXPECT_GT(f.withdrawn.load(), 0U);
    EXPECT_FALSE(f.listening);
}
TEST(WirelessEligibility, UnconfirmedActiveSessionGetsNoConsumerGrace) {
    EligibilityFixture f;
    f.start();
    ASSERT_TRUE(f.eventually([&] { return f.coordinator.admitting(); }));
    f.coordinator.tick(true);
    f.coordinator.consumer(std::nullopt);
    EXPECT_FALSE(f.coordinator.valid_session());
    EXPECT_EQ(f.withdrawn.load(), 0U);
}
TEST(WirelessEligibility, ConsumerGraceLastsTenSecondsWithoutFreshCredentials) {
    EligibilityFixture f;
    f.start();
    ASSERT_TRUE(f.eventually([&] { return f.coordinator.admitting(); }));
    ASSERT_TRUE(f.confirm());
    f.coordinator.tick(true);
    const auto began = std::chrono::steady_clock::now();
    f.coordinator.consumer(std::nullopt);
    bool retained_at_nine_seconds = false;
    while (std::chrono::steady_clock::now() - began < 11s) {
        f.coordinator.tick(true);
        if (std::chrono::steady_clock::now() - began >= 9s)
            retained_at_nine_seconds = retained_at_nine_seconds || f.coordinator.valid_session();
        if (f.coordinator.epoch() > 2) break;
        std::this_thread::sleep_for(5ms);
    }
    EXPECT_TRUE(retained_at_nine_seconds);
    EXPECT_FALSE(f.coordinator.valid_session());
    EXPECT_FALSE(f.listening);
    EXPECT_EQ(f.allowed.load(), 1U);
    EXPECT_LT(std::chrono::steady_clock::now() - began, 11s);
}
TEST(WirelessEligibility, SlowHealthyLateSamplingCannotExtendConservativeEvidence) {
    EligibilityFixture f;
    f.start();
    ASSERT_TRUE(f.eventually([&] { return f.coordinator.admitting(); }));
    f.slow_refresh = true;
    EXPECT_TRUE(f.eventually([&] { return !f.listening; }, 1s));
}
TEST(WirelessEligibility, HealthyEstablishmentMayExceedOneSecond) {
    EligibilityFixture f;
    f.slow_setup = true;
    f.start();
    EXPECT_TRUE(f.eventually([&] { return f.coordinator.admitting(); }, 2s));
}
TEST(WirelessEligibility, HealthyLateSamplingWithinSerialRefreshBudgetStaysEligible) {
    EligibilityFixture f;
    f.start();
    ASSERT_TRUE(f.eventually([&] { return f.coordinator.admitting(); }));
    f.moderate_refresh = true;
    const auto until = std::chrono::steady_clock::now() + 1200ms;
    while (std::chrono::steady_clock::now() < until) {
        f.coordinator.tick();
        ASSERT_TRUE(f.coordinator.admitting());
        std::this_thread::sleep_for(5ms);
    }
}
void serialized_validation_preserves_freshness(bool confirming) {
    // Given 350ms refreshes with a 350ms validating authorize or completed confirm between them.
    EligibilityFixture f;
    f.moderate_refresh = !confirming; f.moderate_ops = !confirming;
    f.start();
    ASSERT_TRUE(f.eventually([&] { return f.coordinator.admitting(); }, 2s));
    if (confirming) { f.moderate_refresh = true; f.moderate_ops = true; f.coordinator.confirm_completion(); }
    const auto epoch = f.coordinator.epoch();
    // When three serialized operations exceed one 950ms evidence window.
    const auto until = std::chrono::steady_clock::now() + 1500ms;
    while (std::chrono::steady_clock::now() < until) {
        f.coordinator.tick();
        // Then each successful validation renews from its request start and eligibility never lapses.
        ASSERT_TRUE(f.coordinator.valid_session());
        ASSERT_EQ(f.coordinator.epoch(), epoch);
        std::this_thread::sleep_for(5ms);
    }
    EXPECT_EQ(f.coordinator.bootstrap_completed(), confirming);
    EXPECT_EQ(f.denied.load(), 0U);
}
TEST(WirelessEligibility, SerializedAuthorizePreservesFreshness) { serialized_validation_preserves_freshness(false); }
TEST(WirelessEligibility, SerializedCompletedConfirmPreservesFreshness) { serialized_validation_preserves_freshness(true); }
TEST(WirelessEligibility, EarlySampleDeliveredLateDoesNotRenewFromReplyTime) {
    EligibilityFixture f;
    f.start();
    ASSERT_TRUE(f.eventually([&] { return f.coordinator.admitting(); }));
    f.early_sample = true; f.slow_refresh = true; f.sampled = false;
    ASSERT_TRUE(f.eventually([&] { return f.sampled.load(); }));
    const auto mutation = std::chrono::steady_clock::now();
    f.helper_live = false;
    ASSERT_TRUE(f.eventually([&] { return !f.listening; }, 1s));
    EXPECT_LT(std::chrono::steady_clock::now() - mutation, 1s);
}
TEST(WirelessEligibility, ConsumerLossDuringAuthorizeExplicitlyRetiresRemoteEffect) {
    EligibilityFixture f;
    f.authorize_wait = true;
    f.start();
    ASSERT_TRUE(f.eventually([&] { return f.authorizing.load(); }));
    f.coordinator.consumer(std::nullopt);
    EXPECT_FALSE(f.listening);
    f.authorize_wait = false;
    ASSERT_TRUE(f.eventually([&] { return f.denied.load() > 0; }));
    EXPECT_FALSE(f.coordinator.admitting());
}
// #161: a retry (ReconnectPeerV1, then begin with the same source) during a live bootstrap
// must keep it: no retirement, no denial, and no retired_handle poisoning its re-observation.
TEST(WirelessEligibility, RetryDuringAuthorizationKeepsTheBootstrap) {
    EligibilityFixture f;
    f.authorize_wait = true;
    f.start();
    ASSERT_TRUE(f.eventually([&] { return f.authorizing.load(); }));
    const auto epoch = f.coordinator.epoch();
    f.coordinator.begin(f.source);
    EXPECT_EQ(f.coordinator.epoch(), epoch);
    EXPECT_TRUE(f.listening);
    f.authorize_wait = false;
    ASSERT_TRUE(f.eventually([&] { return f.coordinator.admitting(); }));
    EXPECT_EQ(f.denied.load(), 0U);
    EXPECT_EQ(f.allowed.load(), 1U);
}
TEST(WirelessEligibility, RetryBeforeCompletionConfirmedKeepsTheBootstrap) {
    EligibilityFixture f;
    f.start();
    ASSERT_TRUE(f.eventually([&] { return f.coordinator.admitting(); }));
    const auto epoch = f.coordinator.epoch();
    f.coordinator.begin(f.source);
    // Keep ticking for a while: the retry must neither retire nor re-observe.
    const auto until = std::chrono::steady_clock::now() + 100ms;
    while (std::chrono::steady_clock::now() < until) {
        f.coordinator.tick();
        ASSERT_TRUE(f.coordinator.admitting());
        std::this_thread::sleep_for(5ms);
    }
    EXPECT_EQ(f.coordinator.epoch(), epoch);
    EXPECT_EQ(f.denied.load(), 0U);
    EXPECT_EQ(f.initial_observations.load(), 1U) << "the live bootstrap is not observed again";
    ASSERT_TRUE(f.confirm());
}
TEST(WirelessEligibility, BootstrapObservedLateRunsToItsOwnBudget) {
    // Given: the phone connects just before the establishment bound.
    EligibilityFixture f{{std::chrono::milliseconds{300}, std::chrono::milliseconds{600}, std::chrono::milliseconds{200}}};
    f.helper_live = false; f.authorize_wait = true;
    f.start();
    const auto began = std::chrono::steady_clock::now();
    f.eventually([] { return false; }, 220ms);
    f.helper_live = true;
    ASSERT_TRUE(f.eventually([&] { return f.authorizing.load(); }));
    // When: establishment passes while the bootstrap is still authorizing.
    while (std::chrono::steady_clock::now() - began < 450ms) {
        f.coordinator.tick();
        // Then: it is not retired before its own budget.
        ASSERT_TRUE(f.coordinator.bootstrapping());
        ASSERT_EQ(f.denied.load(), 0U);
        std::this_thread::sleep_for(5ms);
    }
    // And: a bootstrap that never progresses still ends at its own budget (600 ms here).
    ASSERT_TRUE(f.eventually([&] { return !f.coordinator.bootstrapping(); }, 1s));
    EXPECT_GE(std::chrono::steady_clock::now() - began, 750ms);
    EXPECT_FALSE(f.listening);
    f.authorize_wait = false;
    ASSERT_TRUE(f.eventually([&] { return f.denied.load() > 0; }));
    EXPECT_FALSE(f.coordinator.admitting());
}
TEST(WirelessEligibility, UnconfirmedAuthorizedHoldIsBounded) {
    // A completed exchange whose phone never opens the TCP session: the attempt may be
    // deferred once the budget and the connect grace have passed.
    EligibilityFixture f{{std::chrono::milliseconds{10'000}, std::chrono::milliseconds{200}, std::chrono::milliseconds{200}}};
    f.start();
    ASSERT_TRUE(f.eventually([&] { return f.coordinator.admitting(); }));
    EXPECT_TRUE(f.coordinator.bootstrapping());
    EXPECT_TRUE(f.eventually([&] { return !f.coordinator.bootstrapping(); }, 1s));
    EXPECT_TRUE(f.coordinator.admitting()) << "evidence stays fresh; only the hold ends";
    ASSERT_TRUE(f.confirm());
    EXPECT_FALSE(f.coordinator.bootstrapping());
}
TEST(WirelessEligibility, LateOldCompletionCannotReopenOrRetireReplacement) {
    EligibilityFixture f;
    f.start();
    ASSERT_TRUE(f.eventually([&] { return f.coordinator.admitting(); }));
    f.stalled = true; f.sampled = false;
    ASSERT_TRUE(f.eventually([&] { return f.sampled.load(); }));
    f.coordinator.consumer(aa::core::ConsumerId{8});
    EXPECT_FALSE(f.listening);
    f.observed_handle = 42; f.stalled = false;
    f.coordinator.begin({8, 0});
    EXPECT_FALSE(f.coordinator.admitting());
    ASSERT_TRUE(f.eventually([&] { return f.coordinator.admitting(); }, 2s));
    EXPECT_EQ(f.allowed.load(), 2U);
    EXPECT_GT(f.denied.load(), 0U);
}
}

TEST(WirelessEligibility, AdmissionExpiryEventNamesThePhaseWithoutIdentities) {
    using aa::wireless_receiver::admission_expired_json;
    EXPECT_EQ(admission_expired_json(true, true),
        R"json({"event":"wireless_admission_expired","reason":"attempt_deadline","phase":"ap_activation"})json");
    EXPECT_EQ(admission_expired_json(false, true),
        R"json({"event":"wireless_admission_expired","reason":"no_bluetooth_link","phase":"bluetooth_link",)json"
        R"json("detail":"waiting for HFP (owned by infotainment)"})json");
    EXPECT_EQ(admission_expired_json(false, false),
        R"json({"event":"wireless_admission_expired","reason":"attempt_deadline","phase":"phone_handoff"})json");
}
// #169 review (Neon P2): the helper binds the phone's bootstrap to this attempt's source inside
// PollBootstrapV1, before the observation's reply reaches the coordinator. An attempt retired in
// that gap must still deny the handle the observation bound; otherwise the helper keeps the live
// bootstrap, its binding and its lease into the next attempt's ReconnectPeerV1.
TEST(WirelessEligibility, RetireWithAnObservationInFlightDeniesTheHandleItBound) {
    EligibilityFixture f;
    f.slow_setup = true;
    f.start();
    ASSERT_TRUE(f.eventually([&] { return f.initial_observations.load() > 0; }));
    // Runtime defer() -> receiver.stop(): the external retirement settles before it returns.
    f.coordinator.retire();
    EXPECT_EQ(f.denied.load(), 1U) << "retire() returned without denying the observed handle";
    EXPECT_EQ(f.denied_handle.load(), f.peer.handle);
}
TEST(WirelessEligibility, TickRetriesADenialThatTimedOutDuringRetire) {
    VirtualClock clock;
    EligibilityFixture f;
    f.start();
    ASSERT_TRUE(f.eventually([&] { return f.coordinator.admitting(); }));
    f.fail_next_denial = true;
    f.coordinator.retire();
    ASSERT_EQ(f.denial_attempts.load(), 1U);
    ASSERT_EQ(f.denied.load(), 0U);
    unsigned attempts = 1;
    for (const auto delay : {100ms, 200ms, 400ms, 800ms, 1000ms}) {
        f.fail_next_denial = true;
        clock.advance(delay - 1ms);
        f.coordinator.tick();
        f.coordinator.retire(Retirement::respect_backoff);
        EXPECT_EQ(f.denial_attempts.load(), attempts);
        clock.advance(1ms);
        f.coordinator.tick();
        f.coordinator.retire(Retirement::respect_backoff);
        EXPECT_EQ(f.denial_attempts.load(), ++attempts);
    }
    clock.advance(999ms);
    f.coordinator.tick();
    f.coordinator.retire(Retirement::respect_backoff);
    EXPECT_EQ(f.denial_attempts.load(), attempts);
    clock.advance(1ms);
    EXPECT_TRUE(f.eventually([&] { return f.denied.load() == 1U; }))
        << "tick did not retry the retired handoff's denial";
    EXPECT_EQ(f.denial_attempts.load(), ++attempts);
    EXPECT_EQ(f.denied_handle.load(), f.peer.handle);
    f.coordinator.retire();
    EXPECT_EQ(f.denial_attempts.load(), attempts) << "a successful denial must not be retried";
}
TEST(WirelessEligibility, TickRetriesADenialThatTimedOutDuringTick) {
    VirtualClock clock;
    EligibilityFixture f;
    f.start();
    ASSERT_TRUE(f.eventually([&] { return f.coordinator.admitting(); }));
    f.fail_next_denial = true;
    f.coordinator.consumer(std::nullopt);
    ASSERT_TRUE(f.eventually([&] { return f.denial_attempts.load() == 1U; }));
    f.coordinator.retire(Retirement::respect_backoff);
    ASSERT_EQ(f.denial_attempts.load(), 1U);
    clock.advance(100ms);
    EXPECT_TRUE(f.eventually([&] { return f.denied.load() == 1U; }))
        << "tick dropped the failed asynchronous denial";
    EXPECT_EQ(f.denial_attempts.load(), 2U);
    EXPECT_EQ(f.denied_handle.load(), f.peer.handle);
    f.coordinator.retire();
    EXPECT_EQ(f.denial_attempts.load(), 2U);
}
TEST(WirelessEligibility, SustainedRejectionParksTheDenial) {
    VirtualClock clock;
    EligibilityFixture f;
    f.start();
    ASSERT_TRUE(f.eventually([&] { return f.coordinator.admitting(); }));
    f.reject_denials = true;
    f.deny_wait = true;
    f.coordinator.consumer(std::nullopt);
    ASSERT_TRUE(f.eventually([&] { return f.denials_in_flight.load() == 1U; }));
    for (int tick = 0; tick < 100; ++tick) {
        clock.advance(25ms);
        f.coordinator.tick();
    }
    EXPECT_EQ(f.denial_workers.load(), 1U);
    f.deny_wait = false;
    f.coordinator.retire(Retirement::respect_backoff);
    clock.advance(99ms);
    f.coordinator.tick();
    f.coordinator.retire(Retirement::respect_backoff);
    EXPECT_EQ(f.denial_attempts.load(), 1U);
    clock.advance(1ms);
    f.coordinator.tick();
    f.coordinator.retire(Retirement::respect_backoff);
    ASSERT_GE(f.denial_attempts.load(), 2U);
    for (int tick = 0; tick < 250; ++tick) {
        clock.advance(1s);
        f.coordinator.tick();
        f.coordinator.retire(Retirement::respect_backoff);
    }
    EXPECT_EQ(f.denial_attempts.load(), 2U);
    EXPECT_EQ(f.denial_workers.load(), 2U);
    EXPECT_EQ(f.denials_in_flight.load(), 0U);
    EXPECT_FALSE(f.coordinator.admitting());

    // Reconnect settlement also leaves a parked denial alone. A successful poll
    // for the same binding proves the helper can still act on it.
    f.coordinator.retire();
    ASSERT_EQ(f.denial_attempts.load(), 2U);
    f.replacement_after_denial = 42;
    f.reject_denials = false;
    f.start();
    ASSERT_TRUE(f.eventually([&] { return f.coordinator.admitting(); }));
    EXPECT_EQ(f.denial_attempts.load(), 3U);
    EXPECT_EQ(f.denied_handle.load(), f.peer.handle);
}
TEST(WirelessEligibility, ParkedDenialReactivatesAtMostOncePerAttempt) {
    VirtualClock clock;
    EligibilityFixture f;
    f.start();
    ASSERT_TRUE(f.eventually([&] { return f.coordinator.admitting(); }));
    f.reject_denials = true;
    f.coordinator.retire();
    f.coordinator.retire();
    ASSERT_EQ(f.denial_attempts.load(), 2U);
    ASSERT_EQ(f.denial_workers.load(), 0U);

    for (unsigned attempt = 1; attempt <= 2; ++attempt) {
        SCOPED_TRACE(attempt);
        const auto prior_observations = f.initial_observations.load();
        const auto prior_denials = f.denial_attempts.load();
        const auto prior_workers = f.denial_workers.load();
        f.start();
        const auto attempt_epoch = f.coordinator.epoch();
        ASSERT_TRUE(f.eventually([&] {
            if (f.coordinator.epoch() != attempt_epoch || f.denial_attempts.load() > prior_denials + 2)
                return true;
            clock.advance(25ms);
            return false;
        }));
        ASSERT_NE(f.coordinator.epoch(), attempt_epoch) << "the stale handle kept the attempt polling";
        EXPECT_EQ(f.initial_observations.load(), prior_observations + 2);
        EXPECT_EQ(f.denial_attempts.load(), prior_denials + 2);
        EXPECT_EQ(f.denial_workers.load(), prior_workers + 2);
        EXPECT_EQ(f.denials_in_flight.load(), 0U);
        EXPECT_FALSE(f.coordinator.admitting());
    }
}
TEST(WirelessEligibility, NewObservationSupersedesAnOutstandingDenial) {
    VirtualClock clock;
    EligibilityFixture f;
    f.start();
    ASSERT_TRUE(f.eventually([&] { return f.coordinator.admitting(); }));
    f.fail_next_denial = true;
    f.deny_wait = true;
    f.coordinator.consumer(std::nullopt);
    ASSERT_TRUE(f.eventually([&] { return f.denials_in_flight.load() == 1U; }));
    f.observed_handle = 42;
    f.start();
    for (int tick = 0; tick < 50; ++tick) f.coordinator.tick();
    EXPECT_EQ(f.initial_observations.load(), 1U);
    EXPECT_EQ(f.denial_workers.load(), 1U);
    f.deny_wait = false;
    ASSERT_TRUE(f.eventually([&] { return f.coordinator.admitting(); }));
    EXPECT_EQ(f.denial_attempts.load(), 1U);
    f.coordinator.retire();
    EXPECT_EQ(f.denial_attempts.load(), 2U);
    EXPECT_EQ(f.denied_handle.load(), 42U);
}
TEST(WirelessEligibility, SameSourceAttemptSurvivesObservingTheRetiredHandleDuringBackoff) {
    VirtualClock clock;
    EligibilityFixture f;
    f.replacement_after_denial = 42;
    f.start();
    ASSERT_TRUE(f.eventually([&] { return f.coordinator.admitting(); }));
    f.fail_next_denial = true;
    f.coordinator.retire();
    ASSERT_EQ(f.denial_attempts.load(), 1U);
    f.start();
    const auto epoch = f.coordinator.epoch();
    ASSERT_TRUE(f.eventually([&] { return f.initial_observations.load() >= 2U; }));
    // Drive the old handle's reply while time is frozen inside the retry window.
    EXPECT_FALSE(f.eventually([&] { return f.coordinator.epoch() != epoch; }, 100ms))
        << "observing the old handle retired the replacement attempt";
    EXPECT_EQ(f.denial_attempts.load(), 1U);
    EXPECT_EQ(f.initial_observations.load(), 2U);
    clock.advance(100ms);
    ASSERT_TRUE(f.eventually([&] { return f.denied.load() == 1U; }));
    EXPECT_EQ(f.denied_handle.load(), f.peer.handle);
    EXPECT_TRUE(f.eventually([&] { return f.coordinator.admitting(); }));
    EXPECT_EQ(f.coordinator.epoch(), epoch);
}
TEST(WirelessEligibility, EstablishmentExpiryWithAnObservationInFlightDeniesTheHandleItBound) {
    EligibilityFixture f{{std::chrono::milliseconds{100}, std::chrono::milliseconds{10'000}, std::chrono::milliseconds{5'000}}};
    f.slow_setup = true;
    f.start();
    ASSERT_TRUE(f.eventually([&] { return f.initial_observations.load() > 0; }));
    // The establishment bound retires the attempt while the observation is still in flight; its
    // stale reply then names the handle the helper bound.
    EXPECT_TRUE(f.eventually([&] { return f.denied.load() > 0; }, 2s)) << "the stale observation was dropped";
    EXPECT_EQ(f.denied_handle.load(), f.peer.handle);
    EXPECT_FALSE(f.coordinator.admitting());
}
