// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Private adapter helpers (task 15): control-payload framing and strict
// protobuf parsing. This header lives under src/**/*_adapter/ so its single
// external include (the generated-code runtime base class) is legal here and
// nowhere else; no external type crosses into include/aa/**.

#include <aa/core/Result.hpp>
#include <aa/protocol/Messages.hpp>

#include <google/protobuf/message_lite.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace aa::protocol::aasdk_adapter {

inline constexpr std::size_t kControlMessageIdBytes = 2;
inline constexpr std::size_t kMaxProtoBodyBytes = 1 << 20;

[[nodiscard]] inline Error malformed() noexcept {
    return Error{ErrorCode::protocol_malformed_message};
}

// Split one control payload into (discriminator, body), rejecting a short
// payload or a discriminator other than `expected`.
[[nodiscard]] inline core::Result<std::span<const std::byte>>
control_body(std::span<const std::byte> payload, ControlMessageId expected) {
    if (payload.size() < kControlMessageIdBytes) {
        return malformed();
    }
    const auto raw = static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(payload[0]) << 8U) |
        static_cast<std::uint16_t>(payload[1]));
    if (raw != static_cast<std::uint16_t>(expected)) {
        return malformed();
    }
    return payload.subspan(kControlMessageIdBytes);
}

// Parse one protobuf body strictly: reject oversized bodies, bytes that do not
// parse, and any message missing a proto2 required field.
[[nodiscard]] inline core::Result<void>
parse_body(google::protobuf::MessageLite& message, std::span<const std::byte> body) {
    if (body.size() > kMaxProtoBodyBytes) {
        return malformed();
    }
    if (!message.ParseFromArray(body.data(), static_cast<int>(body.size()))) {
        return malformed();
    }
    if (!message.IsInitialized()) {
        return malformed();
    }
    return {};
}

// Serialize one protobuf body and prefix the big-endian discriminator.
[[nodiscard]] inline core::Result<std::vector<std::byte>>
framed(ControlMessageId id, const google::protobuf::MessageLite& message) {
    std::string body;
    if (!message.SerializeToString(&body)) {
        return malformed();
    }
    std::vector<std::byte> payload;
    payload.reserve(kControlMessageIdBytes + body.size());
    const auto raw = static_cast<std::uint16_t>(id);
    payload.push_back(static_cast<std::byte>((raw >> 8U) & 0xFFU));
    payload.push_back(static_cast<std::byte>(raw & 0xFFU));
    for (const char byte : body) {
        payload.push_back(static_cast<std::byte>(byte));
    }
    return payload;
}

// Prefix a raw (non-protobuf) body with the big-endian discriminator.
[[nodiscard]] inline core::Result<std::vector<std::byte>>
framed_raw(ControlMessageId id, std::span<const std::byte> body) {
    std::vector<std::byte> payload;
    payload.reserve(kControlMessageIdBytes + body.size());
    const auto raw = static_cast<std::uint16_t>(id);
    payload.push_back(static_cast<std::byte>((raw >> 8U) & 0xFFU));
    payload.push_back(static_cast<std::byte>(raw & 0xFFU));
    payload.insert(payload.end(), body.begin(), body.end());
    return payload;
}

} // namespace aa::protocol::aasdk_adapter
