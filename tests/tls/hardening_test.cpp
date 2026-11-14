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
    Bundle(X509* certificate, EVP_PKEY* key, bool trailing = false, X509* intermediate = nullptr) {
        const char* temporary = std::getenv("TMPDIR");
        path = std::string(temporary ? temporary : "/tmp") + "/tls-check-XXXXXX";
        const int fd = ::mkstemp(path.data());
        if (fd < 0) throw std::runtime_error("test-file-create");
        FILE* file = ::fdopen(fd, "w");
        if (!file) { ::close(fd); throw std::runtime_error("test-file-open"); }
        EXPECT_EQ(PEM_write_X509(file, certificate), 1);
        if (intermediate) { EXPECT_EQ(PEM_write_X509(file, intermediate), 1); }
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
#include <sys/socket.h>
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

#include "../transport/PendingIo.hpp"
TEST(TlsHardening, SilentPeerHitsHandshakeDeadline) {
    namespace tr = aa::transport;
    auto pair = tr::make_tls_loopback([] { return true; },
        tr::TlsMode::encryption_only_compatibility);
    ASSERT_TRUE(pair);
    const auto start = std::chrono::steady_clock::now();
    const auto result = pair.value().client->open({});
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code(), aa::ErrorCode::timeout);
    EXPECT_GE(std::chrono::steady_clock::now() - start, std::chrono::seconds{9});
    EXPECT_LT(std::chrono::steady_clock::now() - start, std::chrono::seconds{12});
}

TEST(TlsHardening, IdlePeerHitsDerivedDeadlineWithInjectedClock) {
    namespace tr = aa::transport;
    using namespace std::chrono_literals;
    auto pair = tr::make_tls_loopback([] { return true; },
        tr::TlsMode::encryption_only_compatibility);
    ASSERT_TRUE(pair);
    aa::core::CancellationSource cancellation;
    bool client_open = false;
    std::thread thread([&] { client_open = pair.value().client->open(cancellation.get_token()).has_value(); });
    const auto server_open = pair.value().server->open({});
    thread.join();
    ASSERT_TRUE(client_open && server_open.has_value());
    const auto epoch = std::chrono::steady_clock::now();
    std::atomic<int> seconds{0};
    ASSERT_TRUE(pair.value().client->set_peer_ping_interval(30s,
        [&] { return epoch + std::chrono::seconds{seconds.load()}; }));
    PendingIo pending([&] { return pair.value().client->receive(); });
    EXPECT_TRUE(pending.blocked(SYS_poll));
    seconds = 99; // three 30s intervals + 10s margin: still healthy
    EXPECT_NE(pending.result.wait_for(100ms), std::future_status::ready);
    seconds = 101;
    const bool timed_out = pending.result.wait_for(500ms) == std::future_status::ready;
    // Bound the negative control against the old infinite wait.
    if (!timed_out) cancellation.request_stop();
    const auto result = pending.result.get();
    EXPECT_TRUE(timed_out);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code(), aa::ErrorCode::timeout);
    EXPECT_EQ(pair.value().client->receive().error().code(), aa::ErrorCode::transport_closed);
}

TEST(TlsHardening, EmptyListenerAcceptTimesOutAndCancels) {
    namespace tr = aa::transport;
    using namespace std::chrono_literals;
    auto listener = tr::TlsListener::listen("127.0.0.1", 0,
        [](const tr::TlsPeerInfo&) { return true; },
        tr::TlsMode::encryption_only_compatibility);
    ASSERT_TRUE(listener);
    const auto timeout = listener.value()->accept({std::chrono::steady_clock::now() + 20ms, {}});
    ASSERT_FALSE(timeout);
    EXPECT_EQ(timeout.error().code(), aa::ErrorCode::timeout);
    aa::core::CancellationSource cancellation;
    PendingIo pending([&] {
        return listener.value()->accept({std::chrono::steady_clock::now() + 10s, cancellation.get_token()});
    });
    EXPECT_TRUE(pending.blocked(SYS_poll));
    cancellation.request_stop();
    ASSERT_EQ(pending.result.wait_for(1s), std::future_status::ready);
    const auto result = pending.result.get();
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code(), aa::ErrorCode::cancelled);
    auto connect = tr::TlsTransport::connect("127.0.0.1", listener.value()->port(),
        [] { return true; }, tr::TlsMode::encryption_only_compatibility,
        {std::chrono::steady_clock::now() + 10s, cancellation.get_token()});
    ASSERT_FALSE(connect);
    EXPECT_EQ(connect.error().code(), aa::ErrorCode::cancelled);
}

