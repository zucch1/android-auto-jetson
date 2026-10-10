// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <aa/core/Result.hpp>
#include <aa/protocol/Protocol.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <variant>
#include <vector>

namespace aa::protocol {

// Project-owned Android Auto message schema (task 15). The private adapter in
// src/protocol/aasdk_adapter/ binds these types to the pinned AASDK protobuf
// code; no generated or SDK type ever crosses this boundary.
//
// THREE DISTINCT ID SPACES (never interchangeable):
// 1. Static SDK ChannelId ordinal: reference mapping only
//    (`channel_role_from_wire`), NEVER used to route negotiated frames.
// 2. Dynamic Service.id: a per-session identity the head unit allocates in
//    ServiceDiscoveryResponse and the phone echoes in ChannelOpenRequest
//    (ServiceKey), also used as the frame byte (1..255). It is unrelated to
//    the static SDK ordinal; zero is reserved for control.
// 3. Control message ID: the two-byte big-endian discriminator prefixed to
//    every control-channel payload (ControlMessageId).
//
// Wire shape of a control payload: 2-byte big-endian ControlMessageId followed
// by the message body. Body encodings are message-specific: VersionRequest is
// 4 raw bytes (two big-endian uint16); every message below is protobuf.

// Control-channel discriminator. Values are the wire contract; new values are
// appended, existing values never change meaning.
enum class ControlMessageId : std::uint16_t {
    version_request = 1,
    version_response = 2,
    encapsulated_ssl = 3,
    auth_complete = 4,
    service_discovery_request = 5,
    service_discovery_response = 6,
    channel_open_request = 7,
    channel_open_response = 8,
    channel_close_notification = 9,
    ping_request = 11,
    ping_response = 12,
    nav_focus_request = 13,
    nav_focus_notification = 14,
    byebye_request = 15,
    byebye_response = 16,
    voice_session_notification = 17,
    audio_focus_request = 18,
    audio_focus_notification = 19,
    car_connected_devices_request = 20,
    car_connected_devices_response = 21,
    user_switch_request = 22,
    battery_status_notification = 23,
    call_availability_status = 24,
    user_switch_response = 25,
    service_discovery_update = 26,
    unexpected_message = 255,
    framing_error = 65535,
};

// Dynamic per-session service identity (ID space 2). Head-unit allocated,
// strictly positive; zero is the unset value.
struct ServiceKey final {
    std::int32_t value{};

    [[nodiscard]] constexpr explicit operator bool() const noexcept {
        return value > 0 && value <= 255;
    }

    [[nodiscard]] friend constexpr bool operator==(ServiceKey, ServiceKey) noexcept = default;
};

// Semantic bounds enforced on parse and encode of untrusted wire data.
inline constexpr std::size_t kMaxPingDataBytes = 65'536;
inline constexpr std::size_t kMaxDiscoveredServices = 64;

// MESSAGE_VERSION_REQUEST body: two big-endian uint16 version fields.
struct VersionRequest final {
    std::uint16_t major{};
    std::uint16_t minor{};

    [[nodiscard]] friend constexpr bool operator==(const VersionRequest&,
                                                   const VersionRequest&) noexcept = default;
};

// MESSAGE_PING_REQUEST body: protobuf, `timestamp` required on the wire.
struct PingRequest final {
    std::int64_t timestamp{};
    bool bug_report{};
    std::vector<std::byte> data{};

    [[nodiscard]] friend bool operator==(const PingRequest&, const PingRequest&) = default;
};

// MESSAGE_PING_RESPONSE body: protobuf, `timestamp` required on the wire.
struct PingResponse final {
    std::int64_t timestamp{};
    std::vector<std::byte> data{};

    [[nodiscard]] friend bool operator==(const PingResponse&, const PingResponse&) = default;
};

// MESSAGE_CHANNEL_OPEN_REQUEST body: protobuf, `priority` and `service_id`
// required. `service` is the dynamic Service.id (ID space 2), never a
// frame-channel ordinal.
struct ChannelOpenRequest final {
    std::int32_t priority{};
    ServiceKey service{};

