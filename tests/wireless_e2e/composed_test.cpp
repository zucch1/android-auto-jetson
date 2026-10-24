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
    std::unique_ptr<Coordinator> coordinator;
    Clock::time_point bootstrap_deadline{};
    void TearDown() override {
        coordinator.reset();
        std::cout << "refresh_max_ms=" << refresh_max_ms.load() << '\n';
        NmDaemon::TearDown();
    }
    void admit() {
        start(); acquire(); ASSERT_TRUE(state(State::active));
        const trust::TransportIdentity identity{trust::WirelessIdentity{"02:00:00:00:00:01", "02:00:00:00:00:AA"}};
        ASSERT_TRUE(store->approve(identity)); ASSERT_EQ(bluez.command("paired"), "ok");
        ASSERT_TRUE(client->reconnect(identity));
        bootstrap_deadline = Clock::now() + 10s;
        ASSERT_EQ(bluez.command("NewConnection"), "allowed");
        auto operations = app::handoff_operations(*client);
        auto validate = std::move(operations.validate);
        operations.validate = [this, validate](app::HandoffOperations::Validation request) {
            const auto began = Clock::now();
            auto result = validate(request);
            validation_ms = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - began).count();
            if (request.handle) refresh_max_ms = std::max(refresh_max_ms.load(), validation_ms.load());
            validation_code = result ? 0 : static_cast<int>(result.error().code());
            return result;
        };
        auto authorize = std::move(operations.authorize);
        operations.authorize = [this, authorize](Source source, std::uint64_t handle, bool allow) {
            if (allow) ready_before_allow = listening;
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
        coordinator->consumer(core::ConsumerId{7}); coordinator->begin({7, 0});
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
            EXPECT_TRUE(eventually([&] { return bluez.command("wire-read") == "closed"; }));
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
    EXPECT_EQ(bluez.command("wire-read"), "closed");
    const auto until = Clock::now() + 1500ms;
    while (Clock::now() < until) {
        coordinator->tick(true);
        ASSERT_TRUE(coordinator->valid_session()) << "validation=" << validation_code.load();
        EXPECT_FALSE(coordinator->admitting());
        std::this_thread::sleep_for(10ms);
    }
}
}
}
