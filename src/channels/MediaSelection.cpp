// SPDX-License-Identifier: GPL-3.0-or-later
#include <aa/channels/Negotiator.hpp>
#include <algorithm>

namespace aa::channels {
core::Result<protocol::VideoProfile>
Negotiator::select_video(protocol::ServiceKey service,
                         std::span<const protocol::VideoProfile> offered) const {
    if (offered.empty() || offered.size() > protocol::kMaxMediaProfiles
        || !std::all_of(offered.begin(), offered.end(), protocol::valid_video_profile)) {
        return Error{ErrorCode::protocol_malformed_message};
    }
    const auto* route = opened_route(service);
    if (route == nullptr) { return Error{ErrorCode::channel_not_registered}; }
    const auto* config = std::get_if<protocol::VideoConfiguration>(&route->descriptor.configuration);
    if (config == nullptr) { return Error{ErrorCode::protocol_unsupported_channel}; }
    for (const auto profile : config->profiles) {
        if (std::find(offered.begin(), offered.end(), profile) != offered.end()) { return profile; }
    }
    return Error{ErrorCode::protocol_unsupported_channel};
}

core::Result<protocol::AudioProfile>
Negotiator::select_audio(protocol::ServiceKey service,
                         std::span<const protocol::AudioProfile> offered) const {
    if (offered.empty() || offered.size() > protocol::kMaxMediaProfiles
        || !std::all_of(offered.begin(), offered.end(), protocol::valid_audio_profile)) {
        return Error{ErrorCode::protocol_malformed_message};
    }
    const auto* route = opened_route(service);
    if (route == nullptr) { return Error{ErrorCode::channel_not_registered}; }
    const auto* config = std::get_if<protocol::AudioConfiguration>(&route->descriptor.configuration);
    if (config == nullptr) { return Error{ErrorCode::protocol_unsupported_channel}; }
    for (const auto profile : config->profiles) {
        if (std::find(offered.begin(), offered.end(), profile) != offered.end()) { return profile; }
    }
    return Error{ErrorCode::protocol_unsupported_channel};
}
} // namespace aa::channels
