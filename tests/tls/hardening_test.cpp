// SPDX-License-Identifier: GPL-3.0-or-later
#include <aa/tls/Credentials.hpp>
#include <aa/tls/Policy.hpp>
#include <gtest/gtest.h>
#include <openssl/err.h>

namespace {
using Context = std::unique_ptr<SSL_CTX, decltype(&SSL_CTX_free)>;
using Connection = std::unique_ptr<SSL, decltype(&SSL_free)>;
}

namespace {
bool verified_handshake(bool present_certificate, bool trust_certificate) {
    auto credentials = aa::tls::load_credentials();
    Context server_ctx(SSL_CTX_new(TLS_method()), SSL_CTX_free);
    Context client_ctx(SSL_CTX_new(TLS_method()), SSL_CTX_free);
    EXPECT_EQ(SSL_CTX_set_max_proto_version(server_ctx.get(), TLS1_2_VERSION), 1);
    EXPECT_EQ(SSL_CTX_use_certificate(server_ctx.get(), credentials.certificate.get()), 1);
    EXPECT_EQ(SSL_CTX_use_PrivateKey(server_ctx.get(), credentials.private_key.get()), 1);
    if (present_certificate) {
        EXPECT_EQ(SSL_CTX_use_certificate(client_ctx.get(), credentials.certificate.get()), 1);
        EXPECT_EQ(SSL_CTX_use_PrivateKey(client_ctx.get(), credentials.private_key.get()), 1);
    }
    if (trust_certificate) {
        EXPECT_EQ(X509_STORE_add_cert(SSL_CTX_get_cert_store(server_ctx.get()), credentials.certificate.get()), 1);
    }
    Connection server(SSL_new(server_ctx.get()), SSL_free);
    Connection client(SSL_new(client_ctx.get()), SSL_free);
    aa::tls::Policy([] { return true; }, aa::tls::Mode::verified_peer_test_only).configure(server.get());
    BIO *a = nullptr, *b = nullptr;
    EXPECT_EQ(BIO_new_bio_pair(&a, 0, &b, 0), 1);
    SSL_set_bio(server.get(), a, a);
    SSL_set_bio(client.get(), b, b);
    SSL_set_accept_state(server.get());
    SSL_set_connect_state(client.get());
    bool rejected = false;
    for (int i = 0; i < 100; ++i) {
        ERR_clear_error();
        (void)SSL_do_handshake(client.get());
        ERR_clear_error();
        const int result = SSL_do_handshake(server.get());
        if (result == 1) break;
        const int error = SSL_get_error(server.get(), result);
        if (error != SSL_ERROR_WANT_READ && error != SSL_ERROR_WANT_WRITE) {
            rejected = true;
            break;
        }
    }
    return !rejected && SSL_is_init_finished(server.get());
}

}
TEST(TlsHardening, VerifiedServerRejectsMissingCertificate) {
    EXPECT_FALSE(verified_handshake(false, false));
}
TEST(TlsHardening, VerifiedServerAcceptsExplicitlyTrustedCertificate) {
    EXPECT_TRUE(verified_handshake(true, true));
}
TEST(TlsHardening, VerifiedServerRejectsUntrustedCertificate) {
    EXPECT_FALSE(verified_handshake(true, false));
}
