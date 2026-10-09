// SPDX-License-Identifier: GPL-3.0-or-later
// Safe TLS record adapter (task 16): seals/opens each frame payload as exactly
// one TLS 1.2 AES-GCM record with a validated length, reusing the hardened task-8
// credential and admission policy. This replaces the pinned SDK's hard-coded
// record-overhead subtract (the decrypt-29 flaw): every length is checked before
// any arithmetic so a short record can never underflow.

#include "TlsRecordCodec.hpp"

#include "tls_error_map.hpp"

#include <cstring>
#include <stdexcept>

#include <openssl/err.h>
#include <openssl/ssl.h>

namespace aa::transport::tls_adapter {
namespace {

// TLS 1.2 AES-GCM record overhead: 5-byte record header + 8-byte explicit nonce
// + 16-byte AEAD tag. Verified against the negotiated cipher before use.
inline constexpr std::size_t kAesGcmRecordOverhead = 29;
inline constexpr std::size_t kTlsRecordHeaderBytes = 5;
inline constexpr std::size_t kAesGcmNonceTagBytes = 24;
inline constexpr std::uint8_t kTlsApplicationData = 23;
inline constexpr std::uint16_t kTls12RecordVersion = 0x0303;

} // namespace

struct TlsRecordCodec::Ssl final {
    SSL_CTX* ctx{nullptr};
    SSL* ssl{nullptr};
    BIO* rbio{nullptr}; // bytes into TLS (peer -> ssl)
    BIO* wbio{nullptr}; // bytes out of TLS (ssl -> peer)

