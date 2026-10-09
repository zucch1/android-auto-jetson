// SPDX-License-Identifier: GPL-3.0-or-later
// Shared task-8 TLS failure -> transport error mapping (task 16). Private to the
// tls_adapter - not a public header.
#pragma once

#include <aa/core/Result.hpp>
#include <aa/tls/Credentials.hpp>

namespace aa::transport::tls_adapter {

// Map a hardened task-8 TLS failure to a project-owned transport error. Unknown
// or revoked peers and rejected handshakes surface as transport_unauthorized_peer
// so the caller fails closed.
[[nodiscard]] inline Error map_tls_error(const aa::tls::Error& error) {
    switch (error.failure()) {
    case aa::tls::Failure::unknown_phone:
    case aa::tls::Failure::peer_rejected:
        return Error{ErrorCode::transport_unauthorized_peer};
    case aa::tls::Failure::inactive:
        return Error{ErrorCode::transport_closed};
    case aa::tls::Failure::configuration:
        return Error{ErrorCode::transport_io};
    default:
        return Error{ErrorCode::transport_malformed_frame};
    }
}

} // namespace aa::transport::tls_adapter
