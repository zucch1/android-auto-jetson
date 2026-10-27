// SPDX-License-Identifier: GPL-3.0-or-later
#include "Platform.hpp"
#include <condition_variable>
#include <iostream> // IWYU pragma: keep
#include <mutex>
#include <thread>
#include <utility>
#include <future>

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
    bool wanted{}, exiting{}, revoke_pending{}, revoke_done{}, rotated{}, rekey_pending{};
    std::uint64_t generation{};
    std::uint64_t revoking_phone{};
    State state{State::inactive};
    std::unique_ptr<Connection> connection;
    std::string device;
    bool protected_interface{};
    bool owned_table{};
    bool reconciled{};
    bool quarantined{};
    ApSnapshot activation;
    struct SnapshotJob {
        std::promise<core::Result<ApSnapshot>> result;
        SnapshotDeadline deadline;
        SnapshotDeadline authority_deadline;
    };
    std::shared_ptr<SnapshotJob> snapshot_job;
    struct FenceJob {
        std::promise<bool> result;
        std::uint64_t epoch;
        std::function<void()> send;
        SnapshotDeadline deadline;
    };
    std::shared_ptr<FenceJob> fence_job;
    std::uint64_t validated_epoch{};
    std::pair<std::string, std::string> validated_endpoint;
    SnapshotDeadline validated_deadline;
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
    bool quarantine_input() {
        return commands->run({"/usr/sbin/nft", "-f", "-"},
            "flush chain inet aa_receiver_ap input\n"
            "add rule inet aa_receiver_ap input tcp dport " + std::to_string(policy.port) + " drop\n"
            "add rule inet aa_receiver_ap input iifname \"" + policy.interface + "\" drop\n").has_value();
    }
    bool stop() {
        wipe(activation.password); activation.generation = 0;
        if (protected_interface && !quarantined) quarantined = quarantine_input();
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
        // A fresh in-memory SSID/PSK per activation: credentials released for an
        // earlier activation never admit a station to this one.
        const auto secret = fresh_credentials();
        if (!secret || !current(epoch)) return false;
        if (!commands->run({"/usr/sbin/nft", "-f", "-"}, firewall(policy))) return false;
        owned_table = protected_interface = true;
        if (!current(epoch) || Clock::now() >= deadline || !connection->start(policy, secret.value(), device)) return false;
        while (current(epoch) && Clock::now() < deadline) {
            if (!connection->healthy()) return false;
            if (connection->active()) {
                if (!actual_channel(policy, *commands) || Clock::now() >= deadline) return false;
                const std::lock_guard lock{mutex};
                if (wanted && !exiting && generation == epoch) {
                    activation = ApSnapshot{{secret.value().ssid, secret.value().psk, {}, {}, policy.port, epoch}};
                    state = State::active; return true;
                }
                return false;
            }
            std::unique_lock lock{mutex}; changed.wait_for(lock, std::chrono::milliseconds(50));
        }
        return false;
    }
#include "BackendWorker.inc"
};
Backend::Backend(Policy policy, std::string address, std::unique_ptr<Commands> commands)
    : impl_(std::make_unique<Impl>(std::move(policy), std::move(address), std::move(commands))) {}
Backend::~Backend() = default;
bool Backend::snapshot_current(std::uint64_t epoch) {
    return with_snapshot(epoch, [] {});
}
bool Backend::with_snapshot(std::uint64_t epoch, const std::function<void()>& send) {
    auto job = std::make_shared<Impl::FenceJob>();
    auto result = job->result.get_future();
    {
        const std::lock_guard lock{impl_->mutex};
        if (!epoch || epoch != impl_->generation || epoch != impl_->validated_epoch || !impl_->wanted || impl_->exiting ||
            impl_->revoke_pending || impl_->state != State::active || impl_->fence_job) return false;
        job->epoch = epoch; job->send = send; job->deadline = impl_->validated_deadline;
        if (job->deadline.expired()) return false;
        job->deadline.until = std::min(job->deadline.until, job->deadline.now() + std::chrono::milliseconds(400));
        impl_->fence_job = job; impl_->changed.notify_all();
    }
    // The callback borrows caller storage, so drain the worker even on expiry.
    return result.get();
}
core::Result<ApSnapshot> Backend::ap_snapshot() {
    return ap_snapshot({});
}
core::Result<ApSnapshot> Backend::ap_snapshot(const SnapshotDeadline& deadline) {
    if (deadline.expired()) return Error{ErrorCode::helper_unavailable};
    auto job = std::make_shared<Impl::SnapshotJob>();
    job->deadline = deadline;
    job->authority_deadline = deadline;
    job->deadline.until = std::min(deadline.until, deadline.now() + std::chrono::milliseconds(400));
    auto result = job->result.get_future();
    {
        const std::lock_guard lock{impl_->mutex};
        if (!impl_->wanted || impl_->exiting || impl_->revoke_pending || impl_->state != State::active || impl_->snapshot_job)
            return Error{ErrorCode::helper_unavailable};
        impl_->snapshot_job = job; impl_->changed.notify_all();
    }
    while (result.wait_for(std::chrono::milliseconds(1)) != std::future_status::ready) {
        if (job->deadline.expired()) return Error{ErrorCode::helper_unavailable};
    }
    if (job->deadline.expired()) return Error{ErrorCode::helper_unavailable};
    auto snapshot = result.get();
    if (!snapshot) return Error{ErrorCode::helper_unavailable};
    return snapshot;
}
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
void Backend::rekey(std::uint64_t epoch) {
    const std::lock_guard lock{impl_->mutex};
    if (!epoch || epoch != impl_->generation || !impl_->wanted || impl_->exiting || impl_->revoke_pending) return;
    impl_->rekey_pending = true; ++impl_->generation; impl_->validated_epoch = 0;
    impl_->state = State::inactive; impl_->changed.notify_all();
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
