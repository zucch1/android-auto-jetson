// SPDX-License-Identifier: GPL-3.0-or-later
// Reuse the existing private-bus NM/BlueZ process fixture without altering its
// approved sources; this executable runs only WirelessComposed via its filter.
// NOLINTNEXTLINE(bugprone-suspicious-include): existing process fixture is private to this translation unit.
#include "../helper/nm_test.cpp"
#include <aa/app/HandoffEligibility.hpp>

namespace aa::helper::nm {
namespace {
class WirelessComposed : public NmDaemon {
protected:
    using Coordinator = app::HandoffEligibilityCoordinator;
    bool listening = false;
    unsigned starts{};
    std::atomic<int> validation_code{-1}, authorization_code{-1};
    std::atomic<long long> validation_ms{}, refresh_max_ms{};
    std::atomic<bool> ready_before_allow{};
    std::atomic<int> withdraw_code{-1};
    // A refresh that is still in flight when the attempt is retired (#169 review).
    std::atomic<bool> slow_refresh{}, refreshing{};
    // An initial observation whose reply is held after the helper bound the hand-off (Neon P2).
    std::atomic<bool> slow_observe{}, observed{};
    // The next GetAPCredentialsV1 fails on the receiver's side, after PollBootstrapV1 bound.
    std::atomic<bool> fail_credentials{};
    std::atomic<bool> fail_next_deny{};
    std::atomic<unsigned> denial_attempts{};
    std::unique_ptr<Coordinator> coordinator;
    Clock::time_point bootstrap_deadline{};
    void TearDown() override {
        coordinator.reset();
        std::cout << "refresh_max_ms=" << refresh_max_ms.load() << '\n';
        NmDaemon::TearDown();
    }
    const trust::TransportIdentity identity{trust::WirelessIdentity{"02:00:00:00:00:01", "02:00:00:00:00:AA"}};
    // acquisition: Runtime's AP-inactive path (reconnect, then AcquireApV1 with this source, which
    // becomes the helper lease and the attempt's source). Otherwise the AP-active {7, 0} source.
    void admit(std::optional<Source> acquisition = std::nullopt, bool until_admitting = true) {
        start(); acquire(); ASSERT_TRUE(state(State::active));
        ASSERT_TRUE(store->approve(identity)); ASSERT_EQ(bluez.command("paired"), "ok");
        ASSERT_TRUE(client->reconnect(identity));
        if (acquisition) { ASSERT_TRUE(client->acquire_ap(*acquisition)); }
        bootstrap_deadline = Clock::now() + 10s;
        ASSERT_EQ(bluez.command("NewConnection"), "allowed");
        auto bus = app::handoff_bus(*client);
        bus.credentials = [this, credentials = std::move(bus.credentials)](Source source, std::uint64_t handle)
            -> core::Result<ApSnapshot> {
            // The request does not reach the helper in budget (BusClient: lock or reply timeout).
            if (fail_credentials.exchange(false)) return Error{ErrorCode::timeout};
            return credentials(source, handle);
        };
        auto operations = app::handoff_operations(std::move(bus));
        auto validate = std::move(operations.validate);
        operations.validate = [this, validate](app::HandoffOperations::Validation request) {
            if (request.handle && slow_refresh.load()) { refreshing = true; std::this_thread::sleep_for(700ms); }
            const auto began = Clock::now();
            auto result = validate(request);
            if (!request.handle && result && slow_observe.load()) { observed = true; std::this_thread::sleep_for(700ms); }
            validation_ms = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - began).count();
            if (request.handle) refresh_max_ms = std::max(refresh_max_ms.load(), validation_ms.load());
            validation_code = result ? 0 : static_cast<int>(result.error().code());
            return result;
        };
        auto authorize = std::move(operations.authorize);
        operations.authorize = [this, authorize](Source source, std::uint64_t handle, bool allow) {
            if (allow) ready_before_allow = listening;
            if (!allow) {
                const bool timeout = fail_next_deny.exchange(false);
                ++denial_attempts;
                if (timeout) return core::Result<void>{Error{ErrorCode::timeout}};
            }
            auto result = authorize(source, handle, allow);
            authorization_code = result ? 0 : static_cast<int>(result.error().code());
            return result;
        };
        auto withdraw = std::move(operations.withdraw);
        operations.withdraw = [this, withdraw](Source source, std::uint64_t handle) {
            auto result = withdraw(source, handle);
            withdraw_code = result ? 0 : static_cast<int>(result.error().code());
            return result;
        };
        coordinator = std::make_unique<Coordinator>(*store, std::move(operations), Coordinator::Listener{
            [this](const ApSnapshot& snapshot) -> core::Result<void> {
                EXPECT_EQ(snapshot.ip, "10.42.0.1"); EXPECT_EQ(snapshot.port, 5277U);
                listening = true; ++starts; return {};
            }, [this] { listening = false; }});
        coordinator->consumer(core::ConsumerId{7}); coordinator->begin(acquisition.value_or(Source{7, 0}));
        if (!until_admitting) return;
        // v2.5 supersedes the unsupported 1s setup SLA; loss cancellation stays <=1s.
        ASSERT_TRUE(eventually([&] { coordinator->tick(); return coordinator->admitting(); },
            std::chrono::duration_cast<std::chrono::milliseconds>(bootstrap_deadline - Clock::now())))
            << "validation=" << validation_code.load() << " latency_ms=" << validation_ms.load()
            << " authorization=" << authorization_code.load() << " starts=" << starts << " epoch=" << coordinator->epoch();
        EXPECT_TRUE(ready_before_allow); EXPECT_EQ(starts, 1U);
        EXPECT_LT(Clock::now(), bootstrap_deadline);
    }
    void complete_bootstrap() {
        auto phone = std::async(std::launch::async, [&] {
            ASSERT_TRUE(eventually([&] { return bluez.command("wire-read") != "open"; }));
            ASSERT_EQ(bluez.command("wire-send 00000002"), "ok");
            ASSERT_TRUE(eventually([&] { return bluez.command("wire-read") != "open"; }));
            ASSERT_EQ(bluez.command("wire-send 000200071800000200060800"), "ok");
            // Success: BlueZ keeps its reference, so the channel stays open (#161). The helper
            // takes one record per pump; stay quiet ~1 s so completion has landed.
            for (int i = 0; i < 5; ++i) { EXPECT_EQ(bluez.command("wire-read"), "open"); }
        });
        ASSERT_TRUE(eventually([&] { coordinator->tick(); return phone.wait_for(0ms) == std::future_status::ready; }, 5s));
        phone.get();
    }
};
#include "composed_matrix.inc"
TEST_F(WirelessComposed, CoordinatorUsesRealCompletedAuthorityAndPinsApOnApprovedReconnect) {
    ASSERT_NO_FATAL_FAILURE(admit());
    ASSERT_NO_FATAL_FAILURE(complete_bootstrap());
    const auto healthy_until = Clock::now() + 1s;
    while (Clock::now() < healthy_until) {
        coordinator->tick();
        ASSERT_TRUE(coordinator->admitting()) << "validation_ms=" << validation_ms.load();
        std::this_thread::sleep_for(10ms);
    }
    const auto before = coordinator->epoch();
    ASSERT_TRUE(eventually([&] { coordinator->tick(true); return coordinator->valid_session(); }, 500ms))
        << "validation=" << validation_code.load() << " latency_ms=" << validation_ms.load()
        << " authorization=" << authorization_code.load() << " epoch=" << coordinator->epoch();
    const auto began = Clock::now();
    nm->changed_mac = true;
    ASSERT_TRUE(eventually([&] { coordinator->tick(true); return !listening; }, 1s));
    EXPECT_LT(Clock::now() - began, 1s);
    std::cout << "ap_loss_close_ms="
        << std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - began).count() << '\n';
    EXPECT_GT(coordinator->epoch(), before);
    nm->changed_mac = false;
    coordinator->tick(true);
    EXPECT_FALSE(coordinator->admitting());
    EXPECT_EQ(store->phones().size(), 1U);
}
TEST_F(WirelessComposed, ActivationWithWithheldBootstrapRetiresBeforeCredentialRelease) {
    ASSERT_NO_FATAL_FAILURE(admit());
    ASSERT_TRUE(eventually([&] { coordinator->tick(); return bluez.command("wire-read") != "open"; }));
    coordinator->confirm_completion();
    ASSERT_TRUE(eventually([&] { coordinator->tick(); return !coordinator->valid_session(); }, 1s))
        << "withdraw=" << withdraw_code.load();
    EXPECT_FALSE(coordinator->bootstrap_completed());
    EXPECT_NE(withdraw_code.load(), 0);
    std::string wire;
    ASSERT_TRUE(eventually([&] { wire = bluez.command("wire-read"); return wire != "open"; }));
    EXPECT_EQ(wire, "closed");
    HandoffFailure failure{};
    EXPECT_FALSE(client->poll_bootstrap({7, 0}, failure));
}
TEST_F(WirelessComposed, ConsumerLossAfterCompletedBootstrapKeepsActiveGrace) {
    ASSERT_NO_FATAL_FAILURE(admit());
    ASSERT_NO_FATAL_FAILURE(complete_bootstrap());
    coordinator->confirm_completion();
    ASSERT_TRUE(eventually([&] { coordinator->tick(); return coordinator->bootstrap_completed(); }, 2s))
        << "withdraw=" << withdraw_code.load();
    EXPECT_EQ(withdraw_code.load(), 0);
    coordinator->tick(true);
    coordinator->consumer(std::nullopt);
    EXPECT_FALSE(listening);
    EXPECT_EQ(bluez.command("wire-read"), "open");
    const auto until = Clock::now() + 1500ms;
    while (Clock::now() < until) {
        coordinator->tick(true);
        ASSERT_TRUE(coordinator->valid_session()) << "validation=" << validation_code.load();
        EXPECT_FALSE(coordinator->admitting());
        std::this_thread::sleep_for(10ms);
    }
}

