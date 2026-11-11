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

#include <cstdlib>
#include <string>
#include <unistd.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>

namespace {
struct Bundle {
    std::string previous{std::getenv("HU_KEY_PATH")};
    std::string path;
    Bundle(X509* certificate, EVP_PKEY* key, bool trailing = false) {
        const char* temporary = std::getenv("TMPDIR");
        path = std::string(temporary ? temporary : "/tmp") + "/tls-check-XXXXXX";
        const int fd = ::mkstemp(path.data());
        if (fd < 0) throw std::runtime_error("test-file-create");
        FILE* file = ::fdopen(fd, "w");
        if (!file) { ::close(fd); throw std::runtime_error("test-file-open"); }
        EXPECT_EQ(PEM_write_X509(file, certificate), 1);
        EXPECT_EQ(PEM_write_PrivateKey(file, key, nullptr, nullptr, 0, nullptr, nullptr), 1);
        if (trailing) std::fputs("invalid trailing data", file);
        EXPECT_EQ(std::fclose(file), 0);
        ::setenv("HU_KEY_PATH", path.c_str(), 1);
    }
    ~Bundle() { ::setenv("HU_KEY_PATH", previous.c_str(), 1); ::unlink(path.c_str()); }
};

void expect_insecure() {
    try {
        (void)aa::tls::load_credentials();
        FAIL() << "insecure credential was accepted";
    } catch (const aa::tls::Error& error) {
        EXPECT_EQ(error.failure(), aa::tls::Failure::insecure);
    }
}
}

TEST(TlsHardening, RejectsRsa1024) {
    auto credential = aa::tls::load_credentials();
    std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)> context(
        EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr), EVP_PKEY_CTX_free);
    ASSERT_EQ(EVP_PKEY_keygen_init(context.get()), 1);
    ASSERT_EQ(EVP_PKEY_CTX_set_rsa_keygen_bits(context.get(), 1024), 1);
    EVP_PKEY* generated = nullptr;
    ASSERT_EQ(EVP_PKEY_keygen(context.get(), &generated), 1);
    aa::tls::PrivateKey key(generated, EVP_PKEY_free);
    ASSERT_EQ(X509_set_pubkey(credential.certificate.get(), key.get()), 1);
    ASSERT_GT(X509_sign(credential.certificate.get(), key.get(), EVP_sha256()), 0);
    Bundle bundle(credential.certificate.get(), key.get());
    expect_insecure();
}

TEST(TlsHardening, RejectsSha1Signature) {
    auto credential = aa::tls::load_credentials();
    ASSERT_GT(X509_sign(credential.certificate.get(), credential.private_key.get(), EVP_sha1()), 0);
    Bundle bundle(credential.certificate.get(), credential.private_key.get());
    expect_insecure();
}

TEST(TlsHardening, ExpiredHeadUnitCertificateWarnsAndLoads) {
    auto credential = aa::tls::load_credentials();
    ASSERT_NE(X509_gmtime_adj(X509_getm_notBefore(credential.certificate.get()), -7200), nullptr);
    ASSERT_NE(X509_gmtime_adj(X509_getm_notAfter(credential.certificate.get()), -3600), nullptr);
    ASSERT_GT(X509_sign(credential.certificate.get(), credential.private_key.get(), EVP_sha256()), 0);
    Bundle bundle(credential.certificate.get(), credential.private_key.get());
    testing::internal::CaptureStderr();
    EXPECT_NO_THROW((void)aa::tls::load_credentials());
    EXPECT_EQ(testing::internal::GetCapturedStderr(), "tls-credential-warning: certificate-expired\n");
}

