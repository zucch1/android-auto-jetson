// SPDX-License-Identifier: GPL-3.0-or-later
#include "TlsStream.hpp"
#include "../socket_util.hpp"

#include <openssl/err.h>
#include <utility>

namespace aa::transport {

TlsTransport::Impl::Impl(int socket_fd, aa::tls::Policy pol)
    : fd(socket_fd), policy(std::move(pol)),
      io([this](std::span<std::byte> buffer, const core::CancellationToken& token) {
             return ssl_read(buffer, token);
         },
         [this](std::span<const std::byte> data, const core::CancellationToken& token) {
             return ssl_write(data, token);
         }) {}

TlsTransport::Impl::~Impl() { close_once(); }

void TlsTransport::Impl::close_once() noexcept {
    handshaked = false;
    io.close();
    // Terminal teardown is abortive: no close_notify retry or SSL I/O after a
    // failed/cancelled operation. Free the non-owning BIO before closing its fd.
    SSL_free(std::exchange(ssl, nullptr));
    SSL_CTX_free(std::exchange(ctx, nullptr));
    socket_util::close_fd(fd);
}

core::Result<std::size_t> TlsTransport::Impl::ssl_read(
    std::span<std::byte> buffer, const core::CancellationToken& token) {
    for (;;) {
        if (token.stop_requested()) {
            return Error{ErrorCode::cancelled};
        }
        ERR_clear_error();
        const int count = SSL_read(ssl, buffer.data(), static_cast<int>(buffer.size()));
        if (count > 0) {
            return static_cast<std::size_t>(count);
        }
        const int error = SSL_get_error(ssl, count);
        switch (error) {
        case SSL_ERROR_ZERO_RETURN:
            return std::size_t{0};
        case SSL_ERROR_WANT_READ:
        case SSL_ERROR_WANT_WRITE:
            break;
        default:
            return Error{ErrorCode::transport_io};
        }
        switch (wait_ready(fd, error == SSL_ERROR_WANT_WRITE ? POLLOUT : POLLIN,
                           token, Deadline::max())) {
        case WaitReady::ready:
            continue;
        case WaitReady::cancelled:
            return Error{ErrorCode::cancelled};
        case WaitReady::timeout:
        case WaitReady::failed:
            return Error{ErrorCode::transport_io};
        }
    }
}

core::Result<std::size_t> TlsTransport::Impl::ssl_write(
    std::span<const std::byte> data, const core::CancellationToken& token) {
    for (;;) {
        if (token.stop_requested()) {
            return Error{ErrorCode::cancelled};
        }
        ERR_clear_error();
        const int count = SSL_write(ssl, data.data(), static_cast<int>(data.size()));
        if (count > 0) {
            return static_cast<std::size_t>(count);
        }
        const int error = SSL_get_error(ssl, count);
        if (error != SSL_ERROR_WANT_READ && error != SSL_ERROR_WANT_WRITE) {
            return Error{ErrorCode::transport_io};
        }
        switch (wait_ready(fd, error == SSL_ERROR_WANT_READ ? POLLIN : POLLOUT,
                           token, Deadline::max())) {
        case WaitReady::ready:
            continue;
        case WaitReady::cancelled:
            return Error{ErrorCode::cancelled};
        case WaitReady::timeout:
        case WaitReady::failed:
            return Error{ErrorCode::transport_io};
        }
    }
}

WaitReady TlsTransport::Impl::handshake(const core::CancellationToken& token, Deadline deadline) {
    for (;;) {
        if (token.stop_requested()) {
            return WaitReady::cancelled;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            return WaitReady::timeout;
        }
        ERR_clear_error();
        const int result = SSL_do_handshake(ssl);
        if (result == 1) {
            handshaked = true;
            return WaitReady::ready;
        }
        const int error = SSL_get_error(ssl, result);
        if (error != SSL_ERROR_WANT_READ && error != SSL_ERROR_WANT_WRITE) {
            return WaitReady::failed;
        }
        const auto status = wait_ready(fd, error == SSL_ERROR_WANT_WRITE ? POLLOUT : POLLIN,
                                       token, deadline);
        if (status != WaitReady::ready) {
            return status;
        }
    }
}

} // namespace aa::transport
