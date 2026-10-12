// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <aa/audio/Audio.hpp>
#include <aa/core/Result.hpp>
#include <aa/core/Time.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace aa::audio {

// Mix ratios for the AAP channel model (task 25). Gains are exact binary
// fractions so fixture-sample assertions are exact integer arithmetic:
//
//   role      gain      dB      ducked gain   when
//   media     1.0       0.0     0.25 (-12dB)  speech or guidance active
//   guidance  1.0       0.0     (never)
//   system    0.5      -6.0     (never)
//   speech    1.0       0.0     (never)
//
// "Active" means a guidance or speech chunk was mixed in this frame or within
// ducking_window before the frame timestamp. System audio never ducks media
// (AAP model: only prompt-like roles interrupt music). Mute is global: mixed
// output is silenced regardless of inputs.
struct MixPolicy final {
    float media_gain{1.0F};
    float media_ducked_gain{0.25F};
    float guidance_gain{1.0F};
    float system_gain{0.5F};
    float speech_gain{1.0F};
    core::Milliseconds ducking_window{core::Milliseconds{200}};
};

// One time-aligned mix frame: up to one chunk per role covering the same
// sample range (same format and sample count); a null role contributes
// silence. Roles are indexed by the Role enum order (see role_index).
struct MixFrame final {
    core::Nanoseconds timestamp{};
    std::uint64_t samples{};
    std::array<const PcmChunk*, 4> roles{};
};

// The mixed output frame: all contributing roles summed with their gains
// applied. Deliberately role-free - the routed output stream is not any single
// AAP channel. The span is mixer-owned and valid until the next mix() call.
struct MixedChunk final {
    AudioFormat format{};
    core::Nanoseconds timestamp{};
    std::span<const std::byte> pcm{};
};

// Pure PCM mixer (16-bit interleaved). Caller-driven and thread-free; no
// GStreamer here (that stays in the adapter zone). mix() also updates the
// ducking activity window.
class AudioMixer final {
public:
    explicit AudioMixer(MixPolicy policy = {}) noexcept;

    void set_muted(bool muted) noexcept;
    [[nodiscard]] bool muted() const noexcept;
    void set_policy(const MixPolicy& policy) noexcept;
    [[nodiscard]] const MixPolicy& policy() const noexcept;

    // Every present chunk must be 16-bit, share one channel count, and carry
    // exactly `samples` frames, else -> audio_format_unsupported. An empty
    // frame (zero samples or no contributing role) -> invalid_argument.
    [[nodiscard]] core::Result<MixedChunk> mix(const MixFrame& frame);

private:
    MixPolicy policy_{};
    bool muted_{false};
    bool guidance_seen_{false};
    bool speech_seen_{false};
    core::Nanoseconds last_guidance_active_{};
    core::Nanoseconds last_speech_active_{};
    std::vector<std::int16_t> output_{};
    AudioFormat output_format_{};
};

// Role -> MixFrame.roles index (Role enum order: media, guidance, system,
// speech).
[[nodiscard]] constexpr std::size_t role_index(Role role) noexcept {
    return static_cast<std::size_t>(role);
}

} // namespace aa::audio
