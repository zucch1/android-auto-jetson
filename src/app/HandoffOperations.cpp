// SPDX-License-Identifier: GPL-3.0-or-later
#include <aa/app/HandoffEligibility.hpp>
namespace aa::app {
HandoffOperations handoff_operations(helper::BusClient& client, HandoffWait waiting) {
    return {
        [&client, waiting](HandoffOperations::Validation request) -> core::Result<HandoffObservation> {
            if (request.handle && request.credentials_allowed) {
                auto snapshot = client.get_ap_credentials(request.source, request.handle);
                if (!snapshot) return snapshot.error();
                return HandoffObservation{{request.handle, {}}, std::move(snapshot).value()};
            }
            helper::HandoffFailure failure{};
            auto peer = client.poll_bootstrap(request.source, failure);
            if (waiting) waiting->store(static_cast<std::uint32_t>(failure));
            if (!peer) return peer.error();
            HandoffObservation observation{peer.value(), std::nullopt};
            if (!request.handle && request.credentials_allowed) {
                auto snapshot = client.get_ap_credentials(request.source, peer.value().handle);
                if (!snapshot) return snapshot.error();
                observation.snapshot = std::move(snapshot).value();
            }
            return observation;
        },
        [&client](helper::Source source, std::uint64_t handle, bool allow) {
            return client.authorize_bootstrap(source, handle, allow, allow);
        },
        [&client](helper::Source source, std::uint64_t handle) {
            return client.authorize_bootstrap(source, handle, true, false);
        }};
}
}
