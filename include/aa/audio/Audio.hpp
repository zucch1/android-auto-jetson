// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <aa/core/Result.hpp>
#include <aa/core/Time.hpp>

#include <cstddef>
#include <cstdint>
#include <span>

namespace aa::audio {

// Audio routing boundary (task 25 decodes AAP audio and routes it). Playback
// only on the current HDMI/PipeWire default output; A2DP/HFP and call/voice
// paths are out of scope.
//
// Ownership/threading: the sink is owned by the audio thread and write() is
// called only from it. The media path hands over one decoded PCM chunk per
// call; the payload span is valid only for the duration of write().
//
// Error contract: a missing sink or default route degrades the session (no
// crash) and reports audio_sink_unavailable / audio_format_unsupported.
enum class Role { media, guidance, system, speech };

struct AudioFormat final {
    std::uint32_t sample_rate_hz{};
    std::uint8_t channels{};
    std::uint8_t bits_per_sample{};

    [[nodiscard]] friend constexpr bool operator==(const AudioFormat&,
                                                   const AudioFormat&) noexcept = default;
};

// One decoded PCM chunk on the common monotonic A/V clock.
struct PcmChunk final {
    Role role{};
    AudioFormat format{};
    core::Nanoseconds timestamp{};
    std::span<const std::byte> pcm{};
};

class AudioSink {
public:
    virtual ~AudioSink() = default;
    AudioSink() = default;
    AudioSink(const AudioSink&) = delete;
    AudioSink& operator=(const AudioSink&) = delete;
    AudioSink(AudioSink&&) = delete;
    AudioSink& operator=(AudioSink&&) = delete;

    virtual core::Result<void> write(const PcmChunk& chunk) = 0;
    virtual void flush() noexcept = 0;
};

// Release budgets (plan "Contract budgets"), measured end to end later.
inline constexpr core::Milliseconds kMaxAudioLatency{200};
inline constexpr core::Milliseconds kMaxAvSyncSkew{50};

} // namespace aa::audio
