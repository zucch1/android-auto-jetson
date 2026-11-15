// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <aa/config/VideoProfiles.hpp>
#include <aa/input/Capability.hpp>
#include <aa/protocol/Messages.hpp>
#include <utility>
#include <vector>

namespace aa::wireless_receiver {

// Service discovery for a wireless link. The video service offers exactly the
// profiles the receiver was configured with (--video-profiles, defaulting to the
// shared default), so a negotiated profile the phone picks is one we offered.
inline std::vector<protocol::ServiceDescriptor> service_descriptors(
    const input::CapabilityDescriptor& capabilities,
    std::vector<protocol::VideoProfile> profiles = config::default_video_profiles()) {
    using protocol::ChannelRole;
    return {
        {protocol::ServiceKey{99}, ChannelRole::video,
            protocol::VideoConfiguration{std::move(profiles)}},
        {protocol::ServiceKey{100}, ChannelRole::media_audio,
            protocol::AudioConfiguration{{{48000, 16, 2}}}},
        {protocol::ServiceKey{101}, ChannelRole::guidance_audio,
            protocol::AudioConfiguration{{{16000, 16, 1}}}},
        {protocol::ServiceKey{102}, ChannelRole::system_audio,
            protocol::AudioConfiguration{{{16000, 16, 1}}}},
        {protocol::ServiceKey{104}, ChannelRole::input,
            protocol::ButtonConfiguration{capabilities.keycodes()}},
        {protocol::ServiceKey{105}, ChannelRole::sensors,
            protocol::SensorConfiguration{{protocol::SensorKind::night_mode}}},
        {protocol::ServiceKey{106}, ChannelRole::microphone,
            protocol::MicrophoneConfiguration{{16000, 16, 1}}},
    };
}

} // namespace aa::wireless_receiver
