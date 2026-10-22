// SPDX-License-Identifier: GPL-3.0-or-later
#include "Platform.hpp"
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <thread>

namespace aa::helper::nm {
namespace {
using Clock = std::chrono::steady_clock;
constexpr auto device_interface = "org.freedesktop.NetworkManager.Device";
}
struct Backend::Impl final {
    Policy policy;
    std::string address;
    std::unique_ptr<Commands> commands;
    std::mutex mutex;
    std::condition_variable changed;
    bool wanted{}, exiting{}, revoke_pending{}, revoke_done{}, rotated{};
    std::uint64_t generation{};
    std::uint64_t revoking_phone{};
    State state{State::inactive};
    std::unique_ptr<Connection> connection;
    std::string device;
    bool protected_interface{};
    bool owned_table{};
    bool reconciled{};
    bool quarantined{};
    std::thread worker;
    Impl(Policy provision, std::string bus, std::unique_ptr<Commands> runner)
        : policy(std::move(provision)), address(std::move(bus)), commands(std::move(runner)),
          worker([this] { run(); }) {}
    ~Impl() {
        { const std::lock_guard lock{mutex}; exiting = true; wanted = false; ++generation; }
        changed.notify_all(); worker.join();
    }
    bool current(std::uint64_t epoch) {
        const std::lock_guard lock{mutex};
        return wanted && !exiting && epoch == generation;
    }
    void publish(State value) {
        const std::lock_guard lock{mutex}; state = value; changed.notify_all();
    }
    bool reconcile() {
        connection = std::make_unique<Connection>(address);
        if (!connection->healthy()) return false;
        device = connection->device(policy.interface);
        if (device.empty() || connection->number_property(device, device_interface, "DeviceType") != 2 ||
            connection->text_property(device, device_interface, "Interface") != policy.interface ||
            connection->text_property(device, device_interface, "ActiveConnection") != "/") return false;
        const auto tables = commands->run({"/usr/sbin/nft", "list", "tables", "inet"});
        if (!tables) return false;
        if (tables.value().find("table inet aa_receiver_ap\n") != std::string::npos) {
            const auto table = commands->run({"/usr/sbin/nft", "list", "table", "inet", "aa_receiver_ap"});
            if (!table) return false;
            if (table.value().find("comment \"aa-system-helper-v1\"") == std::string::npos ||
                table.value().find("table inet aa_receiver_ap") == std::string::npos) return false;
            owned_table = protected_interface = true;
            if (!remove_rules()) return false;
        }
        reconciled = true;
        return true;
    }
    bool remove_rules() {
        if (!owned_table) return true;
        if (!commands->run({"/usr/sbin/nft", "-f", "-"}, "delete table inet aa_receiver_ap\n")) return false;
        owned_table = protected_interface = quarantined = false;
        return true;
    }
    bool stop() {
        if (protected_interface && !quarantined) {
            quarantined = commands->run({"/usr/sbin/nft", "-f", "-"},
                "flush chain inet aa_receiver_ap input\nadd rule inet aa_receiver_ap input iifname \"" +
                policy.interface + "\" drop\n").has_value();
        }
        if (!connection || device.empty()) return !protected_interface;
        if (!connection->stop(device)) return false;
        return remove_rules();
    }
    bool start(std::uint64_t epoch) {
        const auto deadline = Clock::now() + std::chrono::seconds(5);
        if (!reconciled && !reconcile()) return false;
        if (!connection || !connection->healthy()) return false;
        if (connection->text_property(device, device_interface, "ActiveConnection") != "/") return false;
        if (!current(epoch) || !eligible(policy, *commands)) return false;
        const auto secret = credentials(policy, false);
        if (!secret || !current(epoch)) return false;
        if (!commands->run({"/usr/sbin/nft", "-f", "-"}, firewall(policy))) return false;
        owned_table = protected_interface = true;
        if (!current(epoch) || Clock::now() >= deadline || !connection->start(policy, secret.value(), device)) return false;
        while (current(epoch) && Clock::now() < deadline) {
            if (!connection->healthy()) return false;
            if (connection->active()) {
                if (!actual_channel(policy, *commands) || Clock::now() >= deadline) return false;
                const std::lock_guard lock{mutex};
                if (wanted && !exiting && generation == epoch) { state = State::active; return true; }
                return false;
            }
            std::unique_lock lock{mutex}; changed.wait_for(lock, std::chrono::milliseconds(50));
        }
        return false;
    }
    void run() {
        std::uint64_t attempted = 0;
        auto next_regulatory = Clock::now();
        for (;;) {
            bool request{}, quit{}, revoke{};
            std::uint64_t epoch{};
            { std::unique_lock lock{mutex};
              changed.wait_for(lock, std::chrono::milliseconds(100));
              request = wanted; quit = exiting; epoch = generation; revoke = revoke_pending;
            }
            if (quit) { (void)stop(); return; }
            if (revoke) {
                if (protected_interface && !quarantined) {
                    quarantined = commands->run({"/usr/sbin/nft", "-f", "-"},
                        "flush chain inet aa_receiver_ap input\nadd rule inet aa_receiver_ap input iifname \"" +
                        policy.interface + "\" drop\n").has_value();
                }
                if (!rotated) rotated = credentials(policy, true).has_value();
                const bool safe = stop();
                const std::lock_guard lock{mutex};
                state = safe && rotated ? State::inactive : State::unavailable;
                if (safe && rotated) { revoke_pending = false; revoke_done = true; changed.notify_all(); }
                continue;
            }
            if (!request) {
                publish(!protected_interface || stop() ? State::inactive : State::unavailable);
                continue;
            }
            if (attempted != epoch) {
                attempted = epoch;
                if (!start(epoch)) {
                    (void)stop();
                    publish(State::unavailable);
                    std::cerr << "system-helper: AP startup failed; check jurisdiction, channel36, NM and isolation\n";
                }
                continue;
            }
            State observed;
            { const std::lock_guard lock{mutex}; observed = state; }
            bool regulatory_ok = true;
            if (observed == State::active && Clock::now() >= next_regulatory) {
                regulatory_ok = eligible(policy, *commands);
                next_regulatory = Clock::now() + std::chrono::seconds(1);
            }
            if (observed == State::active && (!regulatory_ok || !connection->healthy() || !connection->active() ||
                !actual_channel(policy, *commands))) {
                (void)stop(); publish(State::unavailable);
                std::cerr << "system-helper: AP lost eligibility/channel/owner; rolled back\n";
            } else if (observed == State::unavailable && protected_interface) (void)stop();
        }
    }
};
Backend::Backend(Policy policy, std::string address, std::unique_ptr<Commands> commands)
    : impl_(std::make_unique<Impl>(std::move(policy), std::move(address), std::move(commands))) {}
Backend::~Backend() = default;
core::Result<State> Backend::apply(Action action, std::uint32_t channel) {
    const std::lock_guard lock{impl_->mutex};
    switch (action) {
    case Action::acquire_ap:
        if (channel != 36 || impl_->revoke_pending) return Error{ErrorCode::helper_rejected};
        if (!impl_->wanted) {
            impl_->wanted = true; ++impl_->generation; impl_->state = State::inactive;
            impl_->revoke_done = false; impl_->rotated = false; impl_->changed.notify_all();
        }
        return impl_->state;
    case Action::ap_status: return impl_->state;
    case Action::release_ap:
        impl_->wanted = false; ++impl_->generation; impl_->state = State::unavailable;
        impl_->changed.notify_all(); return State::unavailable;
    case Action::usb_status:
    case Action::modem_status: return State::unavailable;
    default: return Error{ErrorCode::helper_unavailable};
    }
}
void Backend::cleanup() noexcept {
    const std::lock_guard lock{impl_->mutex};
    impl_->wanted = false; ++impl_->generation; impl_->state = State::unavailable; impl_->changed.notify_all();
}
core::Result<void> Backend::revoke(std::uint64_t phone) {
    std::unique_lock lock{impl_->mutex};
    if (impl_->revoke_done && impl_->revoking_phone == phone) return {};
    if (impl_->revoke_pending && impl_->revoking_phone != phone) return Error{ErrorCode::helper_unavailable};
    if (!impl_->revoke_pending) {
        impl_->revoking_phone = phone; impl_->revoke_done = false; impl_->rotated = false;
        impl_->revoke_pending = true; impl_->wanted = false; ++impl_->generation; impl_->changed.notify_all();
    }
    if (impl_->changed.wait_for(lock, std::chrono::milliseconds(250), [this] { return impl_->revoke_done; })) return {};
    return Error{ErrorCode::helper_unavailable};
}
}
