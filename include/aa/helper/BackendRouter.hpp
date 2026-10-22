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
private:
    Backend& ap_;
    Backend& bluetooth_;
};
} // namespace aa::helper
