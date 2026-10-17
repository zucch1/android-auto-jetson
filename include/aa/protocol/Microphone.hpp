// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <aa/core/Result.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace aa::protocol {

// Negotiation only: a configured PCM source is not a promise of capture.
struct MicrophoneSetup final {};
struct MicrophoneRequest final {
    bool open{};
    std::optional<bool> anc_enabled{};
    std::optional<bool> ec_enabled{};
    std::optional<std::int32_t> max_unacked{};
};
struct MicrophoneResponse final {
    std::int32_t status{};
};
inline constexpr std::int32_t kMicrophoneCaptureUnsupported = -250;

[[nodiscard]] core::Result<MicrophoneSetup> decode_microphone_setup(std::span<const std::byte>);
[[nodiscard]] core::Result<MicrophoneRequest> decode_microphone_request(std::span<const std::byte>);
[[nodiscard]] core::Result<MicrophoneResponse> decode_microphone_response(std::span<const std::byte>);
[[nodiscard]] core::Result<void> decode_microphone_ack(std::span<const std::byte>);
[[nodiscard]] core::Result<std::vector<std::byte>> encode(const MicrophoneSetup&);
[[nodiscard]] core::Result<std::vector<std::byte>> encode(const MicrophoneRequest&);
[[nodiscard]] core::Result<std::vector<std::byte>> encode(const MicrophoneResponse&);
[[nodiscard]] core::Result<std::vector<std::byte>> encode_microphone_ready();

} // namespace aa::protocol
