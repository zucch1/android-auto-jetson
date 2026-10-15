// SPDX-License-Identifier: GPL-3.0-or-later
// Control-message adapter (task 15): binds the project-owned control schema to
// the pinned AASDK protobuf code (aap_protobuf target). Every parse checks the
// control message id, proto2 required fields and semantic bounds; malformed
// payloads are rejected with protocol_malformed_message, never trusted.
#include "Framing.hpp"

#include <aap_protobuf/service/control/ControlMessageType.pb.h>
#include <aap_protobuf/service/control/message/AuthResponse.pb.h>
#include <aap_protobuf/service/control/message/ChannelOpenRequest.pb.h>
#include <aap_protobuf/service/control/message/ChannelOpenResponse.pb.h>
#include <aap_protobuf/service/control/message/PingRequest.pb.h>
#include <aap_protobuf/service/control/message/PingResponse.pb.h>

#include <aa/protocol/Messages.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace aa::protocol {
namespace {

namespace ctl = aap_protobuf::service::control::message;

// The project control ids are the wire contract of the pinned schema.
static_assert(static_cast<std::uint16_t>(ControlMessageId::version_request) == 1);
static_assert(static_cast<std::uint16_t>(ControlMessageId::ping_request) ==
              static_cast<std::uint16_t>(ctl::MESSAGE_PING_REQUEST));
static_assert(static_cast<std::uint16_t>(ControlMessageId::ping_response) ==
              static_cast<std::uint16_t>(ctl::MESSAGE_PING_RESPONSE));
static_assert(static_cast<std::uint16_t>(ControlMessageId::channel_open_request) ==
              static_cast<std::uint16_t>(ctl::MESSAGE_CHANNEL_OPEN_REQUEST));
static_assert(static_cast<std::uint16_t>(ControlMessageId::service_discovery_response) ==
              static_cast<std::uint16_t>(ctl::MESSAGE_SERVICE_DISCOVERY_RESPONSE));

// Bound check shared by decode and encode so both sides accept the same value
// space; an out-of-bound payload is rejected with the same typed code.
[[nodiscard]] bool ping_data_in_bounds(const std::vector<std::byte> &data) noexcept {
    return data.size() <= kMaxPingDataBytes;
}

[[nodiscard]] std::vector<std::byte> copy_bytes(const std::string &raw) {
    std::vector<std::byte> bytes;
    bytes.reserve(raw.size());
    for (const char byte : raw) {
        bytes.push_back(static_cast<std::byte>(byte));
    }
    return bytes;
}

[[nodiscard]] std::string copy_string(const std::vector<std::byte> &bytes) {
    std::string raw;
    raw.reserve(bytes.size());
    for (const std::byte byte : bytes) {
        raw.push_back(static_cast<char>(byte));
    }
    return raw;
}

} // namespace

core::Result<VersionRequest> decode_version_request(std::span<const std::byte> payload) {
    const auto body = aasdk_adapter::control_body(payload, ControlMessageId::version_request);
    if (!body) {
        return body.error();
    }
    // Fixed-width body: two big-endian uint16 fields, nothing else.
    if (body.value().size() != 4) {
        return aasdk_adapter::malformed();
    }
    const auto *raw = body.value().data();
    VersionRequest message;
    message.major = static_cast<std::uint16_t>((static_cast<std::uint16_t>(raw[0]) << 8U) |
                                               static_cast<std::uint16_t>(raw[1]));
    message.minor = static_cast<std::uint16_t>((static_cast<std::uint16_t>(raw[2]) << 8U) |
                                               static_cast<std::uint16_t>(raw[3]));
    return message;
}

core::Result<std::vector<std::byte>> encode(const VersionRequest &message) {
    const std::byte body[4] = {
        static_cast<std::byte>((message.major >> 8U) & 0xFFU),
        static_cast<std::byte>(message.major & 0xFFU),
        static_cast<std::byte>((message.minor >> 8U) & 0xFFU),
        static_cast<std::byte>(message.minor & 0xFFU),
    };
    return aasdk_adapter::framed_raw(ControlMessageId::version_request, body);
}

