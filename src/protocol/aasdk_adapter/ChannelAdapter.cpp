// SPDX-License-Identifier: GPL-3.0-or-later
// Frame-channel adapter (task 15): maps the static AASDK frame-channel ordinal
// (ID space 1, the per-frame channel byte) to and from the project ChannelRole.
// The pinned SDK's ChannelId layout is asserted at compile time so a renumbering
// in the dependency breaks here, not at runtime.
#include <aasdk/Messenger/ChannelId.hpp>

#include <aa/protocol/Messages.hpp>

#include <cstdint>

namespace aa::protocol {
namespace {

namespace sdk = aasdk::messenger;

static_assert(static_cast<std::uint8_t>(sdk::ChannelId::CONTROL) == 0);
static_assert(static_cast<std::uint8_t>(sdk::ChannelId::SENSOR) == 1);
static_assert(static_cast<std::uint8_t>(sdk::ChannelId::MEDIA_SINK) == 2);
static_assert(static_cast<std::uint8_t>(sdk::ChannelId::MEDIA_SINK_VIDEO) == 3);
static_assert(static_cast<std::uint8_t>(sdk::ChannelId::MEDIA_SINK_MEDIA_AUDIO) == 4);
static_assert(static_cast<std::uint8_t>(sdk::ChannelId::MEDIA_SINK_GUIDANCE_AUDIO) == 5);
static_assert(static_cast<std::uint8_t>(sdk::ChannelId::MEDIA_SINK_SYSTEM_AUDIO) == 6);
static_assert(static_cast<std::uint8_t>(sdk::ChannelId::MEDIA_SINK_TELEPHONY_AUDIO) == 7);
static_assert(static_cast<std::uint8_t>(sdk::ChannelId::INPUT_SOURCE) == 8);
static_assert(static_cast<std::uint8_t>(sdk::ChannelId::MEDIA_SOURCE_MICROPHONE) == 9);
static_assert(static_cast<std::uint8_t>(sdk::ChannelId::BLUETOOTH) == 10);
static_assert(static_cast<std::uint8_t>(sdk::ChannelId::RADIO) == 11);
static_assert(static_cast<std::uint8_t>(sdk::ChannelId::NAVIGATION_STATUS) == 12);
static_assert(static_cast<std::uint8_t>(sdk::ChannelId::MEDIA_PLAYBACK_STATUS) == 13);
static_assert(static_cast<std::uint8_t>(sdk::ChannelId::PHONE_STATUS) == 14);
static_assert(static_cast<std::uint8_t>(sdk::ChannelId::MEDIA_BROWSER) == 15);
static_assert(static_cast<std::uint8_t>(sdk::ChannelId::VENDOR_EXTENSION) == 16);
static_assert(static_cast<std::uint8_t>(sdk::ChannelId::GENERIC_NOTIFICATION) == 17);
static_assert(static_cast<std::uint8_t>(sdk::ChannelId::WIFI_PROJECTION) == 18);
static_assert(static_cast<std::uint8_t>(sdk::ChannelId::NONE) == 255);

[[nodiscard]] Error unsupported_channel() noexcept {
    return Error{ErrorCode::protocol_unsupported_channel};
}

} // namespace

core::Result<ChannelRole> channel_role_from_wire(std::uint8_t ordinal) {
    switch (static_cast<sdk::ChannelId>(ordinal)) {
    case sdk::ChannelId::SENSOR:
        return ChannelRole::sensors;
    case sdk::ChannelId::MEDIA_SINK_VIDEO:
        return ChannelRole::video;
    case sdk::ChannelId::MEDIA_SINK_MEDIA_AUDIO:
        return ChannelRole::media_audio;
    case sdk::ChannelId::MEDIA_SINK_GUIDANCE_AUDIO:
        return ChannelRole::guidance_audio;
    case sdk::ChannelId::MEDIA_SINK_SYSTEM_AUDIO:
        return ChannelRole::system_audio;
    case sdk::ChannelId::MEDIA_SINK_TELEPHONY_AUDIO:
        return ChannelRole::speech_audio;
    case sdk::ChannelId::INPUT_SOURCE:
        return ChannelRole::input;
    case sdk::ChannelId::MEDIA_SOURCE_MICROPHONE:
        return ChannelRole::microphone;
    case sdk::ChannelId::BLUETOOTH:
        return ChannelRole::bluetooth_projection;
    case sdk::ChannelId::WIFI_PROJECTION:
        return ChannelRole::wifi_projection;
    case sdk::ChannelId::CONTROL:
    case sdk::ChannelId::MEDIA_SINK:
    case sdk::ChannelId::RADIO:
    case sdk::ChannelId::NAVIGATION_STATUS:
    case sdk::ChannelId::MEDIA_PLAYBACK_STATUS:
    case sdk::ChannelId::PHONE_STATUS:
    case sdk::ChannelId::MEDIA_BROWSER:
    case sdk::ChannelId::VENDOR_EXTENSION:
    case sdk::ChannelId::GENERIC_NOTIFICATION:
    case sdk::ChannelId::NONE:
        return unsupported_channel();
    }
    return unsupported_channel();
}

core::Result<std::uint8_t> wire_from_channel_role(ChannelRole role) {
    switch (role) {
    case ChannelRole::sensors:
        return static_cast<std::uint8_t>(sdk::ChannelId::SENSOR);
    case ChannelRole::video:
        return static_cast<std::uint8_t>(sdk::ChannelId::MEDIA_SINK_VIDEO);
    case ChannelRole::media_audio:
        return static_cast<std::uint8_t>(sdk::ChannelId::MEDIA_SINK_MEDIA_AUDIO);
    case ChannelRole::guidance_audio:
        return static_cast<std::uint8_t>(sdk::ChannelId::MEDIA_SINK_GUIDANCE_AUDIO);
    case ChannelRole::system_audio:
        return static_cast<std::uint8_t>(sdk::ChannelId::MEDIA_SINK_SYSTEM_AUDIO);
    case ChannelRole::speech_audio:
        return static_cast<std::uint8_t>(sdk::ChannelId::MEDIA_SINK_TELEPHONY_AUDIO);
    case ChannelRole::input:
        return static_cast<std::uint8_t>(sdk::ChannelId::INPUT_SOURCE);
    case ChannelRole::microphone:
        return static_cast<std::uint8_t>(sdk::ChannelId::MEDIA_SOURCE_MICROPHONE);
    case ChannelRole::bluetooth_projection:
        return static_cast<std::uint8_t>(sdk::ChannelId::BLUETOOTH);
    case ChannelRole::wifi_projection:
        return static_cast<std::uint8_t>(sdk::ChannelId::WIFI_PROJECTION);
    case ChannelRole::diagnostics:
        return unsupported_channel();
    }
    return unsupported_channel();
}

} // namespace aa::protocol
