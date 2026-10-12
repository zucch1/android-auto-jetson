// SPDX-License-Identifier: GPL-3.0-or-later
// Task 25 pure PCM mixer: per-role gains, speech/guidance ducking of media,
// global mute. No GStreamer here (that stays in the adapter zone).
#include <aa/audio/AudioMix.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace aa::audio {
namespace {

[[nodiscard]] double gain_of(Role role, bool ducked, const MixPolicy& policy) noexcept {
    switch (role) {
    case Role::media: return ducked ? policy.media_ducked_gain : policy.media_gain;
    case Role::guidance: return policy.guidance_gain;
    case Role::system: return policy.system_gain;
    case Role::speech: return policy.speech_gain;
    }
    return 0.0;
}

[[nodiscard]] std::int16_t clamp_s16(double value) noexcept {
    const long rounded = std::lround(value);
    const long lo = static_cast<long>(std::numeric_limits<std::int16_t>::min());
    const long hi = static_cast<long>(std::numeric_limits<std::int16_t>::max());
    return static_cast<std::int16_t>(std::clamp(rounded, lo, hi));
}

[[nodiscard]] std::int16_t load_s16(const std::byte* base, std::size_t index) noexcept {
    std::int16_t value{};
    std::memcpy(&value, base + index * sizeof(std::int16_t), sizeof(value));
    return value;
}

} // namespace

AudioMixer::AudioMixer(MixPolicy policy) noexcept : policy_(policy) {}

void AudioMixer::set_muted(bool muted) noexcept { muted_ = muted; }

[[nodiscard]] bool AudioMixer::muted() const noexcept { return muted_; }

void AudioMixer::set_policy(const MixPolicy& policy) noexcept { policy_ = policy; }

[[nodiscard]] const MixPolicy& AudioMixer::policy() const noexcept { return policy_; }

core::Result<MixedChunk> AudioMixer::mix(const MixFrame& frame) {
    if (frame.samples == 0) {
        return Error{ErrorCode::invalid_argument};
    }

    const AudioFormat* format = nullptr;
    std::size_t contributing = 0;
    for (const PcmChunk* chunk : frame.roles) {
        if (chunk == nullptr) {
            continue;
        }
        ++contributing;
        if (chunk->format.bits_per_sample != 16 || chunk->format.channels == 0) {
            return Error{ErrorCode::audio_format_unsupported};
        }
        const std::size_t expected =
            static_cast<std::size_t>(frame.samples) * chunk->format.channels * sizeof(std::int16_t);
        if (chunk->pcm.size() != expected) {
            return Error{ErrorCode::audio_format_unsupported};
        }
        if (format == nullptr) {
            format = &chunk->format;
        } else if (!(*format == chunk->format)) {
            return Error{ErrorCode::audio_format_unsupported};
        }
    }
    if (contributing == 0 || format == nullptr) {
        return Error{ErrorCode::invalid_argument};
    }

    // Ducking state: guidance/speech in this frame, or active within the
    // window before this frame's timestamp.
    if (frame.roles[role_index(Role::guidance)] != nullptr) {
        guidance_seen_ = true;
        last_guidance_active_ = frame.timestamp;
    }
    if (frame.roles[role_index(Role::speech)] != nullptr) {
        speech_seen_ = true;
        last_speech_active_ = frame.timestamp;
    }
    const auto active_within_window = [&](bool seen, core::Nanoseconds last) {
        if (!seen) {
            return false;
        }
        const std::int64_t delta_ns = frame.timestamp.count - last.count;
        return delta_ns >= 0 && delta_ns <= core::to_nanoseconds(policy_.ducking_window).count;
    };
    const bool ducked = active_within_window(guidance_seen_, last_guidance_active_)
        || active_within_window(speech_seen_, last_speech_active_);

    const std::size_t channels = format->channels;
    const std::size_t values = static_cast<std::size_t>(frame.samples) * channels;
    output_.assign(values, 0);
    output_format_ = *format;

    if (!muted_) {
        std::vector<std::int32_t> accum(values, 0);
        for (std::size_t role = 0; role < frame.roles.size(); ++role) {
            const PcmChunk* chunk = frame.roles[role];
            if (chunk == nullptr) {
                continue;
            }
            const double gain = gain_of(static_cast<Role>(role), ducked, policy_);
            for (std::size_t i = 0; i < values; ++i) {
                const std::int16_t sample = load_s16(chunk->pcm.data(), i);
                accum[i] += static_cast<std::int32_t>(std::lround(gain * static_cast<double>(sample)));
            }
        }
        for (std::size_t i = 0; i < values; ++i) {
            output_[i] = clamp_s16(static_cast<double>(accum[i]));
        }
    }

    return MixedChunk{*format, frame.timestamp,
                      std::as_bytes(std::span<const std::int16_t>{output_.data(), output_.size()})};
}

} // namespace aa::audio
