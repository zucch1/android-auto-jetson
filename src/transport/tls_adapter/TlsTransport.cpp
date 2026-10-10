// SPDX-License-Identifier: GPL-3.0-or-later
#include <aa/transport/TlsTransport.hpp>

#include "TlsStream.hpp"
#include "SocketBio.hpp"
#include "tls_error_map.hpp"
#include "../socket_util.hpp"
#include <aa/tls/Credentials.hpp>
#include <fcntl.h>
#include <utility>

namespace aa::transport {

TlsTransport::TlsTransport(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
TlsTransport::~TlsTransport() = default;

core::Result<std::unique_ptr<TlsTransport>>
TlsTransport::build_impl(int fd, ApprovedPredicate approved, TlsMode mode, bool server) {
    auto impl = std::make_unique<Impl>(fd, make_policy(std::move(approved), mode));
    const int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0 || ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        return Error{ErrorCode::transport_io};
    }
    try {
        auto credentials = aa::tls::load_credentials();
        impl->ctx = SSL_CTX_new(TLS_method());
        if (impl->ctx == nullptr) {
            return Error{ErrorCode::transport_io};
        }
        if (SSL_CTX_set_min_proto_version(impl->ctx, TLS1_2_VERSION) != 1 ||
            SSL_CTX_set_max_proto_version(impl->ctx, TLS1_2_VERSION) != 1 ||
            SSL_CTX_use_certificate(impl->ctx, credentials.certificate.get()) != 1 ||
            SSL_CTX_use_PrivateKey(impl->ctx, credentials.private_key.get()) != 1 ||
            SSL_CTX_check_private_key(impl->ctx) != 1) {
            return Error{ErrorCode::transport_unavailable};
        }
        impl->ssl = SSL_new(impl->ctx);
        if (impl->ssl == nullptr) {
            return Error{ErrorCode::transport_io};
        }
        BIO* bio = tls_adapter::SocketBio::create(fd);
        if (bio == nullptr) {
            return Error{ErrorCode::transport_io};
        }
        SSL_set_bio(impl->ssl, bio, bio);
        impl->policy.configure(impl->ssl);
        if (server) {
            SSL_set_accept_state(impl->ssl);
        } else {
            SSL_set_connect_state(impl->ssl);
        }
    } catch (const aa::tls::Error& error) {
        return tls_adapter::map_tls_error(error);
    }
    return std::unique_ptr<TlsTransport>(new TlsTransport(std::move(impl)));
}

core::Result<std::unique_ptr<TlsTransport>>
TlsTransport::connect(const char* host, std::uint16_t port,
                      ApprovedPredicate approved, TlsMode mode) {
    const int fd = socket_util::connect_socket(host, port);
    if (fd < 0) {
        return Error{ErrorCode::transport_unavailable};
    }
    return build_impl(fd, std::move(approved), mode, false);
}

core::Result<void> TlsTransport::open(core::CancellationToken cancellation) {
    if (impl_->fd < 0) {
        return Error{ErrorCode::transport_closed};
    }
    token_ = std::move(cancellation);
    try {
        impl_->policy.require_approved();
    } catch (const aa::tls::Error& error) {
        close();
        return tls_adapter::map_tls_error(error);
    }
    if (token_.stop_requested()) {
        close();
        return Error{ErrorCode::cancelled};
    }
    if (impl_->handshaked) {
        return {};
    }
    const auto status = impl_->handshake(token_, std::chrono::steady_clock::now() + kHandshakeDeadline);
    if (status == WaitReady::ready) {
        return {};
    }
    close();
    switch (status) {
    case WaitReady::cancelled:
        return Error{ErrorCode::cancelled};
    case WaitReady::timeout:
        return Error{ErrorCode::timeout};
    case WaitReady::failed:
        return Error{ErrorCode::transport_unauthorized_peer};
    case WaitReady::ready:
        break;
    }
    return Error{ErrorCode::internal};
}

core::Result<void> TlsTransport::send(std::span<const std::byte> frame) {
    if (!impl_->handshaked) {
        return Error{ErrorCode::transport_closed};
    }
    try {
        impl_->policy.require_approved();
    } catch (const aa::tls::Error& error) {
        close();
        return tls_adapter::map_tls_error(error);
    }
    auto sent = impl_->io.send_frame(frame, token_);
    if (!sent) {
        close();
    }
    return sent;
}

core::Result<std::vector<std::byte>> TlsTransport::receive() {
    if (!impl_->handshaked) {
        return Error{ErrorCode::transport_closed};
    }
    try {
        impl_->policy.require_approved();
    } catch (const aa::tls::Error& error) {
        close();
        return tls_adapter::map_tls_error(error);
    }
    auto got = impl_->io.receive_frame(token_);
    if (!got) {
        close();
    }
    return got;
}

void TlsTransport::close() noexcept { impl_->close_once(); }

} // namespace aa::transport
