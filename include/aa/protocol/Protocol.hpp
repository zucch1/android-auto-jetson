// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <aa/core/Ids.hpp>
#include <aa/core/Result.hpp>
#include <aa/core/Time.hpp>

#include <cstddef>
#include <cstdint>
#include <span>

namespace aa::protocol {

// Protocol boundary: the project-owned shape of Android Auto messages and
// channels. This is the surface tasks 15-18 adapt AASDK to.
//
// HARD BOUNDARY (enforced by tests/architecture/boundary_scan.py): no Qt/QML,
// GStreamer, BlueZ, NetworkManager, AASDK, protobuf or OpenSSL type, header or
// symbol may appear here or in any other aa:: public header. External library
// types are converted at private adapters (src/**/*_adapter/); nothing they
// produce crosses this interface unconverted.
enum class ChannelRole {
    video,
    media_audio,
    guidance_audio,
    system_audio,
    speech_audio,
    input,
    microphone,          // never advertised by default (no qualified mic path)
    sensors,             // never advertised by default (no vehicle sensors)
    bluetooth_projection,
    wifi_projection,
    diagnostics,
};

// Negotiable video shape. The bench contract is 1280x720@30 primary with an
// 800x480 fallback (plan "Contract budgets").
struct VideoProfile final {
    std::uint16_t width{};
    std::uint16_t height{};
    std::uint8_t fps{};

    [[nodiscard]] friend constexpr bool operator==(const VideoProfile&,
                                                   const VideoProfile&) noexcept = default;
};

// PCM only until a caller supplies a qualified encoded-audio path.
struct AudioProfile final {
    std::uint32_t sampling_rate{};
    std::uint8_t bits{};
    std::uint8_t channels{};
    [[nodiscard]] friend constexpr bool operator==(const AudioProfile&,
                                                   const AudioProfile&) noexcept = default;
};

// A frame byte is a negotiated Service.id, not a static SDK ChannelId ordinal.
// Byte zero is control and is never accepted by the service router.
struct WireMessageView final {
    std::uint8_t service{};
    std::uint64_t sequence{};
    core::Nanoseconds timestamp{};
    std::span<const std::byte> payload{};
};

// Message identity on the wire side of the adapter. `timestamp` is the common
// monotonic A/V clock (never wall clock).
struct MessageHeader final {
    ChannelRole channel{};
    std::uint64_t sequence{};
    core::Nanoseconds timestamp{};
};

// Non-owning view; the payload is valid only for the duration of the call.
struct MessageView final {
    MessageHeader header{};
    std::span<const std::byte> payload{};
};

// Channel endpoint implemented by channel services (task 18). Invoked on the
// session thread by the dispatcher only; implementations must not block.
class MessageSink {
public:
    virtual ~MessageSink() = default;
    MessageSink() = default;
    MessageSink(const MessageSink&) = delete;
    MessageSink& operator=(const MessageSink&) = delete;
    MessageSink(MessageSink&&) = delete;
    MessageSink& operator=(MessageSink&&) = delete;

    virtual core::Result<void> on_message(const MessageView& message) = 0;
};

} // namespace aa::protocol
