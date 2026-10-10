// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <string_view>

namespace aa {

// Module domain a typed error belongs to. Derived from the code's numeric band,
// so diagnostics and logs can attribute failures without carrying extra state.
enum class ErrorDomain { core, transport, protocol, session, channels, ipc,
                         audio, trust, config, diagnostics, helper };

// Stable, project-owned error codes. External failure kinds (errno, OpenSSL,
// AASDK, protobuf) are converted into these codes at private adapter boundaries
// and never cross a public interface. Values are a wire-visible contract: new
// codes are appended inside their domain band (1xx transport, 2xx protocol,
// 3xx session, 4xx channels, 5xx ipc, 6xx audio, 7xx trust, 8xx config,
// 9xx diagnostics, 10xx helper); existing values never change meaning.
enum class ErrorCode : std::uint16_t {
    // core
    invalid_argument = 1,
    cancelled = 2,
    timeout = 3,
    internal = 4,

    // transport
    transport_unavailable = 100,
    transport_closed = 101,
    transport_io = 102,
    transport_malformed_frame = 103,
    transport_oversize_frame = 104,
    transport_unauthorized_peer = 105,
    transport_queue_full = 106,

    // protocol
    protocol_version_mismatch = 200,
    protocol_unsupported_channel = 201,
    protocol_negotiation_failed = 202,
    protocol_malformed_message = 203,

    // session
    session_illegal_transition = 300,
    session_pairing_timeout = 301,
    session_unknown_phone = 302,
    session_not_active = 303,

    // channels
    channel_not_registered = 400,
    channel_unsupported_capability = 401,
    channel_dispatch_failed = 402,
    channel_caps_unsupported = 403,
    channel_decode_unavailable = 404,
    channel_decode_failed = 405,

    // ipc
    ipc_consumer_rejected = 500,
    ipc_peer_unauthorized = 501,
    ipc_queue_full = 502,
    ipc_malformed_request = 503,

    // audio
    audio_sink_unavailable = 600,
    audio_format_unsupported = 601,
    audio_underrun = 602,

    // trust
    trust_store_io = 700,
    trust_unknown_phone = 701,
    trust_rejected = 702,

    // config
    config_invalid = 800,
    config_jurisdiction_unconfirmed = 801,

    // diagnostics
    diagnostics_sink_failed = 900,

    // helper
    helper_unavailable = 1000,
    helper_rejected = 1001,
    helper_protocol_mismatch = 1002,
};

// Maps a code to its module domain by numeric band.
[[nodiscard]] constexpr ErrorDomain domain_of(ErrorCode code) noexcept {
    const auto band = static_cast<std::uint16_t>(code) / 100U;
    switch (band) {
    case 0: return ErrorDomain::core;
    case 1: return ErrorDomain::transport;
    case 2: return ErrorDomain::protocol;
    case 3: return ErrorDomain::session;
    case 4: return ErrorDomain::channels;
    case 5: return ErrorDomain::ipc;
    case 6: return ErrorDomain::audio;
    case 7: return ErrorDomain::trust;
    case 8: return ErrorDomain::config;
    case 9: return ErrorDomain::diagnostics;
    default: return ErrorDomain::helper;
    }
}

// Immutable typed error value. The message is a stable description of the code
// only; dynamic diagnostic data belongs in pre-redacted log fields, never here.
class Error final {
public:
    constexpr explicit Error(ErrorCode code) noexcept : code_(code) {}

    [[nodiscard]] constexpr ErrorCode code() const noexcept { return code_; }
    [[nodiscard]] constexpr ErrorDomain domain() const noexcept { return domain_of(code_); }
    [[nodiscard]] std::string_view message() const noexcept;

    [[nodiscard]] friend constexpr bool operator==(const Error& lhs,
                                                   const Error& rhs) noexcept = default;

private:
    ErrorCode code_;
};

// Stable names for the error and domain enums (logs and diagnostics contract).
[[nodiscard]] std::string_view to_string(ErrorCode code) noexcept;
[[nodiscard]] std::string_view to_string(ErrorDomain domain) noexcept;

} // namespace aa
