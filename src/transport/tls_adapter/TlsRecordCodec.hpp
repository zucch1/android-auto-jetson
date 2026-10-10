// SPDX-License-Identifier: GPL-3.0-or-later
// Private TLS record adapter (task 16): a safe per-frame payload cipher that
// reuses the hardened task-8 credential and admission policy (aa::tls::Policy,
// aa::tls::load_credentials) and seals/opens each frame payload as exactly one
// TLS record. This is the "safe private TLS record integration" that replaces
// the pinned SDK's hard-coded record-overhead subtract (the decrypt-29 flaw):
// every length is validated before any arithmetic, so a short record can never
// underflow. The record overhead is pinned to TLS 1.2 AES-GCM (29 bytes) and
// verified against the negotiated cipher. No external type crosses the public
// PayloadCipher interface - OpenSSL lives only here, under src/**/*_adapter/.
#pragma once

#include <aa/core/Result.hpp>
#include <aa/tls/Credentials.hpp>
#include <aa/tls/Policy.hpp>
#include <aa/transport/Framing.hpp>

#include <cstddef>
#include <memory>
#include <span>
#include <vector>

namespace aa::transport::tls_adapter {

class TlsRecordCodec final : public PayloadCipher {
public:
    enum class Role { client, server };

    // `policy` carries the approved-phone predicate and compatibility mode;
    // `role` selects connect vs accept state. The codec loads the HU credential
    // (HU_KEY_PATH) and refuses to activate for an unapproved or revoked peer.
    TlsRecordCodec(aa::tls::Policy policy, Role role);
    ~TlsRecordCodec() override;

    TlsRecordCodec(const TlsRecordCodec&) = delete;
    TlsRecordCodec& operator=(const TlsRecordCodec&) = delete;
    TlsRecordCodec(TlsRecordCodec&&) = delete;
    TlsRecordCodec& operator=(TlsRecordCodec&&) = delete;

    // Drive the TLS handshake over memory BIOs. Exchange read_handshake() /
    // write_handshake() buffers between the two codecs until is_active() (see
    // pump_handshake below). do_handshake() returns true once the handshake is
    // complete; it throws aa::tls::Error (mapped by the caller) on a rejected
    // peer.
    bool do_handshake();
    [[nodiscard]] std::vector<std::byte> read_handshake();
    void write_handshake(std::span<const std::byte> bytes);

    // PayloadCipher: seal one plaintext chunk into one TLS record; open one TLS
    // record back to its plaintext, validating the record length first.
    [[nodiscard]] core::Result<std::vector<std::byte>>
    seal(std::span<const std::byte> plaintext) override;
    [[nodiscard]] core::Result<std::vector<std::byte>>
    open(std::span<const std::byte> record) override;
    [[nodiscard]] std::size_t record_overhead() const noexcept override;
    [[nodiscard]] bool is_active() const noexcept override;

    // The negotiated TLS compatibility label (task-8 Policy label).
    [[nodiscard]] const char* label() const noexcept;

private:
    struct Ssl;
    std::unique_ptr<Ssl> ssl_;
    aa::tls::Policy policy_;
    Role role_;
    bool active_{false};
    std::size_t overhead_{0};
};

// Exchange handshake buffers between two codecs (one client, one server) until
// both are active. Single-threaded, deterministic: each step pumps one codec's
// outbound handshake bytes into the other's inbound side.
void pump_handshake(TlsRecordCodec& a, TlsRecordCodec& b);

} // namespace aa::transport::tls_adapter