    ~Ssl() {
        // SSL_set_bio transfers BIO ownership to the SSL object; SSL_free
        // releases both BIOs. Only the context is freed separately.
        if (ssl != nullptr) {
            SSL_free(ssl);
        }
        if (ctx != nullptr) {
            SSL_CTX_free(ctx);
        }
    }
};

TlsRecordCodec::TlsRecordCodec(aa::tls::Policy policy, Role role)
    : ssl_(std::make_unique<Ssl>()), policy_(std::move(policy)), role_(role) {
    auto credentials = aa::tls::load_credentials();

    ssl_->ctx = SSL_CTX_new(TLS_method());
    if (ssl_->ctx == nullptr) {
        throw std::runtime_error("tls-record-adapter-context");
    }
    // Pin TLS 1.2 AES-GCM so the record overhead is exactly 29 bytes.
    SSL_CTX_set_min_proto_version(ssl_->ctx, TLS1_2_VERSION);
    SSL_CTX_set_max_proto_version(ssl_->ctx, TLS1_2_VERSION);
    SSL_CTX_set_cipher_list(
        ssl_->ctx,
        "ECDHE-RSA-AES128-GCM-SHA256:ECDHE-RSA-AES256-GCM-SHA384:"
        "AES128-GCM-SHA256:AES256-GCM-SHA384");
    if (SSL_CTX_use_certificate(ssl_->ctx, credentials.certificate.get()) != 1 ||
        SSL_CTX_use_PrivateKey(ssl_->ctx, credentials.private_key.get()) != 1 ||
        SSL_CTX_check_private_key(ssl_->ctx) != 1) {
        throw std::runtime_error("tls-record-adapter-credential");
    }

    ssl_->ssl = SSL_new(ssl_->ctx);
    ssl_->rbio = BIO_new(BIO_s_mem());
    ssl_->wbio = BIO_new(BIO_s_mem());
    if (ssl_->ssl == nullptr || ssl_->rbio == nullptr || ssl_->wbio == nullptr) {
        throw std::runtime_error("tls-record-adapter-ssl");
    }
    SSL_set_bio(ssl_->ssl, ssl_->rbio, ssl_->wbio);
    if (role_ == Role::client) {
        SSL_set_connect_state(ssl_->ssl);
    } else {
        SSL_set_accept_state(ssl_->ssl);
    }
    // Task-8 admission policy sets the verify mode / compatibility label. The
    // approved-phone predicate is enforced separately at every session step.
    policy_.configure(ssl_->ssl);
}

TlsRecordCodec::~TlsRecordCodec() = default;

bool TlsRecordCodec::do_handshake() {
    policy_.require_approved();
    const int result = SSL_do_handshake(ssl_->ssl);
    if (result == 1) {
        const SSL_CIPHER* cipher = SSL_get_current_cipher(ssl_->ssl);
        const char* name = (cipher != nullptr) ? SSL_CIPHER_get_name(cipher) : "";
        if (std::strstr(name, "GCM") == nullptr) {
            // Only AES-GCM is admitted; any other negotiated cipher would make
            // the fixed record overhead unsound.
            throw aa::tls::Error(aa::tls::Failure::configuration);
        }
        overhead_ = kAesGcmRecordOverhead;
        active_ = true;
        return true;
    }
    const int error = SSL_get_error(ssl_->ssl, result);
    if (error == SSL_ERROR_WANT_READ || error == SSL_ERROR_WANT_WRITE) {
        return false;
    }
    active_ = false;
    throw aa::tls::Error(aa::tls::Failure::peer_rejected);
}

std::vector<std::byte> TlsRecordCodec::read_handshake() {
    std::vector<std::byte> out;
    const std::size_t pending = BIO_ctrl_pending(ssl_->wbio);
    if (pending > 0) {
        out.resize(pending);
        BIO_read(ssl_->wbio, out.data(), static_cast<int>(out.size()));
    }
    return out;
}

void TlsRecordCodec::write_handshake(std::span<const std::byte> bytes) {
    if (!bytes.empty()) {
        BIO_write(ssl_->rbio, bytes.data(), static_cast<int>(bytes.size()));
    }
}

core::Result<std::vector<std::byte>>
TlsRecordCodec::seal(std::span<const std::byte> plaintext) {
    if (!active_) {
        return Error{ErrorCode::transport_closed};
    }
    if (plaintext.empty()) {
        return Error{ErrorCode::invalid_argument};
    }
    try {
        policy_.require_approved();
    } catch (const aa::tls::Error& error) {
        active_ = false;
        return map_tls_error(error);
    }
    if (plaintext.size() > kPlainChunkBytes) {
        return Error{ErrorCode::transport_oversize_frame};
    }

    std::size_t sent = 0;
    while (sent < plaintext.size()) {
        const int count = SSL_write(ssl_->ssl, plaintext.data() + sent,
                                    static_cast<int>(plaintext.size() - sent));
        if (count <= 0) {
            active_ = false;
            return Error{ErrorCode::transport_io};
        }
        sent += static_cast<std::size_t>(count);
    }

    std::vector<std::byte> record;
    const std::size_t pending = BIO_ctrl_pending(ssl_->wbio);
    if (pending > 0) {
        record.resize(pending);
        BIO_read(ssl_->wbio, record.data(), static_cast<int>(record.size()));
    }
    // Exact-overhead check: one chunk is exactly one record whose ciphertext is
    // the plaintext plus the AES-GCM record overhead.
    if (record.size() != plaintext.size() + kAesGcmRecordOverhead) {
        active_ = false;
        return Error{ErrorCode::transport_malformed_frame};
    }
    return record;
}

core::Result<std::vector<std::byte>>
TlsRecordCodec::open(std::span<const std::byte> record) {
    if (!active_) {
        return Error{ErrorCode::transport_closed};
    }
    try {
        policy_.require_approved();
    } catch (const aa::tls::Error& error) {
        active_ = false;
        return map_tls_error(error);
    }

    // Validate the record length BEFORE any subtract. This is the safe path the
    // SDK's `frameLength - 29` lacked: a short record is a typed error, never a
    // negative length that wraps to a huge read.
    if (record.size() < kAesGcmRecordOverhead) {
        return Error{ErrorCode::transport_malformed_frame};
    }
    const std::uint8_t* header = reinterpret_cast<const std::uint8_t*>(record.data());
    const std::uint16_t fragment_len =
        static_cast<std::uint16_t>((header[3] << 8U) | header[4]);
    if (header[0] != kTlsApplicationData ||
        header[1] != static_cast<std::uint8_t>(kTls12RecordVersion >> 8U) ||
        header[2] != static_cast<std::uint8_t>(kTls12RecordVersion & 0xFFU) ||
        fragment_len != record.size() - kTlsRecordHeaderBytes ||
        fragment_len < kAesGcmNonceTagBytes) {
        return Error{ErrorCode::transport_malformed_frame};
    }
    const std::size_t plain_len = record.size() - kAesGcmRecordOverhead;
    if (plain_len == 0) {
        return Error{ErrorCode::transport_malformed_frame};
    }

    BIO_write(ssl_->rbio, record.data(), static_cast<int>(record.size()));
    std::vector<std::byte> out(plain_len);
    std::size_t got = 0;
    while (got < plain_len) {
        const int count = SSL_read(ssl_->ssl, out.data() + got,
                                   static_cast<int>(plain_len - got));
        if (count <= 0) {
            // Authenticated decryption failed or the record was truncated.
            active_ = false;
            return Error{ErrorCode::transport_malformed_frame};
        }
        got += static_cast<std::size_t>(count);
    }
    return out;
}

std::size_t TlsRecordCodec::record_overhead() const noexcept {
    return overhead_;
}

bool TlsRecordCodec::is_active() const noexcept {
    return active_;
}

const char* TlsRecordCodec::label() const noexcept {
    return policy_.label();
}

void pump_handshake(TlsRecordCodec& a, TlsRecordCodec& b) {
    // Alternate handshake steps, pumping each codec's outbound records into the
    // other's inbound side, until both complete.
    for (int iteration = 0; iteration < 64 && (!a.is_active() || !b.is_active()); ++iteration) {
        if (!a.is_active()) {
            a.do_handshake();
            b.write_handshake(a.read_handshake());
        }
        if (!b.is_active()) {
            b.do_handshake();
            a.write_handshake(b.read_handshake());
        }
    }
    if (!a.is_active() || !b.is_active()) {
        throw std::runtime_error("tls-record-adapter-handshake-incomplete");
    }
}

} // namespace aa::transport::tls_adapter