// #169 review: an expired attempt's completed, unconfirmed bootstrap is retired (Runtime defer()
// -> receiver.stop()) while a refresh is still in flight, so its denial cannot be sent at once.
// The next admission's ReconnectPeerV1 must not find that bootstrap still valid: the helper
// would keep its binding and the acquisition lease {7, 1}, and the fresh AP-active attempt's
// {7, 0} polls would be rejected even after the late denial retired the old hand-off.
TEST_F(WirelessComposed, ExpiredHandoffDeniedLateLeavesNoLeaseForTheFreshAttempt) {
    ASSERT_NO_FATAL_FAILURE(admit(Source{7, 1}));
    ASSERT_NO_FATAL_FAILURE(complete_bootstrap());
    slow_refresh = true;
    ASSERT_TRUE(eventually([&] { coordinator->tick(); return refreshing.load(); }, 2s));
    // Given: Runtime defers the expired attempt with that refresh still running.
    coordinator->retire();
    // When: the next admit() reconnects and begins the AP-active source.
    ASSERT_TRUE(client->reconnect(identity));
    coordinator->begin({7, 0});
    slow_refresh = false;
    // Then: the old phone channel is shut down, and the phone's fresh bootstrap is admitted
    // under the new source within the establishment bound.
    ASSERT_TRUE(eventually([&] { coordinator->tick(); return bluez.command("wire-read") != "open"; }, 3s));
    ASSERT_EQ(bluez.command("NewConnection"), "allowed");
    EXPECT_TRUE(eventually([&] { coordinator->tick(); return coordinator->admitting(); }, 5s))
        << "validation=" << validation_code.load() << " authorization=" << authorization_code.load();
    HandoffFailure failure{};
    EXPECT_TRUE(client->poll_bootstrap({7, 0}, failure)) << "the retired attempt's lease carried over";
}

