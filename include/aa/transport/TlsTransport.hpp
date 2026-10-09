// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <aa/core/Cancellation.hpp>
#include <aa/core/Result.hpp>
#include <aa/transport/Transport.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <vector>

namespace aa::transport {

// TLS admission mode. Mirrors the task-8 Policy modes without exposing an
// OpenSSL type in this public header (the concrete Policy is built in the
// private adapter). "encryption_only_compatibility" is the interop mode where
// identity comes from the approved-phone predicate, not from a phone
// certificate; "verified_peer_test_only" additionally verifies the peer cert.
enum class TlsMode { encryption_only_compatibility, verified_peer_test_only };

// Real TLS transport (task 16): moves complete AAP frames over a TLS-protected
// TCP connection. The connection handshake is a genuine TLS handshake reusing
// the hardened task-8 credential (HU_KEY_PATH) and admission policy: an unknown
// or revoked peer fails closed. The socket and OpenSSL live behind a private
// impl; this public header exposes no external type. Frames are the TLS
// application data, so the whole frame stream is encrypted in transit.
//
// SCOPE: this is a generic TLS-over-TCP (stream-encryption) transport. It is
// distinct from the AAP per-frame payload-record codec (TlsRecordCodec under
// src/**/*_adapter/), which seals each frame payload as one TLS record for the
// framed API's ENCRYPTED flag. This transport layer makes no live Android Auto
// protocol-integration claim; that is a later task.
class TlsTransport final : public Transport {
public:
    // Approved-phone predicate: identity is independent of certificates. A
    // false/throwing predicate makes every session step fail closed.
    using ApprovedPredicate = std::function<bool()>;

    [[nodiscard]] static core::Result<std::unique_ptr<TlsTransport>>
    connect(const char* host, std::uint16_t port, ApprovedPredicate approved, TlsMode mode);

    ~TlsTransport() override;

    [[nodiscard]] Kind kind() const noexcept override { return Kind::tls; }

    // Drives one handshake with a whole-operation 10s deadline and cancellation.
    core::Result<void> open(core::CancellationToken cancellation) override;
    core::Result<void> send(std::span<const std::byte> frame) override;
    [[nodiscard]] core::Result<std::vector<std::byte>> receive() override;
    void close() noexcept override;

private:
    friend class TlsListener;

    struct Impl;
    explicit TlsTransport(std::unique_ptr<Impl> impl) noexcept;
    [[nodiscard]] static core::Result<std::unique_ptr<TlsTransport>>
    build_impl(int fd, ApprovedPredicate approved, TlsMode mode, bool server);

    std::unique_ptr<Impl> impl_;
    core::CancellationToken token_{};
};

// A TLS server listener bound to host:port. Accept one peer and handshake it.
class TlsListener final {
public:
    using ApprovedPredicate = TlsTransport::ApprovedPredicate;

    [[nodiscard]] static core::Result<std::unique_ptr<TlsListener>>
    listen(const char* host, std::uint16_t port, ApprovedPredicate approved, TlsMode mode);

    ~TlsListener();

    [[nodiscard]] std::uint16_t port() const noexcept;
    [[nodiscard]] core::Result<std::unique_ptr<TlsTransport>> accept();
    void close() noexcept;

private:
    struct Impl;
    explicit TlsListener(std::unique_ptr<Impl> impl) noexcept;

    std::unique_ptr<Impl> impl_;
};

// A connected pair of TLS transports over a real 127.0.0.1 socket. The handshake
// is not yet run; call open() on both ends (concurrently) to establish it.
struct TlsLoopbackPair final {
    std::unique_ptr<TlsTransport> server;
    std::unique_ptr<TlsTransport> client;
};

[[nodiscard]] core::Result<TlsLoopbackPair>
make_tls_loopback(const TlsTransport::ApprovedPredicate& approved, TlsMode mode);

} // namespace aa::transport
