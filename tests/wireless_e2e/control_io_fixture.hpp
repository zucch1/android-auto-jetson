// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "fixture.hpp"
#include <aa/app/WirelessConfirmation.hpp>
#include <aa/trust/PairingPolicy.hpp>
#include "../dbus/fakes.hpp"
#include "../trust/syscall_faults.hpp"
#include "../../src/transport/socket_util.hpp"
#include <filesystem>
#include <iostream>
#include <unistd.h>

namespace wireless_test {
enum class ControlOperation { forget, owner_loss, forget703 };
struct ControlHelper final : aa::helper::Confirmation {
    unsigned revocations{}, teardowns{};
    aa::core::Result<void> confirmed(aa::core::ConsumerId, aa::core::PairingRequestId) override { return {}; }
    void cancel(aa::core::ConsumerId, aa::core::PairingRequestId) override {}
    void teardown(aa::core::ConsumerId) override { ++teardowns; }
    aa::helper::RevocationAttempt forgotten_v1(const aa::helper::RevocationV1&,
                                               const aa::helper::RevocationProgress&) override {
        ++revocations;
        aa::helper::RevocationAttempt result;
        result.ap = aa::core::Result<void>{};
        result.bond = aa::core::Result<void>{};
        return result;
    }
};
struct ControlIoFixture {
    using Clock = std::chrono::steady_clock;
    std::string directory;
    aa::trust::ApprovedPhoneStore store;
    Fixture f;
    aa::app::WirelessLink link{{store, f.clock, f.diagnostics}, f.config()};
    aa::app::HandoffEligibilityCoordinator coordinator{store,
         {[this](aa::app::HandoffOperations::Validation request) -> aa::core::Result<aa::app::HandoffObservation> {
             if (!f.helper_live) return aa::Error{aa::ErrorCode::helper_unavailable};
             return aa::app::HandoffObservation{f.peer, request.credentials_allowed ? std::optional{f.snapshot} : std::nullopt};
         }, [](aa::helper::Source, std::uint64_t, bool) -> aa::core::Result<void> { return {}; },
         [](aa::helper::Source, std::uint64_t) -> aa::core::Result<void> { return {}; }},
        {[this](const aa::helper::ApSnapshot& ap) { return link.listen(ap); }, [this] { link.close_admission(); }}};
    ControlHelper helper;
    aa::app::WirelessConfirmation confirmation{helper, [this] {
        return link.session() && link.session()->state() == aa::session::State::active;
    }, [this](aa::core::ConsumerId) { coordinator.consumer(std::nullopt); }};
    aa::trust::SteadyClock clock;
    aa::trust::PairingPolicy phones{store, clock};
    aa::ipc::test::FakePeerCredentials peers;
    aa::ipc::test::RecordingEventSink events;
    aa::ipc::test::SequencePairingIds ids;
    aa::ipc::ControlService control{{::getuid()}, {peers, events, ids, phones, &confirmation}};
    const aa::ipc::BusName caller{":1.900"}, name{"org.custom.Consumer"};
    aa::core::PhoneId phone{};
    bool dispatched{};
    Clock::time_point lost{}, returned{};
    static std::string temporary_directory() {
        std::string path = "/tmp/wireless-control-XXXXXX";
        if (!::mkdtemp(path.data())) throw std::system_error{errno, std::generic_category()};
        return path;
    }
    ControlIoFixture() : directory(temporary_directory()), store(std::filesystem::path{directory} / "phones") {}
    ~ControlIoFixture() { link.stop(); coordinator.retire(); std::filesystem::remove_all(directory); }
    void start() {
        ASSERT_TRUE(store.load());
        const auto approved = store.approve(aa::trust::TransportIdentity{aa::trust::WirelessIdentity{
            f.peer.identity.address, f.peer.identity.adapter}});
        ASSERT_TRUE(approved); phone = approved.value();
        peers.add_connection(caller, ::getuid()); peers.set_owner(name, caller);
        const auto consumer = control.register_consumer(caller, name); ASSERT_TRUE(consumer);
        ASSERT_TRUE(control.start_projection(caller));
        coordinator.consumer(consumer.value()); coordinator.begin({consumer.value().value, 0});
        const auto until = Clock::now() + 500ms;
        while (!coordinator.admitting() && Clock::now() < until) {
            coordinator.tick(); std::this_thread::sleep_for(2ms);
        }
        ASSERT_TRUE(coordinator.admitting()); ASSERT_TRUE(link.listening());
    }
    void dispatch(ControlOperation operation) {
        dispatched = true; lost = Clock::now();
        switch (operation) {
        case ControlOperation::forget:
            EXPECT_TRUE(control.forget_phone(caller, phone));
            EXPECT_TRUE(store.phones().empty()); EXPECT_EQ(helper.revocations, 1U);
            break;
        case ControlOperation::forget703: {
            aa::trust::test::faults::ScopedFault fault{aa::trust::test::faults::Fault::dir_fsync};
            const auto result = control.forget_phone(caller, phone);
            EXPECT_FALSE(result);
            if (!result) { EXPECT_EQ(result.error().code(), aa::ErrorCode::trust_store_not_durable); }
            EXPECT_TRUE(store.phones().empty()); EXPECT_EQ(helper.revocations, 1U);
            break;
        }
        case ControlOperation::owner_loss:
            peers.remove_owner(name);
            EXPECT_EQ(control.state().consumer.value, 0U);
            EXPECT_EQ(helper.teardowns, confirmation_active() ? 0U : 1U);
            break;
        }
        returned = Clock::now();
        EXPECT_FALSE(coordinator.admitting());
        // Accepted ACTIVE links have already closed their listener; admission
        // cancellation cases observe the on-stack accept returning separately.
        if (link.session()) { EXPECT_FALSE(link.listening()); }
    }
    bool confirmation_active() const {
        return link.session() && link.session()->state() == aa::session::State::active;
    }
    void checkpoint() {
        const auto state = control.state();
        coordinator.consumer(state.consumer.value ? std::optional{state.consumer} : std::nullopt);
    }
    void retired(bool grace = false) {
        ASSERT_TRUE(dispatched);
        EXPECT_LT(returned - lost, 5s);
        const auto elapsed = Clock::now() - lost;
        if (grace) {
            EXPECT_GE(elapsed, 10s); EXPECT_LT(elapsed, 11s);
        } else EXPECT_LT(elapsed, 5s);
        EXPECT_FALSE(link.listening()); EXPECT_FALSE(coordinator.admitting());
        EXPECT_FALSE(coordinator.valid_session()); EXPECT_EQ(link.session(), nullptr);
        // Restoring observable eligibility must not revive this retired epoch.
        peers.set_owner(name, caller);
        const auto until = Clock::now() + 100ms;
        while (Clock::now() < until) { coordinator.tick(); std::this_thread::yield(); }
        EXPECT_FALSE(coordinator.admitting()); EXPECT_FALSE(link.listening());
        EXPECT_FALSE(f.connect());
        std::cout << "control_ms=" << std::chrono::duration_cast<std::chrono::milliseconds>(returned - lost).count()
                  << " teardown_ms=" << std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - lost).count() << '\n';
    }
};
struct SocketPeer {
    int fd{-1};
    ~SocketPeer() { aa::transport::socket_util::close_fd(fd); }
    void expect_closed() const {
        const auto until = ControlIoFixture::Clock::now() + 1s;
        std::array<char, 65536> data{};
        ssize_t count{-1};
        while (ControlIoFixture::Clock::now() < until) {
            count = ::recv(fd, data.data(), data.size(), MSG_DONTWAIT);
            if (count == 0 || (count < 0 && errno == ECONNRESET)) break;
            if (count < 0 && errno != EAGAIN && errno != EWOULDBLOCK) break;
            std::this_thread::yield();
        }
        EXPECT_TRUE(count == 0 || (count < 0 && errno == ECONNRESET));
    }
};
}