    [[nodiscard]] friend constexpr bool operator==(const ChannelOpenRequest&,
                                                   const ChannelOpenRequest&) noexcept = default;
};

enum class ChannelOpenStatus { success, unsupported };
struct ChannelOpenResponse final {
    ChannelOpenStatus status{ChannelOpenStatus::unsupported};
    [[nodiscard]] friend constexpr bool operator==(const ChannelOpenResponse&,
                                                   const ChannelOpenResponse&) noexcept = default;
};

struct VideoConfiguration final {
    std::vector<VideoProfile> profiles{}; // ordered backend-supported preference
    [[nodiscard]] friend bool operator==(const VideoConfiguration&,
                                        const VideoConfiguration&) = default;
};
struct AudioConfiguration final {
    std::vector<AudioProfile> profiles{};
    [[nodiscard]] friend bool operator==(const AudioConfiguration&,
                                        const AudioConfiguration&) = default;
};
struct ButtonConfiguration final {
    std::vector<std::int32_t> keycodes{}; // actual non-touch buttons, no defaults
    [[nodiscard]] friend bool operator==(const ButtonConfiguration&,
                                        const ButtonConfiguration&) = default;
};
using ServiceConfiguration = std::variant<std::monostate, VideoConfiguration,
                                          AudioConfiguration, ButtonConfiguration>;

// One advertised channel service. `id` is dynamic (ID space 2); `role` is the
// semantic role resolved by the session router, never an ordinal.
struct ServiceDescriptor final {
    ServiceKey id{};
    ChannelRole role{};
    ServiceConfiguration configuration{};

    [[nodiscard]] friend bool operator==(const ServiceDescriptor&,
                                        const ServiceDescriptor&) = default;
};

inline constexpr std::size_t kMaxMediaProfiles = 8;
inline constexpr std::size_t kMaxButtonKeycodes = 64;
[[nodiscard]] core::Result<void> validate_service(const ServiceDescriptor& service);
[[nodiscard]] bool valid_video_profile(VideoProfile profile) noexcept;
[[nodiscard]] bool valid_audio_profile(AudioProfile profile) noexcept;

// MESSAGE_SERVICE_DISCOVERY_RESPONSE body: protobuf, each Service carries a
// required dynamic `id` and exactly one channel-bearing service field.
struct ServiceDiscoveryResponse final {
    std::vector<ServiceDescriptor> services{};

    [[nodiscard]] friend bool operator==(const ServiceDiscoveryResponse&,
                                         const ServiceDiscoveryResponse&) = default;
};

// Decode one control payload (2-byte big-endian ControlMessageId + body).
// Rejects a wrong discriminator, an unparseable protobuf, a missing proto2
// required field and out-of-bound values with protocol_malformed_message.
[[nodiscard]] core::Result<VersionRequest>
decode_version_request(std::span<const std::byte> payload);
[[nodiscard]] core::Result<PingRequest>
decode_ping_request(std::span<const std::byte> payload);
[[nodiscard]] core::Result<PingResponse>
decode_ping_response(std::span<const std::byte> payload);
[[nodiscard]] core::Result<ChannelOpenRequest>
decode_channel_open_request(std::span<const std::byte> payload);
[[nodiscard]] core::Result<ChannelOpenResponse>
decode_channel_open_response(std::span<const std::byte> payload);
[[nodiscard]] core::Result<ServiceDiscoveryResponse>
decode_service_discovery_response(std::span<const std::byte> payload);

// Encode one control payload (discriminator + body). Applies the same
// semantic bounds as decode; an out-of-bound value is rejected rather than
// serialized into a payload our own decode would refuse.
[[nodiscard]] core::Result<std::vector<std::byte>>
encode(const VersionRequest& message);
[[nodiscard]] core::Result<std::vector<std::byte>>
encode(const PingRequest& message);
[[nodiscard]] core::Result<std::vector<std::byte>>
encode(const PingResponse& message);
[[nodiscard]] core::Result<std::vector<std::byte>>
encode(const ChannelOpenRequest& message);
[[nodiscard]] core::Result<std::vector<std::byte>>
encode(const ChannelOpenResponse& message);
[[nodiscard]] core::Result<std::vector<std::byte>>
encode(const ServiceDiscoveryResponse& message);

// Static SDK reference mapping (ID space 1), not a session routing API.
// A role with no static wire channel (diagnostics) and a channel
// with no dispatch role both fail with protocol_unsupported_channel.
[[nodiscard]] core::Result<ChannelRole> channel_role_from_wire(std::uint8_t ordinal);
[[nodiscard]] core::Result<std::uint8_t> wire_from_channel_role(ChannelRole role);

} // namespace aa::protocol