TEST(TlsHardening, RejectsWeakIntermediatePublicKeyWithStrongSignature) {
    auto leaf = aa::tls::load_credentials();
    aa::tls::Certificate intermediate(X509_dup(leaf.certificate.get()), X509_free);
    std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)> context(
        EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr), EVP_PKEY_CTX_free);
    ASSERT_EQ(EVP_PKEY_keygen_init(context.get()), 1);
    ASSERT_EQ(EVP_PKEY_CTX_set_rsa_keygen_bits(context.get(), 1024), 1);
    EVP_PKEY* generated = nullptr;
    ASSERT_EQ(EVP_PKEY_keygen(context.get(), &generated), 1);
    aa::tls::PrivateKey weak(generated, EVP_PKEY_free);
    ASSERT_EQ(X509_set_pubkey(intermediate.get(), weak.get()), 1);
    ASSERT_EQ(X509_NAME_add_entry_by_txt(X509_get_subject_name(intermediate.get()),
        "CN", MBSTRING_ASC, reinterpret_cast<const unsigned char*>("synthetic-intermediate"),
        -1, -1, 0), 1);
    ASSERT_EQ(X509_set_issuer_name(leaf.certificate.get(),
        X509_get_subject_name(intermediate.get())), 1);
    ASSERT_GT(X509_sign(leaf.certificate.get(), weak.get(), EVP_sha256()), 0);
    // A strong issuer signs the weak subject key: signature strength alone passes.
    ASSERT_GT(X509_sign(intermediate.get(), leaf.private_key.get(), EVP_sha256()), 0);
    Bundle bundle(leaf.certificate.get(), leaf.private_key.get(), false, intermediate.get());
    try {
        (void)aa::tls::load_credentials();
        FAIL() << "weak intermediate was accepted";
    } catch (const aa::tls::Error& error) {
        EXPECT_EQ(error.failure(), aa::tls::Failure::insecure);
        EXPECT_STREQ(error.what(),
            "tls-credential-insecure: certificate[1] public-key-below-security-floor");
    }
}

TEST(TlsHardening, AcceptsStrongLeafAndIntermediate) {
    auto leaf = aa::tls::load_credentials();
    auto issuer = aa::tls::load_credentials();
    ASSERT_EQ(X509_NAME_add_entry_by_txt(X509_get_subject_name(issuer.certificate.get()),
        "CN", MBSTRING_ASC, reinterpret_cast<const unsigned char*>("synthetic-strong-intermediate"),
        -1, -1, 0), 1);
    ASSERT_GT(X509_sign(issuer.certificate.get(), issuer.private_key.get(), EVP_sha256()), 0);
    ASSERT_EQ(X509_set_issuer_name(leaf.certificate.get(),
        X509_get_subject_name(issuer.certificate.get())), 1);
    ASSERT_GT(X509_sign(leaf.certificate.get(), issuer.private_key.get(), EVP_sha256()), 0);
    Bundle bundle(leaf.certificate.get(), leaf.private_key.get(), false, issuer.certificate.get());
    auto loaded = aa::tls::load_credentials();
    ASSERT_EQ(loaded.chain.size(), 1u);
    EXPECT_EQ(X509_NAME_cmp(X509_get_subject_name(loaded.chain.front().get()),
                            X509_get_issuer_name(loaded.certificate.get())), 0);
    EXPECT_EQ(X509_verify(loaded.certificate.get(),
                          X509_get0_pubkey(loaded.chain.front().get())), 1);
}

#include "../../src/transport/tls_adapter/TlsStream.hpp"
namespace aa::transport {
struct TlsTransportTestAccess {
    static SSL* ssl(TlsTransport& transport) { return transport.impl_->ssl; }
    static int fd(TlsTransport& transport) { return transport.impl_->fd; }
};
}

namespace {
std::atomic<std::size_t> decrypted_bytes{0};
}
extern "C" int __real_SSL_read(SSL*, void*, int);
extern "C" int __wrap_SSL_read(SSL* ssl, void* buffer, int size) {
    const int count = __real_SSL_read(ssl, buffer, size);
    if (count > 0) decrypted_bytes += static_cast<std::size_t>(count);
    return count;
}

namespace {
class FrameDeadlineTest : public testing::Test {
protected:
    using Clock = std::chrono::steady_clock;
    aa::transport::TlsLoopbackPair pair;
    aa::core::CancellationSource cancellation;
    Clock::time_point epoch{Clock::now()};
    std::atomic<int> milliseconds{0};

    void SetUp() override {
        namespace tr = aa::transport;
        auto made = tr::make_tls_loopback([] { return true; },
            tr::TlsMode::encryption_only_compatibility);
        ASSERT_TRUE(made);
        pair = std::move(made.value());
        bool client_open = false;
        std::thread worker([&] {
            client_open = pair.client->open(cancellation.get_token()).has_value();
        });
        const auto opened = pair.server->open({});
        worker.join();
        ASSERT_TRUE(client_open && opened.has_value());
        ASSERT_TRUE(pair.client->set_peer_ping_interval(std::chrono::seconds{30},
            [&] { return epoch + std::chrono::milliseconds{milliseconds.load()}; }));
        decrypted_bytes = 0;
    }

