// SPDX-License-Identifier: GPL-3.0-or-later
#include <aa/protocol/Messages.hpp>

#include <algorithm>
#include <set>

namespace aa::protocol {

bool valid_video_profile(VideoProfile profile) noexcept {
    return profile.fps == 30 && ((profile.width == 1280 && profile.height == 720)
                                || (profile.width == 800 && profile.height == 480));
}

bool valid_audio_profile(AudioProfile profile) noexcept {
    return profile.sampling_rate >= 8000 && profile.sampling_rate <= 192000
        && (profile.bits == 16 || profile.bits == 24 || profile.bits == 32)
        && profile.channels >= 1 && profile.channels <= 8;
}

core::Result<void> validate_service(const ServiceDescriptor& service) {
    const auto malformed = Error{ErrorCode::protocol_malformed_message};
    if (!service.id) { return malformed; }
    switch (service.role) {
    case ChannelRole::video: {
        const auto* config = std::get_if<VideoConfiguration>(&service.configuration);
        if (config == nullptr || config->profiles.empty()
            || config->profiles.size() > kMaxMediaProfiles) { return malformed; }
        std::vector<VideoProfile> seen;
        for (const auto profile : config->profiles) {
            if (!valid_video_profile(profile)
                || std::find(seen.begin(), seen.end(), profile) != seen.end()) {
                return malformed;
            }
            seen.push_back(profile);
        }
        return {};
    }
    case ChannelRole::media_audio:
    case ChannelRole::guidance_audio:
    case ChannelRole::system_audio:
    case ChannelRole::speech_audio: {
        const auto* config = std::get_if<AudioConfiguration>(&service.configuration);
        if (config == nullptr || config->profiles.empty()
            || config->profiles.size() > kMaxMediaProfiles) { return malformed; }
        std::vector<AudioProfile> seen;
        for (const auto profile : config->profiles) {
            if (!valid_audio_profile(profile)
                || std::find(seen.begin(), seen.end(), profile) != seen.end()) {
                return malformed;
            }
            seen.push_back(profile);
        }
        return {};
    }
    case ChannelRole::input: {
        const auto* config = std::get_if<ButtonConfiguration>(&service.configuration);
        if (config == nullptr || config->keycodes.empty()
            || config->keycodes.size() > kMaxButtonKeycodes) { return malformed; }
        std::set<std::int32_t> seen;
        for (const auto keycode : config->keycodes) {
            if (keycode <= 0 || keycode > 65535 || !seen.insert(keycode).second) {
                return malformed;
            }
        }
        return {};
    }
    case ChannelRole::microphone:
    case ChannelRole::sensors:
    case ChannelRole::bluetooth_projection:
    case ChannelRole::wifi_projection:
    case ChannelRole::diagnostics:
        // No qualified project configuration for these wire services yet.
        return Error{ErrorCode::protocol_unsupported_channel};
    }
    return malformed;
}

} // namespace aa::protocol
