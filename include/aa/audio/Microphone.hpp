// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <aa/audio/Audio.hpp>
#include <aa/core/Result.hpp>

#include <cstdint>
#include <string_view>

namespace aa::audio {

// Microphone contract (task 25, fixture-tested seam only). The AAP
// microphone / speech-audio SOURCE direction (phone-side capture for call and
// voice paths) is OUT OF QUALIFICATION SCOPE: A2DP/HFP/SAP/call/voice handling
// is explicitly unimplemented and forbidden here. This seam exists so the
// contract is typed and testable and stays DISABLED by default; nothing in
// this repository enables it implicitly.
enum class MicrophoneState : std::uint8_t { disabled, enabled };

[[nodiscard]] inline std::string_view to_string(MicrophoneState state) noexcept {
    switch (state) {
    case MicrophoneState::disabled: return "disabled";
    case MicrophoneState::enabled: return "enabled";
    }
    return "unknown";
}

// Typed capture seam a future qualified mic path would implement. The
// direction is receiver -> phone (speech-audio source channel).
class MicrophoneSource {
public:
    virtual ~MicrophoneSource() = default;
    MicrophoneSource() = default;
    MicrophoneSource(const MicrophoneSource&) = delete;
    MicrophoneSource& operator=(const MicrophoneSource&) = delete;
    MicrophoneSource(MicrophoneSource&&) = delete;
    MicrophoneSource& operator=(MicrophoneSource&&) = delete;

    [[nodiscard]] virtual core::Result<void> write(const PcmChunk& chunk) = 0;
    virtual void flush() noexcept = 0;
};

// Admission gate. Disabled unless enable_explicitly() is called; only that
// call flips the state (router operations, submits and playback never do).
// admit() is the typed refusal point: audio_mic_disabled while disabled.
class MicrophoneGate final {
public:
    [[nodiscard]] MicrophoneState state() const noexcept { return state_; }

    void enable_explicitly() noexcept { state_ = MicrophoneState::enabled; }
    void disable() noexcept { state_ = MicrophoneState::disabled; }

    [[nodiscard]] core::Result<void> admit() const {
        if (state_ != MicrophoneState::enabled) {
            return Error{ErrorCode::audio_mic_disabled};
        }
        return {};
    }

private:
    MicrophoneState state_{MicrophoneState::disabled};
};

} // namespace aa::audio
