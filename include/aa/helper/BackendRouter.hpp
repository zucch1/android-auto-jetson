// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <aa/helper/SystemHelper.hpp>

namespace aa::helper {
// Dispatch by ownership, never by probing/falling back after an effect fails.
class BackendRouter final : public Backend {
public:
    struct Routes { Backend& ap; Backend& bluetooth; };
    explicit BackendRouter(Routes routes) : ap_(routes.ap), bluetooth_(routes.bluetooth) {}
    core::Result<State> apply(Action action, std::uint32_t channel) override {
        switch (action) {
        case Action::acquire_ap:
        case Action::release_ap:
        case Action::ap_status: return ap_.apply(action, channel);
        case Action::open_pairing:
        case Action::close_pairing:
        case Action::register_profile:
        case Action::unregister_profile:
        case Action::reconnect: return bluetooth_.apply(action, channel);
        case Action::usb_status:
        case Action::modem_status: return State::unavailable;
        }
        return Error{ErrorCode::helper_rejected};
    }
    void cleanup() noexcept override { ap_.cleanup(); bluetooth_.cleanup(); }
    void retire_lease() noexcept override { ap_.retire_lease(); bluetooth_.retire_lease(); }
    void tick() override { ap_.tick(); bluetooth_.tick(); }
    core::Result<void> revoke(std::uint64_t phone) override { return ap_.revoke(phone); }
    core::Result<ObservedPeer> observe() override { return bluetooth_.observe(); }
    core::Result<State> authorize(std::uint64_t handle) override { return bluetooth_.authorize(handle); }
    core::Result<State> reconnect_peer(std::uint64_t handle) override { return bluetooth_.reconnect_peer(handle); }
    core::Result<void> revoke_peer(const PeerIdentity& peer) override { return bluetooth_.revoke_peer(peer); }
    void discard(std::uint64_t handle) override { bluetooth_.discard(handle); }
    core::Result<ObservedPeer> handoff() override { return bluetooth_.handoff(); }
    HandoffFailure handoff_failure() const override { return bluetooth_.handoff_failure(); }
    void fail_handoff(std::uint64_t handle, HandoffFailure reason) override { bluetooth_.fail_handoff(handle, reason); }
    core::Result<void> authorize_handoff(std::uint64_t handle, bool allow, bool ready) override {
        return bluetooth_.authorize_handoff(handle, allow, ready);
    }
    void snapshot_provider(const SnapshotProvider& provider) override { bluetooth_.snapshot_provider(provider); }
    void snapshot_fence(const SnapshotFence& fence) override { bluetooth_.snapshot_fence(fence); }
    core::Result<ApSnapshot> ap_snapshot() override { return ap_.ap_snapshot(); }
    core::Result<ApSnapshot> ap_snapshot(const SnapshotDeadline& deadline) override { return ap_.ap_snapshot(deadline); }
    SnapshotDeadline handoff_deadline() const override { return bluetooth_.handoff_deadline(); }
    bool snapshot_current(std::uint64_t generation) override { return ap_.snapshot_current(generation); }
    bool with_snapshot(std::uint64_t generation, const std::function<void()>& send) override { return ap_.with_snapshot(generation, send); }
    void rekey(std::uint64_t generation) override { ap_.rekey(generation); }
private:
    Backend& ap_;
    Backend& bluetooth_;
};
} // namespace aa::helper
