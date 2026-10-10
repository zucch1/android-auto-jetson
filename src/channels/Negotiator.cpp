// SPDX-License-Identifier: GPL-3.0-or-later
#include <aa/channels/Negotiator.hpp>

namespace aa::channels {
protocol::ServiceDiscoveryResponse Negotiator::start() {
    reset();
    protocol::ServiceDiscoveryResponse response;
    for (const auto& descriptor : support_.services()) {
        const auto token = registry_.generation(descriptor.role);
        if (!token) { continue; }
        routes_.emplace(descriptor.id.value, Route{descriptor, *token, false});
        response.services.push_back(descriptor);
    }
    return response;
}

void Negotiator::reset() noexcept { routes_.clear(); }

core::Result<protocol::ChannelOpenResponse>
Negotiator::open(const protocol::ChannelOpenRequest& request) {
    if (!request.service) { return Error{ErrorCode::protocol_malformed_message}; }
    const auto found = routes_.find(request.service.value);
    if (found == routes_.end()
        || registry_.generation(found->second.descriptor.role) != found->second.generation) {
        return protocol::ChannelOpenResponse{protocol::ChannelOpenStatus::unsupported};
    }
    found->second.opened = true;
    return protocol::ChannelOpenResponse{protocol::ChannelOpenStatus::success};
}

const Negotiator::Route* Negotiator::opened_route(protocol::ServiceKey service) const {
    const auto found = routes_.find(service.value);
    if (!service || found == routes_.end() || !found->second.opened
        || registry_.generation(found->second.descriptor.role) != found->second.generation) {
        return nullptr;
    }
    return &found->second;
}

core::Result<void> Negotiator::dispatch(const protocol::WireMessageView& message) {
    const auto* route = opened_route(protocol::ServiceKey{message.service});
    if (route == nullptr) { return Error{ErrorCode::channel_not_registered}; }
    const protocol::MessageView semantic{
        {route->descriptor.role, message.sequence, message.timestamp}, message.payload};
    return registry_.dispatch(semantic, route->generation);
}
} // namespace aa::channels
