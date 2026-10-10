// SPDX-License-Identifier: GPL-3.0-or-later
#include <aa/core/Error.hpp>

namespace aa {

std::string_view Error::message() const noexcept {
    switch (code_) {
    case ErrorCode::invalid_argument: return "invalid argument";
    case ErrorCode::cancelled: return "operation cancelled";
    case ErrorCode::timeout: return "operation timed out";
    case ErrorCode::internal: return "internal error";

    case ErrorCode::transport_unavailable: return "transport unavailable";
    case ErrorCode::transport_closed: return "transport closed";
    case ErrorCode::transport_io: return "transport io failure";
    case ErrorCode::transport_malformed_frame: return "malformed transport frame";
    case ErrorCode::transport_oversize_frame: return "oversize transport frame";
    case ErrorCode::transport_unauthorized_peer: return "unauthorized transport peer";
    case ErrorCode::transport_queue_full: return "transport queue full";

    case ErrorCode::protocol_version_mismatch: return "protocol version mismatch";
    case ErrorCode::protocol_unsupported_channel: return "unsupported protocol channel";
    case ErrorCode::protocol_negotiation_failed: return "protocol negotiation failed";
    case ErrorCode::protocol_malformed_message: return "malformed protocol message";

    case ErrorCode::session_illegal_transition: return "illegal session transition";
    case ErrorCode::session_pairing_timeout: return "session pairing timeout";
    case ErrorCode::session_unknown_phone: return "session rejected unknown phone";
    case ErrorCode::session_not_active: return "session not active";

    case ErrorCode::channel_not_registered: return "channel service not registered";
    case ErrorCode::channel_unsupported_capability: return "unsupported channel capability";
    case ErrorCode::channel_dispatch_failed: return "channel dispatch failed";
    case ErrorCode::channel_caps_unsupported: return "unsupported decode caps";
    case ErrorCode::channel_decode_unavailable: return "decode backend unavailable";
    case ErrorCode::channel_decode_failed: return "decode pipeline failure";

    case ErrorCode::ipc_consumer_rejected: return "ipc consumer rejected";
    case ErrorCode::ipc_peer_unauthorized: return "ipc peer unauthorized";
    case ErrorCode::ipc_queue_full: return "ipc queue full";
    case ErrorCode::ipc_malformed_request: return "malformed ipc request";

    case ErrorCode::audio_sink_unavailable: return "audio sink unavailable";
    case ErrorCode::audio_format_unsupported: return "unsupported audio format";
    case ErrorCode::audio_underrun: return "audio underrun";

    case ErrorCode::trust_store_io: return "trust store io failure";
    case ErrorCode::trust_unknown_phone: return "unknown phone not trusted";
    case ErrorCode::trust_rejected: return "phone rejected by trust store";

    case ErrorCode::config_invalid: return "invalid configuration";
    case ErrorCode::config_jurisdiction_unconfirmed: return "wireless jurisdiction unconfirmed";

    case ErrorCode::diagnostics_sink_failed: return "diagnostics sink failure";

    case ErrorCode::helper_unavailable: return "system helper unavailable";
    case ErrorCode::helper_rejected: return "system helper rejected request";
    case ErrorCode::helper_protocol_mismatch: return "system helper protocol mismatch";
    }
    return "unknown error";
}

std::string_view to_string(ErrorCode code) noexcept {
    switch (code) {
    case ErrorCode::invalid_argument: return "invalid_argument";
    case ErrorCode::cancelled: return "cancelled";
    case ErrorCode::timeout: return "timeout";
    case ErrorCode::internal: return "internal";

    case ErrorCode::transport_unavailable: return "transport_unavailable";
    case ErrorCode::transport_closed: return "transport_closed";
    case ErrorCode::transport_io: return "transport_io";
    case ErrorCode::transport_malformed_frame: return "transport_malformed_frame";
    case ErrorCode::transport_oversize_frame: return "transport_oversize_frame";
    case ErrorCode::transport_unauthorized_peer: return "transport_unauthorized_peer";
    case ErrorCode::transport_queue_full: return "transport_queue_full";

    case ErrorCode::protocol_version_mismatch: return "protocol_version_mismatch";
    case ErrorCode::protocol_unsupported_channel: return "protocol_unsupported_channel";
    case ErrorCode::protocol_negotiation_failed: return "protocol_negotiation_failed";
    case ErrorCode::protocol_malformed_message: return "protocol_malformed_message";

    case ErrorCode::session_illegal_transition: return "session_illegal_transition";
    case ErrorCode::session_pairing_timeout: return "session_pairing_timeout";
    case ErrorCode::session_unknown_phone: return "session_unknown_phone";
    case ErrorCode::session_not_active: return "session_not_active";

    case ErrorCode::channel_not_registered: return "channel_not_registered";
    case ErrorCode::channel_unsupported_capability: return "channel_unsupported_capability";
    case ErrorCode::channel_dispatch_failed: return "channel_dispatch_failed";
    case ErrorCode::channel_caps_unsupported: return "channel_caps_unsupported";
    case ErrorCode::channel_decode_unavailable: return "channel_decode_unavailable";
    case ErrorCode::channel_decode_failed: return "channel_decode_failed";

    case ErrorCode::ipc_consumer_rejected: return "ipc_consumer_rejected";
    case ErrorCode::ipc_peer_unauthorized: return "ipc_peer_unauthorized";
    case ErrorCode::ipc_queue_full: return "ipc_queue_full";
    case ErrorCode::ipc_malformed_request: return "ipc_malformed_request";

    case ErrorCode::audio_sink_unavailable: return "audio_sink_unavailable";
    case ErrorCode::audio_format_unsupported: return "audio_format_unsupported";
    case ErrorCode::audio_underrun: return "audio_underrun";

    case ErrorCode::trust_store_io: return "trust_store_io";
    case ErrorCode::trust_unknown_phone: return "trust_unknown_phone";
    case ErrorCode::trust_rejected: return "trust_rejected";

    case ErrorCode::config_invalid: return "config_invalid";
    case ErrorCode::config_jurisdiction_unconfirmed: return "config_jurisdiction_unconfirmed";

    case ErrorCode::diagnostics_sink_failed: return "diagnostics_sink_failed";

    case ErrorCode::helper_unavailable: return "helper_unavailable";
    case ErrorCode::helper_rejected: return "helper_rejected";
    case ErrorCode::helper_protocol_mismatch: return "helper_protocol_mismatch";
    }
    return "unknown";
}

std::string_view to_string(ErrorDomain domain) noexcept {
    switch (domain) {
    case ErrorDomain::core: return "core";
    case ErrorDomain::transport: return "transport";
    case ErrorDomain::protocol: return "protocol";
    case ErrorDomain::session: return "session";
    case ErrorDomain::channels: return "channels";
    case ErrorDomain::ipc: return "ipc";
    case ErrorDomain::audio: return "audio";
    case ErrorDomain::trust: return "trust";
    case ErrorDomain::config: return "config";
    case ErrorDomain::diagnostics: return "diagnostics";
    case ErrorDomain::helper: return "helper";
    }
    return "unknown";
}

} // namespace aa