    void fragment(std::span<const std::byte> bytes) {
        ASSERT_EQ(SSL_write(aa::transport::TlsTransportTestAccess::ssl(*pair.server),
                            bytes.data(), static_cast<int>(bytes.size())),
                  static_cast<int>(bytes.size()));
    }

    template<class Pending>
    bool consumed(Pending& pending, std::size_t bytes) {
        const auto until = Clock::now() + std::chrono::seconds{2};
        while (Clock::now() < until) {
            if (decrypted_bytes >= bytes && pending.blocked_in_poll()) return true;
            if (pending.result.wait_for(std::chrono::seconds{0}) == std::future_status::ready) return false;
            std::this_thread::yield();
        }
        return false;
    }

    template<class Pending>
    bool ready(Pending& pending) {
        const bool done = pending.result.wait_for(std::chrono::seconds{1}) == std::future_status::ready;
        // Negative controls must finish too; the successful path never cancels.
        if (!done) cancellation.request_stop();
        return done;
    }
};
}

TEST_F(FrameDeadlineTest, FirstByteBeforeIdleExpiryGetsFullFrameBudget) {
    using namespace std::chrono_literals;
    const std::vector<std::byte> frame{std::byte{0}, std::byte{3}, std::byte{0},
                                      std::byte{1}, std::byte{42}};
    PendingIo pending([&] { return pair.client->receive(); });
    EXPECT_TRUE(pending.blocked_in_poll());
    milliseconds = 99999; // idle expiry is 100s
    fragment(std::span(frame).first(1));
    EXPECT_TRUE(consumed(pending, 1));
    milliseconds = 100001;
    EXPECT_NE(pending.result.wait_for(100ms), std::future_status::ready);
    milliseconds = 129998; // just inside the new 30s frame budget
    fragment(std::span(frame).subspan(1));
    EXPECT_TRUE(ready(pending));
    const auto received = pending.result.get();
    ASSERT_TRUE(received);
    EXPECT_EQ(received.value(), frame);
}

TEST_F(FrameDeadlineTest, FragmentTrickleCannotExtendWholeFrameBudget) {
    using namespace std::chrono_literals;
    const std::vector<std::byte> frame{std::byte{0}, std::byte{3}, std::byte{0},
                                      std::byte{100}};
    PendingIo pending([&] { return pair.client->receive(); });
    EXPECT_TRUE(pending.blocked_in_poll());
    fragment(frame);
    EXPECT_TRUE(consumed(pending, frame.size()));
    const std::byte byte{42};
    for (int second = 1; second < 30; ++second) {
        milliseconds = second * 1000;
        fragment(std::span(&byte, 1));
        EXPECT_TRUE(consumed(pending, frame.size() + static_cast<std::size_t>(second)));
    }
    EXPECT_NE(pending.result.wait_for(0ms), std::future_status::ready);
    milliseconds = 30000;
    EXPECT_TRUE(ready(pending));
    const auto received = pending.result.get();
    ASSERT_FALSE(received);
    EXPECT_EQ(received.error().code(), aa::ErrorCode::timeout);
    EXPECT_EQ(pair.client->receive().error().code(), aa::ErrorCode::transport_closed);
}

TEST_F(FrameDeadlineTest, BackpressuredSendExpiresWithoutCancellation) {
    namespace tr = aa::transport;
    const int small = 4096;
    const int sender = tr::TlsTransportTestAccess::fd(*pair.client);
    ASSERT_EQ(::setsockopt(sender, SOL_SOCKET, SO_SNDBUF, &small, sizeof(small)), 0);
    ASSERT_EQ(::setsockopt(tr::TlsTransportTestAccess::fd(*pair.server), SOL_SOCKET,
                           SO_RCVBUF, &small, sizeof(small)), 0);
    std::vector<std::byte> frame(16004, std::byte{42});
    frame[0] = std::byte{0}; frame[1] = std::byte{3};
    frame[2] = std::byte{0x3e}; frame[3] = std::byte{0x80}; // 16000 payload bytes
    PendingIo pending([&]() -> aa::core::Result<void> {
        for (;;) {
            auto sent = pair.client->send(frame);
            if (!sent) return sent;
        }
    });
    EXPECT_TRUE(pending.blocked_in_poll(sender));
    milliseconds = 30000;
    EXPECT_TRUE(ready(pending));
    const auto sent = pending.result.get();
    ASSERT_FALSE(sent);
    EXPECT_EQ(sent.error().code(), aa::ErrorCode::timeout);
    EXPECT_FALSE(cancellation.get_token().stop_requested());
    EXPECT_EQ(pair.client->send(frame).error().code(), aa::ErrorCode::transport_closed);
}
