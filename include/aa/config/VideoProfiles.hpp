// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <aa/protocol/Messages.hpp>
#include <algorithm>
#include <charconv>
#include <string_view>
#include <utility>
#include <vector>

namespace aa::config {
inline std::vector<protocol::VideoProfile> default_video_profiles() {
    return {{1280, 720, 30}, {800, 480, 30}};
}
// Atomic parse: callers retain their configuration on any failure.
inline core::Result<std::vector<protocol::VideoProfile>> parse_video_profiles(std::string_view text) {
    std::vector<protocol::VideoProfile> profiles;
    const auto number = [](std::string_view value, unsigned& out) {
        auto r = std::from_chars(value.data(), value.data() + value.size(), out);
        return !value.empty() && r.ec == std::errc{} && r.ptr == value.data() + value.size();
    };
    while (!text.empty()) {
        const auto comma = text.find(',');
        const auto item = text.substr(0, comma);
        const auto x = item.find('x'), at = item.find('@'), colon = item.find(':');
        unsigned w{}, h{}, fps{};
        if (x == item.npos || at == item.npos || colon == item.npos || !(x < at && at < colon)
            || !number(item.substr(0, x), w) || !number(item.substr(x+1, at-x-1), h)
            || !number(item.substr(at+1, colon-at-1), fps) || w > 65535 || h > 65535 || fps > 255) {
            return Error{ErrorCode::config_invalid};
        }
        const auto codec = item.substr(colon+1);
        protocol::VideoCodec selected{};
        for (const auto c : protocol::kVideoCodecs) { if (codec == protocol::codec_name(c)) { selected = c; } }
        if (static_cast<unsigned>(selected) == 0) { return Error{ErrorCode::config_invalid}; }
        protocol::VideoProfile p{static_cast<std::uint16_t>(w), static_cast<std::uint16_t>(h),
            static_cast<std::uint8_t>(fps), selected};
        if (!protocol::valid_video_profile(p) || profiles.size() >= protocol::kMaxVideoProfiles
            || std::find(profiles.begin(), profiles.end(), p) != profiles.end()) { return Error{ErrorCode::config_invalid}; }
        profiles.push_back(p);
        if (comma == text.npos) { break; }
        text.remove_prefix(comma+1);
        if (text.empty()) { return Error{ErrorCode::config_invalid}; }
    }
    if (profiles.empty()) { return Error{ErrorCode::config_invalid}; }
    return profiles;
}
inline constexpr protocol::VideoProfile kFallbackVideoProfile{
    1280, 720, 30, protocol::VideoCodec::h264};

// Ensures the offered list always ends with 1280x720@30:h264 so phones that
// reject all higher/custom profiles still have a universally accepted config,
// unless the caller requested exact profiles only.
inline void ensure_fallback_video_profile(std::vector<protocol::VideoProfile>& profiles,
                                          bool exact_profiles_only = false) {
    if (exact_profiles_only) {
        return;
    }
    if (std::find(profiles.begin(), profiles.end(), kFallbackVideoProfile) != profiles.end()) {
        return;
    }
    if (profiles.size() < protocol::kMaxVideoProfiles) {
        profiles.push_back(kFallbackVideoProfile);
    } else if (!profiles.empty()) {
        profiles.back() = kFallbackVideoProfile;
    } else {
        profiles.push_back(kFallbackVideoProfile);
    }
}

// The `--video-profiles` option as both receivers accept it: atomic, so a bad
// value leaves the configured profiles untouched. Callers own the diagnostic.
inline bool parse_video_profiles_option(std::string_view text,
                                        std::vector<protocol::VideoProfile>& profiles) {
    auto parsed = parse_video_profiles(text);
    if (!parsed) { return false; }
    profiles = std::move(parsed.value());
    return true;
}
} // namespace aa::config