core::Result<PingRequest> decode_ping_request(std::span<const std::byte> payload) {
    const auto body = aasdk_adapter::control_body(payload, ControlMessageId::ping_request);
    if (!body) {
        return body.error();
    }
    ctl::PingRequest wire;
    if (const auto parsed = aasdk_adapter::parse_body(wire, body.value()); !parsed) {
        return parsed.error();
    }
    // proto2 required field, checked explicitly at the boundary.
    if (!wire.has_timestamp()) {
        return aasdk_adapter::malformed();
    }
    PingRequest message;
    message.timestamp = wire.timestamp();
    message.bug_report = wire.bug_report();
    message.data = copy_bytes(wire.data());
    if (!ping_data_in_bounds(message.data)) {
        return aasdk_adapter::malformed();
    }
    return message;
}

core::Result<std::vector<std::byte>> encode(const PingRequest &message) {
    if (!ping_data_in_bounds(message.data)) {
        return aasdk_adapter::malformed();
    }
    ctl::PingRequest wire;
    wire.set_timestamp(message.timestamp);
    wire.set_bug_report(message.bug_report);
    wire.set_data(copy_string(message.data));
    return aasdk_adapter::framed(ControlMessageId::ping_request, wire);
}

core::Result<PingResponse> decode_ping_response(std::span<const std::byte> payload) {
    const auto body = aasdk_adapter::control_body(payload, ControlMessageId::ping_response);
    if (!body) {
        return body.error();
    }
    ctl::PingResponse wire;
    if (const auto parsed = aasdk_adapter::parse_body(wire, body.value()); !parsed) {
        return parsed.error();
    }
    if (!wire.has_timestamp()) {
        return aasdk_adapter::malformed();
    }
    PingResponse message;
    message.timestamp = wire.timestamp();
    message.data = copy_bytes(wire.data());
    if (!ping_data_in_bounds(message.data)) {
        return aasdk_adapter::malformed();
    }
    return message;
}

core::Result<std::vector<std::byte>> encode(const PingResponse &message) {
    if (!ping_data_in_bounds(message.data)) {
        return aasdk_adapter::malformed();
    }
    ctl::PingResponse wire;
    wire.set_timestamp(message.timestamp);
    wire.set_data(copy_string(message.data));
    return aasdk_adapter::framed(ControlMessageId::ping_response, wire);
}

core::Result<ChannelOpenRequest> decode_channel_open_request(std::span<const std::byte> payload) {
    const auto body = aasdk_adapter::control_body(payload, ControlMessageId::channel_open_request);
    if (!body) {
        return body.error();
    }
    ctl::ChannelOpenRequest wire;
    if (const auto parsed = aasdk_adapter::parse_body(wire, body.value()); !parsed) {
        return parsed.error();
    }
    // proto2 required fields, checked explicitly at the boundary.
    if (!wire.has_priority() || !wire.has_service_id()) {
        return aasdk_adapter::malformed();
    }
    ChannelOpenRequest message;
    message.priority = wire.priority();
    message.service = ServiceKey{wire.service_id()};
    // Dynamic Service.id space: strictly positive, never a frame-channel ordinal.
    if (!message.service) {
        return aasdk_adapter::malformed();
    }
    return message;
}

core::Result<std::vector<std::byte>> encode(const ChannelOpenRequest &message) {
    if (!message.service) {
        return aasdk_adapter::malformed();
    }
    ctl::ChannelOpenRequest wire;
    wire.set_priority(message.priority);
    wire.set_service_id(message.service.value);
    return aasdk_adapter::framed(ControlMessageId::channel_open_request, wire);
}

core::Result<ChannelOpenResponse> decode_channel_open_response(std::span<const std::byte> payload) {
    const auto body = aasdk_adapter::control_body(payload, ControlMessageId::channel_open_response);
    if (!body) {
        return body.error();
    }
    ctl::ChannelOpenResponse wire;
    if (const auto parsed = aasdk_adapter::parse_body(wire, body.value()); !parsed) {
        return parsed.error();
    }
    switch (wire.status()) {
    case aap_protobuf::shared::STATUS_SUCCESS:
        return ChannelOpenResponse{ChannelOpenStatus::success};
    case aap_protobuf::shared::STATUS_COMMAND_NOT_SUPPORTED:
        return ChannelOpenResponse{ChannelOpenStatus::unsupported};
    default:
        return aasdk_adapter::malformed();
    }
}

