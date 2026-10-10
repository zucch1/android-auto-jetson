// SPDX-License-Identifier: GPL-3.0-or-later
#include "FixtureValidate.hpp"

#include <cstdint>
#include <string>

namespace aa::replay::detail {
namespace {

constexpr Error malformed() { return Error{ErrorCode::invalid_argument}; }

} // namespace

std::string to_hex(std::span<const std::byte> bytes) {
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string out(bytes.size() * 2, '0');
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        const auto value = static_cast<std::uint8_t>(bytes[i]);
        out[2 * i] = kDigits[value >> 4U];
        out[2 * i + 1] = kDigits[value & 0x0FU];
    }
    return out;
}

core::Result<std::vector<std::byte>> from_hex(std::string_view text) {
    if (text.size() % 2 != 0) {
        return malformed();
    }
    const auto nibble = [](char c) -> int {
        if (c >= '0' && c <= '9') {
            return c - '0';
        }
        if (c >= 'a' && c <= 'f') {
            return c - 'a' + 10;
        }
        return -1;
    };
    std::vector<std::byte> out(text.size() / 2);
    for (std::size_t i = 0; i < out.size(); ++i) {
        const int high = nibble(text[2 * i]);
        const int low = nibble(text[2 * i + 1]);
        if (high < 0 || low < 0) {
            return malformed();
        }
        out[i] = static_cast<std::byte>((high << 4) | low);
    }
    return out;
}

std::string_view role_name(protocol::ChannelRole role) noexcept {
    switch (role) {
    case protocol::ChannelRole::video: return "video";
    case protocol::ChannelRole::media_audio: return "media_audio";
    case protocol::ChannelRole::guidance_audio: return "guidance_audio";
    case protocol::ChannelRole::system_audio: return "system_audio";
    case protocol::ChannelRole::speech_audio: return "speech_audio";
    case protocol::ChannelRole::input: return "input";
    case protocol::ChannelRole::microphone: return "microphone";
    case protocol::ChannelRole::sensors: return "sensors";
    case protocol::ChannelRole::bluetooth_projection: return "bluetooth_projection";
    case protocol::ChannelRole::wifi_projection: return "wifi_projection";
    case protocol::ChannelRole::diagnostics: return "diagnostics";
    }
    return "unknown";
}

core::Result<protocol::ChannelRole> parse_role(std::string_view name) {
    for (const protocol::ChannelRole role :
         {protocol::ChannelRole::video, protocol::ChannelRole::media_audio,
          protocol::ChannelRole::guidance_audio, protocol::ChannelRole::system_audio,
          protocol::ChannelRole::speech_audio, protocol::ChannelRole::input,
          protocol::ChannelRole::microphone, protocol::ChannelRole::sensors,
          protocol::ChannelRole::bluetooth_projection, protocol::ChannelRole::wifi_projection,
          protocol::ChannelRole::diagnostics}) {
        if (role_name(role) == name) {
            return role;
        }
    }
    return malformed();
}

std::string_view audio_role_name(audio::Role role) noexcept {
    switch (role) {
    case audio::Role::media: return "media";
    case audio::Role::guidance: return "guidance";
    case audio::Role::system: return "system";
    case audio::Role::speech: return "speech";
    }
    return "unknown";
}

core::Result<audio::Role> parse_audio_role(std::string_view name) {
    for (const audio::Role role : {audio::Role::media, audio::Role::guidance, audio::Role::system,
                                   audio::Role::speech}) {
        if (audio_role_name(role) == name) {
            return role;
        }
    }
    return malformed();
}

std::string_view session_op_name(SessionOp op) noexcept {
    switch (op) {
    case SessionOp::event: return "event";
    case SessionOp::phone_discovered: return "phone_discovered";
    case SessionOp::authorize_pairing: return "authorize_pairing";
    case SessionOp::confirm_pairing: return "confirm_pairing";
    case SessionOp::fail: return "fail";
    case SessionOp::request_stop: return "request_stop";
    case SessionOp::tick: return "tick";
    case SessionOp::attach_fresh: return "attach_fresh";
    case SessionOp::reset_channels: return "reset_channels";
    }
    return "unknown";
}

core::Result<SessionOp> parse_session_op(std::string_view name) {
    for (const SessionOp op : {SessionOp::event, SessionOp::phone_discovered,
                               SessionOp::authorize_pairing, SessionOp::confirm_pairing,
                               SessionOp::fail, SessionOp::request_stop, SessionOp::tick,
                               SessionOp::attach_fresh, SessionOp::reset_channels}) {
        if (session_op_name(op) == name) {
            return op;
        }
    }
    return malformed();
}

} // namespace aa::replay::detail
