// SPDX-License-Identifier: GPL-3.0-or-later
#include <aa/channels/Negotiator.hpp>
#include <set>

namespace aa::channels {
core::Result<SupportConfiguration>
SupportConfiguration::create(std::vector<protocol::ServiceDescriptor> services) {
    const auto malformed = Error{ErrorCode::protocol_malformed_message};
    if (services.size() > protocol::kMaxDiscoveredServices) { return malformed; }
    std::set<std::int32_t> ids;
    std::set<protocol::ChannelRole> roles;
    for (const auto& service : services) {
        if (!roles.insert(service.role).second) { return malformed; }
        if (service.id.value != 0
            && (!service.id || !ids.insert(service.id.value).second)) { return malformed; }
    }
    std::int32_t candidate = 1;
    for (auto& service : services) {
        if (service.id.value == 0) {
            while (candidate <= 255 && ids.contains(candidate)) { ++candidate; }
            if (candidate > 255) { return malformed; }
            service.id = protocol::ServiceKey{candidate};
            ids.insert(candidate);
        }
        if (const auto valid = protocol::validate_service(service); !valid) { return valid.error(); }
    }
    return SupportConfiguration{std::move(services)};
}
} // namespace aa::channels