TEST_F(WirelessComposed, InFlightDenialTimeoutIsRetriedBeforeReconnect) {
    ASSERT_NO_FATAL_FAILURE(admit(Source{7, 1}));
    ASSERT_NO_FATAL_FAILURE(complete_bootstrap());
    fail_next_deny = true;
    coordinator->consumer(std::nullopt);
    ASSERT_TRUE(eventually([&] { coordinator->tick(); return denial_attempts.load() == 1; }));

    coordinator->retire();
    EXPECT_EQ(denial_attempts.load(), 2U) << "the denial should be retried before reconnect";
    EXPECT_EQ(bluez.command("wire-read"), "closed");
    ASSERT_TRUE(client->reconnect(identity));
    EXPECT_TRUE(client->bootstrap_wait({8, 0})) << "reconnect kept the old lease";
}

TEST_F(WirelessComposed, TickRetriesTimedOutBootstrapDenial) {
    const Source old_source{7, 1};
    const Source fresh_source{8, 0};
    ASSERT_NO_FATAL_FAILURE(admit(old_source));
    ASSERT_NO_FATAL_FAILURE(complete_bootstrap());
    ASSERT_FALSE(client->bootstrap_wait(fresh_source));

    authorization_code = -1;
    fail_next_deny = true;
    coordinator->consumer(std::nullopt);
    ASSERT_TRUE(eventually([&] {
        coordinator->tick();
        return denial_attempts.load() >= 2U && authorization_code.load() == 0;
    }, 3s));
    EXPECT_EQ(denial_attempts.load(), 2U);
    EXPECT_EQ(bluez.command("wire-read"), "closed");
    ASSERT_TRUE(client->reconnect(identity));
    EXPECT_TRUE(client->bootstrap_wait(fresh_source));
}

