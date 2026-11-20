// SPDX-License-Identifier: GPL-3.0-or-later
#include <aa/app/HandoffEligibility.hpp>
namespace aa::app {
HandoffOperations handoff_operations(HandoffBus bus) {
    return {
        [bus](HandoffOperations::Validation request) -> core::Result<HandoffObservation> {
            if (request.handle && request.credentials_allowed) {
                auto snapshot = bus.credentials(request.source, request.handle);
                if (!snapshot) return snapshot.error();
                return HandoffObservation{{request.handle, {}}, std::move(snapshot).value()};
            }
            auto peer = bus.poll(request.source);
            if (!peer) return peer.error();
            HandoffObservation observation{peer.value(), std::nullopt};
            if (!request.handle && request.credentials_allowed) {
                auto snapshot = bus.credentials(request.source, peer.value().handle);
                if (!snapshot) {
                    // PollBootstrapV1 has bound the phone's bootstrap, and with it the lease, to this
                    // source, but the error reply names no handle for the coordinator to deny on
                    // retirement. Deny it now: otherwise a fresh attempt's ReconnectPeerV1 keeps the
                    // binding and lease, and rejects that attempt's source (#169 review).
                    (void)bus.authorize(request.source, peer.value().handle, false, false);
                    return snapshot.error();
                }
                observation.snapshot = std::move(snapshot).value();
            }
            return observation;
        },
        [bus](helper::Source source, std::uint64_t handle, bool allow) {
            return bus.authorize(source, handle, allow, allow);
        },
        [bus](helper::Source source, std::uint64_t handle) {
            return bus.authorize(source, handle, true, false);
        }};
}
HandoffBus handoff_bus(helper::BusClient& client) {
    return {
        [&client](helper::Source source) {
            helper::HandoffFailure failure{};
            return client.poll_bootstrap(source, failure);
        },
        [&client](helper::Source source, std::uint64_t handle) { return client.get_ap_credentials(source, handle); },
        [&client](helper::Source source, std::uint64_t handle, bool allow, bool ready) {
            return client.authorize_bootstrap(source, handle, allow, ready);
        }};
}
HandoffOperations handoff_operations(helper::BusClient& client) { return handoff_operations(handoff_bus(client)); }
}
