// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "wired-receiver/VideoOutlet.hpp"
#include <aa/audio/AudioRenderWorker.hpp>
#include <aa/audio/AudioRouter.hpp>

#include <charconv>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

namespace aa::tools {

inline constexpr std::int64_t kMaxAudioPlayoutMs =
    audio::kMaxPlayoutDepth.count / 1'000'000;
inline constexpr std::int64_t kMinCatchUpWatermarkMs = 20;
inline constexpr std::int64_t kMaxCatchUpWatermarkMs = 200;

// The longest video hold behind lossless audio (both receivers' default: wired
// #228, wireless #230). Lossless audio keeps every sample, so after a burst or a
// stall the hold has to follow it deeper than the drift rules' 400 ms cap, or
// A/V parts by the difference for the half minute the rate correction needs.
// Wired: 700 ms covers a 16-chunk burst (60 ms + 15 x 42.67 ms = 700 ms, twice
// the phone's 341 ms capture buffer) and a sink stall of up to about 600 ms.
// Wireless: the most one Wi-Fi burst can bring after a drought is what the phone
// held back, its 4-chunk credit window (170 ms) plus its capture buffer (341 ms),
// 12 chunks: 120 ms + 11 x 42.67 ms = 589 ms, under the cap with 111 ms to spare
// (it also covers a sink stall of up to about 580 ms). Either way 300 ms of the
// drive smoke's 1.0 s waiting_idr budget is left for the keyframe itself
// (requests are at most 250 ms apart). Deeper than that a lagging touch screen
// is judged worse than lip sync: the hold is clipped, video_hold_capped says so,
// and A/V parts by the rest until it drains.
inline constexpr std::chrono::milliseconds kLosslessVideoHoldCap{700};

struct PlayoutOptions final {
    std::int64_t audio_playout_ms{};
    std::int64_t audio_catch_up_watermark_ms{0};
    std::optional<std::int64_t> video_delay_ms{}; // absent: match audio playout offset
};

[[nodiscard]] inline bool parse_uint64_strict(std::string_view value, std::uint64_t& out) noexcept {
    if (value.empty()) { return false; }
    std::uint64_t number{};
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), number);
    if (error != std::errc{} || end != value.data() + value.size()) { return false; }
    out = number;
    return true;
}

[[nodiscard]] inline bool parse_audio_playout_ms(std::string_view value,
                                                 std::int64_t& out) noexcept {
    std::uint64_t number{};
    if (!parse_uint64_strict(value, number) ||
        number > static_cast<std::uint64_t>(kMaxAudioPlayoutMs)) {
        return false;
    }
    out = static_cast<std::int64_t>(number);
    return true;
}

// Catch-up never acts below the drift high watermark (20 ms), and above the
// router's 200 ms late cap a run is rejected before it could (#173): values
// outside 20..200 would be silently useless. 0 disables catch-up.
[[nodiscard]] inline bool parse_audio_catch_up_watermark_ms(std::string_view value,
                                                            std::int64_t& out) noexcept {
    std::uint64_t number{};
    if (!parse_uint64_strict(value, number)) { return false; }
    if (number != 0 &&
        (number < static_cast<std::uint64_t>(kMinCatchUpWatermarkMs) ||
         number > static_cast<std::uint64_t>(kMaxCatchUpWatermarkMs))) {
        return false;
    }
    out = static_cast<std::int64_t>(number);
    return true;
}

[[nodiscard]] inline bool parse_video_delay_ms(std::string_view value,
                                               std::optional<std::int64_t>& out) noexcept {
    if (value == "match") {
        out.reset();
        return true;
    }
    std::uint64_t number{};
    if (!parse_uint64_strict(value, number) ||
        number > static_cast<std::uint64_t>(kMaxAudioPlayoutMs)) {
        return false;
    }
    out = static_cast<std::int64_t>(number);
    return true;
}

[[nodiscard]] inline bool validate_playout_options(const PlayoutOptions& options) noexcept {
    // Catch-up trims audio the playout buffer exists to keep: it applies only
    // to the legacy arrival model (--audio-playout-ms 0).
    if (options.audio_catch_up_watermark_ms > 0 && options.audio_playout_ms > 0) {
        return false;
    }
    // A video hold without a playout buffer only skews A/V.
    if (options.video_delay_ms && *options.video_delay_ms > 0 && options.audio_playout_ms == 0) {
        return false;
    }
    return true;
}

// With a fixed --video-delay-ms N it is N; with the default "match" this is
// the fallback hold used before the first playout audio write (the configured
// depth) - after it the hold follows the audio playout offset.
[[nodiscard]] inline std::chrono::milliseconds
video_delay(std::optional<std::int64_t> video_delay_ms, std::int64_t audio_playout_ms) noexcept {
    return std::chrono::milliseconds{video_delay_ms.value_or(audio_playout_ms)};
}

// --video-delay-ms match: hold video for the offset audio really plays with
// (audio_write_completed's playout_offset_ns), not the configured buffer.
// `fallback` (the configured depth) holds video until the first playout audio
// write and after a second without one.
inline void match_video_to_audio(wired_receiver::VideoOutlet& video,
                                 audio::AudioRenderWorker& audio,
                                 std::chrono::nanoseconds fallback) {
    video.set_presentation_delay_source([&audio, fallback](core::Nanoseconds content) {
        const auto offset = audio.playout_offset_at(content);
        return offset.count() > 0 ? offset : fallback;
    });
}

inline void configure_video_hold(wired_receiver::VideoOutlet& video,
                                 audio::AudioRenderWorker& audio,
                                 const PlayoutOptions& options,
                                 bool& follow_out,
                                 std::chrono::milliseconds& video_hold_out) {
    video_hold_out = video_delay(options.video_delay_ms, options.audio_playout_ms);
    follow_out = !options.video_delay_ms && options.audio_playout_ms > 0;
    if (follow_out) {
        if (audio.lossless()) { video.set_max_presentation_delay(kLosslessVideoHoldCap); }
        match_video_to_audio(video, audio, video_hold_out);
    } else {
        video.set_presentation_delay(video_hold_out);
    }
}

[[nodiscard]] inline std::string media_policy_json(std::string_view event_name,
                                                   const PlayoutOptions& options,
                                                   std::uint32_t audio_max_unacked,
                                                   bool follow,
                                                   std::chrono::milliseconds video_hold) {
    return "{\"event\":\"" + std::string{event_name} + "\",\"audio_playout_ms\":" +
           std::to_string(options.audio_playout_ms) +
           ",\"audio_max_unacked\":" + std::to_string(audio_max_unacked) +
           ",\"video_hold\":\"" + (follow ? "match" : "fixed") +
           "\",\"video_delay_ms\":" + std::to_string(video_hold.count()) + "}";
}

} // namespace aa::tools