// #169 review (Neon P2, variant of the above): the attempt is retired while its initial
// observation is in flight, after PollBootstrapV1 bound the phone's live bootstrap to {7, 1}.
// Its reply must not be dropped: the bound handle is denied before the next ReconnectPeerV1,
// which would otherwise keep the bootstrap, its binding and the {7, 1} lease.
TEST_F(WirelessComposed, RetiredObservationInFlightLeavesNoLeaseForTheFreshAttempt) {
    slow_observe = true;
    ASSERT_NO_FATAL_FAILURE(admit(Source{7, 1}, false));
    ASSERT_TRUE(eventually([&] { coordinator->tick(); return observed.load(); }, 3s));
    coordinator->retire();
    slow_observe = false;
    ASSERT_TRUE(client->reconnect(identity));
    coordinator->begin({7, 0});
    ASSERT_TRUE(eventually([&] { coordinator->tick(); return bluez.command("wire-read") != "open"; }, 3s))
        << "the bootstrap bound by the dropped observation is still live";
    ASSERT_EQ(bluez.command("NewConnection"), "allowed");
    EXPECT_TRUE(eventually([&] { coordinator->tick(); return coordinator->admitting(); }, 5s))
        << "validation=" << validation_code.load() << " authorization=" << authorization_code.load();
    HandoffFailure failure{};
    EXPECT_TRUE(client->poll_bootstrap({7, 0}, failure)) << "the retired attempt's lease carried over";
}

// #169 review (zucch1-bot, HandoffEligibility.cpp:130): the initial observation's PollBootstrapV1
// bound the phone's live bootstrap, and the lease, to {7, 1}; its GetAPCredentialsV1 then failed on
// the receiver's side (timeout: the helper retired nothing). The error reply names no handle and the
// attempt retires without one. The bound handle must still be denied, or the next ReconnectPeerV1
// keeps the bootstrap, its binding and the {7, 1} lease, and the fresh {7, 0} attempt is rejected.
TEST_F(WirelessComposed, FailedInitialObservationAfterBindingLeavesNoLeaseForTheFreshAttempt) {
    fail_credentials = true;
    ASSERT_NO_FATAL_FAILURE(admit(Source{7, 1}, false));
    ASSERT_TRUE(eventually([&] { coordinator->tick(); return validation_code.load() > 0; }, 5s));
    EXPECT_EQ(validation_code.load(), static_cast<int>(ErrorCode::timeout));
    EXPECT_FALSE(fail_credentials.load()) << "the observation never reached GetAPCredentialsV1";
    coordinator->retire();
    ASSERT_TRUE(client->reconnect(identity));
    coordinator->begin({7, 0});
    EXPECT_TRUE(eventually([&] { coordinator->tick(); return bluez.command("wire-read") != "open"; }, 3s))
        << "the bootstrap bound by the failed observation is still live";
    ASSERT_EQ(bluez.command("NewConnection"), "allowed");
    EXPECT_TRUE(eventually([&] { coordinator->tick(); return coordinator->admitting(); }, 5s))
        << "validation=" << validation_code.load() << " authorization=" << authorization_code.load();
    HandoffFailure failure{};
    EXPECT_TRUE(client->poll_bootstrap({7, 0}, failure)) << "the retired attempt's lease carried over";
}
}
}
