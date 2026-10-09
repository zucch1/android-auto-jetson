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
