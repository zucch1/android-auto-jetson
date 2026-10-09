#include <aa/tls/Policy.hpp>
#include <utility>

namespace aa::tls {
Policy::Policy(std::function<bool()> approved_phone, Mode mode)
    : approved_phone_(std::move(approved_phone)), mode_(mode) {}

void Policy::require_approved() const {
    bool approved = false;
    if (approved_phone_) {
        try { approved = approved_phone_(); }
        catch (...) { throw Error(Failure::unknown_phone); }
    }
    if (!approved) { throw Error(Failure::unknown_phone); }
}

void Policy::configure(SSL* ssl) const {
    if (ssl == nullptr) { throw Error(Failure::configuration); }
    if (SSL_set_min_proto_version(ssl, TLS1_2_VERSION) != 1) {
        throw Error(Failure::configuration);
    }
    switch (mode_) {
    case Mode::encryption_only_compatibility:
        // Real phones need not present verifiable peer certificates. This is
        // encryption only; require_approved() is the independent trust gate.
        SSL_set_verify(ssl, SSL_VERIFY_NONE, nullptr);
        return;
    case Mode::verified_peer_test_only:
        SSL_set_verify(ssl, SSL_VERIFY_PEER, nullptr);
        return;
    }
    throw Error(Failure::configuration);
}

const char* Policy::label() const noexcept {
    switch (mode_) {
    case Mode::encryption_only_compatibility: return "encryption-only-compatibility";
    case Mode::verified_peer_test_only: return "verified-peer-test-only";
    }
    return "invalid-tls-mode";
}
}
