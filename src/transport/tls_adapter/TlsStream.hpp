// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <aa/transport/TlsTransport.hpp>
#include "../FrameStreamIO.hpp"
#include "tls_util.hpp"

namespace aa::transport {

struct TlsTransport::Impl final {
    int fd;
    aa::tls::Policy policy;
    SSL_CTX* ctx{nullptr};
    SSL* ssl{nullptr};
    bool handshaked{false};
    FrameStreamIO io;
    std::function<Deadline()> now{[] { return std::chrono::steady_clock::now(); }};
    // Empty-parser idle expiry is always finite; see set_peer_ping_interval.
    std::chrono::milliseconds idle_budget{kDefaultIdleBudget};
    // The read deadline follows the parser's partial frame across receives;
    // writes must not overwrite it while an inbound frame is buffered.
    Deadline read_deadline{Deadline::max()};
    Deadline write_deadline{Deadline::max()};

    Impl(int socket_fd, aa::tls::Policy pol);
    ~Impl();
    void close_once() noexcept;
    core::Result<std::size_t> ssl_read(std::span<std::byte> buffer,
                                      const core::CancellationToken& token);
    core::Result<std::size_t> ssl_write(std::span<const std::byte> data,
                                       const core::CancellationToken& token);
    WaitReady handshake(const core::CancellationToken& token, Deadline deadline);
};

} // namespace aa::transport