#include <atomic>
namespace {
std::atomic<int> pem_cleanses{0};
std::atomic<bool> cleanse_left_bytes{false};
}
extern "C" void __real_OPENSSL_cleanse(void*, std::size_t);
extern "C" void __wrap_OPENSSL_cleanse(void* buffer, std::size_t size) {
    __real_OPENSSL_cleanse(buffer, size);
    if (size == 65537) {
        const auto* bytes = static_cast<const unsigned char*>(buffer);
        for (std::size_t i = 0; i < size; ++i) {
            if (bytes[i] != 0) cleanse_left_bytes = true;
        }
        ++pem_cleanses;
    }
}

TEST(TlsHardening, CleansesPemOnSuccessfulLoad) {
    const int before = pem_cleanses;
    EXPECT_NO_THROW((void)aa::tls::load_credentials());
    EXPECT_EQ(pem_cleanses.load() - before, 1);
    EXPECT_FALSE(cleanse_left_bytes);
}

TEST(TlsHardening, CleansesPemOnMalformedInput) {
    auto credential = aa::tls::load_credentials();
    Bundle bundle(credential.certificate.get(), credential.private_key.get(), true);
    const int before = pem_cleanses;
    try {
        (void)aa::tls::load_credentials();
        FAIL() << "malformed bundle was accepted";
    } catch (const aa::tls::Error& error) {
        EXPECT_EQ(error.failure(), aa::tls::Failure::malformed);
    }
    EXPECT_EQ(pem_cleanses.load() - before, 1);
    EXPECT_FALSE(cleanse_left_bytes);
}

#include <aa/transport/TlsTransport.hpp>
#include <tuple>

TEST(TlsHardening, ListenerAdmissionReceivesActualPeer) {
    namespace tr = aa::transport;
    std::string observed;
    std::uint16_t observed_port = 0;
    // Variadic only so this regression also compiles against the old API:
    // its missing peer input allowed an unrelated approval to admit anyone.
    auto admission = [&](const auto&... peers) {
        if constexpr (sizeof...(peers) == 1) {
            const auto& peer = std::get<0>(std::tie(peers...));
            observed = peer.address;
            observed_port = peer.port;
            return peer.address == "192.0.2.1";
        }
        return true;
    };
    auto listener = tr::TlsListener::listen("127.0.0.1", 0, admission,
        tr::TlsMode::encryption_only_compatibility);
    ASSERT_TRUE(listener);
    auto client = tr::TlsTransport::connect("127.0.0.1", listener.value()->port(),
        [] { return true; }, tr::TlsMode::encryption_only_compatibility);
    ASSERT_TRUE(client);
    auto server = listener.value()->accept();
    ASSERT_TRUE(server);
    aa::core::CancellationSource cancelled;
    cancelled.request_stop();
    const auto result = server.value()->open(cancelled.get_token());
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code(), aa::ErrorCode::transport_unauthorized_peer);
    EXPECT_EQ(observed, "127.0.0.1");
    EXPECT_GT(observed_port, 0);
}

#include <thread>
TEST(TlsHardening, ListenerRechecksBoundPeerAfterRevocation) {
    namespace tr = aa::transport;
    bool approved = true;
    int checks = 0;
    auto listener = tr::TlsListener::listen("127.0.0.1", 0,
        [&](const tr::TlsPeerInfo& peer) {
            ++checks;
            return approved && peer.address == "127.0.0.1" && peer.port != 0;
        }, tr::TlsMode::encryption_only_compatibility);
    ASSERT_TRUE(listener);
    auto client = tr::TlsTransport::connect("127.0.0.1", listener.value()->port(),
        [] { return true; }, tr::TlsMode::encryption_only_compatibility);
    ASSERT_TRUE(client);
    auto server = listener.value()->accept();
    ASSERT_TRUE(server);
    bool client_open = false;
    std::thread thread([&] { client_open = client.value()->open({}).has_value(); });
    const auto server_open = server.value()->open({});
    thread.join();
    ASSERT_TRUE(client_open && server_open.has_value());
    const int initial = checks;
    approved = false;
    const auto result = server.value()->receive();
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code(), aa::ErrorCode::transport_unauthorized_peer);
    EXPECT_GT(checks, initial);
}
