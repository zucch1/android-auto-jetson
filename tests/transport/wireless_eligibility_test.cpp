// SPDX-License-Identifier: GPL-3.0-or-later
#include <aa/app/HandoffEligibility.hpp>
#include "../session/Fakes.hpp"
#include <aa/trust/Identity.hpp>
#include <gtest/gtest.h>
#include <atomic>
#include <thread>

using namespace std::chrono_literals;
namespace {
struct EligibilityFixture {
    aa::session::test::MapTrustStore trust;
    const aa::helper::Source source{7, 0};
    const aa::helper::ObservedPeer peer{41, {"12:34:56:78:9A:BC", "12:34:56:78:9A:BD"}};
    aa::helper::ApSnapshot snapshot{{"fixture", "test-password", "12:34:56:78:9a:bd", "127.0.0.1", 5288, 1}};
    std::atomic<bool> helper_live{true};
    std::atomic<bool> stalled{false};
    std::atomic<bool> slow_refresh{false};
    std::atomic<bool> moderate_refresh{false};
    std::atomic<bool> slow_setup{false};
    std::atomic<bool> early_sample{false};
    std::atomic<bool> sampled{false};
    std::atomic<bool> authorize_wait{false}, authorizing{false};
    std::atomic<unsigned> denied{};
    std::atomic<std::uint64_t> observed_handle{41};
    std::atomic<unsigned> allowed{};
    std::atomic<bool> bootstrap_complete{true};
    std::atomic<unsigned> withdrawn{};
    bool listening{};
    bool ordering_ok{true};
    aa::trust::PhoneIdentity identity{aa::trust::identity_key(aa::trust::TransportIdentity{
        aa::trust::WirelessIdentity{peer.identity.address, peer.identity.adapter}})};
    aa::app::HandoffEligibilityCoordinator coordinator{trust,
        {[this](aa::app::HandoffOperations::Validation request) -> aa::core::Result<aa::app::HandoffObservation> {
            const bool initial = !request.handle;
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
        }, [this](aa::helper::Source, std::uint64_t, bool allow) -> aa::core::Result<void> {
            if (allow) {
                ordering_ok = listening; authorizing = true;
                while (authorize_wait.load()) std::this_thread::yield();
                ++allowed;
            } else ++denied;
            return {};
        }, [this](aa::helper::Source, std::uint64_t) -> aa::core::Result<void> {
            ++withdrawn;
            if (!bootstrap_complete.load()) return aa::Error{aa::ErrorCode::helper_rejected};
            return {};
        }},
        {[this](const aa::helper::ApSnapshot&) -> aa::core::Result<void> { listening = true; return {}; },
         [this] { listening = false; }}};
    EligibilityFixture() { trust.set(identity, aa::trust::Decision::approved); }
    ~EligibilityFixture() { authorize_wait = false; }
    bool eventually(const std::function<bool()>& predicate, std::chrono::milliseconds budget = 500ms,
                    bool active = false) {
        const auto until = std::chrono::steady_clock::now() + budget;
        do {
            coordinator.tick(active);
            if (predicate()) return true;
            std::this_thread::sleep_for(5ms);
        } while (std::chrono::steady_clock::now() < until);
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