core::Result<std::vector<std::byte>> encode(const ChannelOpenResponse &message) {
    ctl::ChannelOpenResponse wire;
    switch (message.status) {
    case ChannelOpenStatus::success:
        wire.set_status(aap_protobuf::shared::STATUS_SUCCESS);
        break;
    case ChannelOpenStatus::unsupported:
        wire.set_status(aap_protobuf::shared::STATUS_COMMAND_NOT_SUPPORTED);
        break;
    default:
        return aasdk_adapter::malformed();
    }
    return aasdk_adapter::framed(ControlMessageId::channel_open_response, wire);
}

core::Result<VersionResponse> decode_version_response(std::span<const std::byte> payload) {
    const auto body = aasdk_adapter::control_body(payload, ControlMessageId::version_response);
    if (!body) {
        return body.error();
    }
    // Fixed-width raw body prefix: three big-endian uint16 fields. >= v1.6
    // appends a WireConfig tail (conformance D15 / fixture F-B2 - a real S24
    // answered 1.7 with trailing bytes while the receiver requests 1.6), so
    // only the first six bytes are read and trailing bytes are tolerated.
    if (body.value().size() < 6) {
        return aasdk_adapter::malformed();
    }
    const auto *raw = body.value().data();
    VersionResponse message;
    message.major = static_cast<std::uint16_t>((static_cast<std::uint16_t>(raw[0]) << 8U) |
                                               static_cast<std::uint16_t>(raw[1]));
    message.minor = static_cast<std::uint16_t>((static_cast<std::uint16_t>(raw[2]) << 8U) |
                                               static_cast<std::uint16_t>(raw[3]));
    message.status = static_cast<std::uint16_t>((static_cast<std::uint16_t>(raw[4]) << 8U) |
                                                static_cast<std::uint16_t>(raw[5]));
    return message;
}

core::Result<std::vector<std::byte>> encode(const VersionResponse &message) {
    const std::byte body[6] = {
        static_cast<std::byte>((message.major >> 8U) & 0xFFU),
        static_cast<std::byte>(message.major & 0xFFU),
        static_cast<std::byte>((message.minor >> 8U) & 0xFFU),
        static_cast<std::byte>(message.minor & 0xFFU),
        static_cast<std::byte>((message.status >> 8U) & 0xFFU),
        static_cast<std::byte>(message.status & 0xFFU),
    };
    return aasdk_adapter::framed_raw(ControlMessageId::version_response, body);
}

core::Result<EncapsulatedSsl> decode_encapsulated_ssl(std::span<const std::byte> payload) {
    const auto body = aasdk_adapter::control_body(payload, ControlMessageId::encapsulated_ssl);
    if (!body) {
        return body.error();
    }
    return EncapsulatedSsl{std::vector<std::byte>(body.value().begin(), body.value().end())};
}

core::Result<std::vector<std::byte>> encode(const EncapsulatedSsl &message) {
    return aasdk_adapter::framed_raw(ControlMessageId::encapsulated_ssl, message.bytes);
}

core::Result<AuthComplete> decode_auth_complete(std::span<const std::byte> payload) {
    const auto body = aasdk_adapter::control_body(payload, ControlMessageId::auth_complete);
    if (!body) {
        return body.error();
    }
    ctl::AuthResponse wire;
    if (const auto parsed = aasdk_adapter::parse_body(wire, body.value()); !parsed) {
        return parsed.error();
    }
    if (!wire.has_status()) {
        return aasdk_adapter::malformed();
    }
    return AuthComplete{wire.status()};
}

core::Result<std::vector<std::byte>> encode(const AuthComplete &message) {
    ctl::AuthResponse wire;
    wire.set_status(message.status);
    return aasdk_adapter::framed(ControlMessageId::auth_complete, wire);
}

} // namespace aa::protocol
