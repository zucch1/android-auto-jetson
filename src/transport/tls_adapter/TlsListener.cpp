// SPDX-License-Identifier: GPL-3.0-or-later
// TLS server listener and connected-loopback factory (task 16). Accepts one TLS
// peer and hands it a transport that drives a real task-8-policy handshake. The
// socket lives behind a private impl; this adapter is the only place POSIX
// sockets and the OpenSSL-backed endpoint construction appear for the TLS
// server path.

#include <aa/transport/TlsTransport.hpp>

#include "../socket_util.hpp"

#include <utility>

namespace aa::transport {

namespace su = socket_util;

struct TlsListener::Impl final {
    int fd;
    TlsTransport::ApprovedPredicate approved;
    TlsMode mode;
    Impl(int socket_fd, TlsTransport::ApprovedPredicate a, TlsMode m) noexcept
        : fd(socket_fd), approved(std::move(a)), mode(m) {}
    ~Impl() { su::close_fd(fd); }
};

TlsListener::TlsListener(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

TlsListener::~TlsListener() { close(); }

core::Result<std::unique_ptr<TlsListener>>
TlsListener::listen(const char* host, std::uint16_t port,
                    ApprovedPredicate approved, TlsMode mode) {
    const int fd = su::listen_socket(host, port);
    if (fd < 0) {
        return Error{ErrorCode::transport_unavailable};
    }
    return std::unique_ptr<TlsListener>(
        new TlsListener(std::make_unique<Impl>(fd, std::move(approved), mode)));
}

std::uint16_t TlsListener::port() const noexcept {
    return impl_ ? su::bound_port(impl_->fd) : 0;
}

core::Result<std::unique_ptr<TlsTransport>> TlsListener::accept() {
    if (!impl_ || impl_->fd < 0) {
        return Error{ErrorCode::transport_closed};
    }
    const int fd = su::accept_socket(impl_->fd);
    if (fd < 0) {
        return Error{ErrorCode::transport_io};
    }
    auto impl = TlsTransport::build_impl(fd, impl_->approved, impl_->mode, /*server=*/true);
    if (!impl) {
        return impl.error();
    }
    return std::move(impl).value();
}

void TlsListener::close() noexcept {
    if (impl_) {
        su::close_fd(impl_->fd);
    }
}

core::Result<TlsLoopbackPair>
make_tls_loopback(const TlsTransport::ApprovedPredicate& approved, TlsMode mode) {
    auto listener = TlsListener::listen("127.0.0.1", 0, approved, mode);
    if (!listener) {
        return listener.error();
    }
    const std::uint16_t port = listener.value()->port();
    if (port == 0) {
        return Error{ErrorCode::transport_unavailable};
    }
    auto client = TlsTransport::connect("127.0.0.1", port, approved, mode);
    if (!client) {
        return client.error();
    }
    auto server = listener.value()->accept();
    if (!server) {
        return server.error();
    }
    TlsLoopbackPair pair;
    pair.server = std::move(server).value();
    pair.client = std::move(client).value();
    return pair;
}

} // namespace aa::transport
