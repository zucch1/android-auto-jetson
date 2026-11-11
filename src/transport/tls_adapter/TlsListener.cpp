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
    TlsListener::ApprovedPredicate approved;
    TlsMode mode;
    Impl(int socket_fd, TlsListener::ApprovedPredicate a, TlsMode m) noexcept
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
    return accept({core::Deadline::Clock::now() + std::chrono::seconds{10}, {}});
}

core::Result<std::unique_ptr<TlsTransport>> TlsListener::accept(const core::Deadline& deadline) {
    if (!impl_ || impl_->fd < 0) {
        return Error{ErrorCode::transport_closed};
    }
    if (auto checked = deadline.check(); !checked) return checked.error();
    sockaddr_storage address{};
    socklen_t length = sizeof(address);
    const int fd = su::accept_socket(impl_->fd, reinterpret_cast<sockaddr*>(&address), &length, deadline);
    if (fd < 0) {
        if (auto checked = deadline.check(); !checked) return checked.error();
        return Error{ErrorCode::transport_io};
    }
    char host[NI_MAXHOST]{};
    char service[NI_MAXSERV]{};
    if (::getnameinfo(reinterpret_cast<sockaddr*>(&address), length,
                      host, sizeof(host), service, sizeof(service),
                      NI_NUMERICHOST | NI_NUMERICSERV) != 0) {
        ::close(fd);
        return Error{ErrorCode::transport_io};
    }
    const TlsPeerInfo peer{host, static_cast<std::uint16_t>(std::stoul(service))};
    // Bind immutable socket identity into the policy, rechecked at open and
    // every send/receive so revocation still applies to the same TCP peer.
    auto approved = [predicate = impl_->approved, peer] {
        return predicate && predicate(peer);
    };
    auto impl = TlsTransport::build_impl(fd, std::move(approved), impl_->mode, /*server=*/true);
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
    auto listener = TlsListener::listen("127.0.0.1", 0,
        [approved](const TlsPeerInfo& peer) {
            return peer.address == "127.0.0.1" && approved && approved();
        }, mode);
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
