#include <aa/tls/Credentials.hpp>
#include <aa/tls/Policy.hpp>
#include <aasdk/Messenger/Cryptor.hpp>
#include <aasdk/Transport/SSLWrapper.hpp>

#include <algorithm>
#include <array>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <openssl/x509v3.h>

namespace {
int reference_index() {
    static const int index = SSL_CTX_get_ex_new_index(0, nullptr, nullptr, nullptr, nullptr);
    return index;
}

int pinned_receiver(int, X509_STORE_CTX* store) {
    if (X509_STORE_CTX_get_error_depth(store) != 0) { return 1; }
    auto* ssl = static_cast<SSL*>(X509_STORE_CTX_get_ex_data(store, SSL_get_ex_data_X509_STORE_CTX_idx()));
    auto* reference = static_cast<X509*>(SSL_CTX_get_ex_data(SSL_get_SSL_CTX(ssl), reference_index()));
    return X509_cmp(X509_STORE_CTX_get_current_cert(store), reference) == 0 ? 1 : 0;
}

void require(bool condition) {
    if (!condition) { throw std::runtime_error("tls-test-operation-failed"); }
}

void transfer_to_server(aasdk::messenger::Cryptor& client, BIO* server_input) {
    const auto bytes = client.readHandshakeBuffer();
    if (!bytes.empty()) {
        require(BIO_write(server_input, bytes.data(), static_cast<int>(bytes.size())) == static_cast<int>(bytes.size()));
    }
}

void transfer_to_client(BIO* server_output, aasdk::messenger::Cryptor& client) {
    std::array<unsigned char, 4096> bytes{};
    while (BIO_ctrl_pending(server_output) > 0) {
        const int count = BIO_read(server_output, bytes.data(), bytes.size());
        require(count > 0);
        client.writeHandshakeBuffer(aasdk::common::DataConstBuffer(bytes.data(), static_cast<size_t>(count)));
    }
}
}

int main(int argc, char** argv) {
    if (argc != 2) { return 2; }
    const std::string_view test(argv[1]);
    try {
        // Given: the actual patched Cryptor and a hermetic memory-BIO TLS server.
        auto credentials = aa::tls::load_credentials();
        bool approved = test != "unknown";
        aa::tls::Policy policy([&approved] { return approved; });
        if (test == "absent") { policy = aa::tls::Policy{}; }
        if (test == "verified-unknown" || test == "verified-selfsigned") {
            policy = aa::tls::Policy([&approved] { return approved; }, aa::tls::Mode::verified_peer_test_only);
        }
        const auto wrapper = std::make_shared<aasdk::transport::SSLWrapper>();
        aasdk::messenger::Cryptor client(wrapper, policy);
        client.init();
        if (test == "inactive") {
            aasdk::common::Data encrypted;
            client.encrypt(encrypted, aasdk::common::DataConstBuffer(static_cast<const void*>(nullptr), 0));
            return 2;
        }
        aa::tls::Certificate server_certificate(X509_dup(credentials.certificate.get()), &X509_free);
        require(server_certificate != nullptr);
        if (test == "verified-selfsigned" || test == "compat-selfsigned") {
            require(X509_set_issuer_name(server_certificate.get(), X509_get_subject_name(server_certificate.get())) == 1);
            require(X509_sign(server_certificate.get(), credentials.private_key.get(), EVP_sha256()) > 0);
            require(X509_check_issued(server_certificate.get(), server_certificate.get()) == X509_V_OK);
        }
        std::unique_ptr<SSL_CTX, decltype(&SSL_CTX_free)> context(SSL_CTX_new(TLS_server_method()), &SSL_CTX_free);
        require(context != nullptr);
        require(SSL_CTX_use_certificate(context.get(), server_certificate.get()) == 1);
        require(SSL_CTX_use_PrivateKey(context.get(), credentials.private_key.get()) == 1);
        require(SSL_CTX_check_private_key(context.get()) == 1);
        require(SSL_CTX_set_ex_data(context.get(), reference_index(), credentials.certificate.get()) == 1);
        // This server pins the public receiver reference, not a phone trust store.
        SSL_CTX_set_verify(context.get(), SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT, pinned_receiver);
        std::unique_ptr<SSL, decltype(&SSL_free)> server(SSL_new(context.get()), &SSL_free);
        require(server != nullptr);
        BIO* input = BIO_new(BIO_s_mem());
        BIO* output = BIO_new(BIO_s_mem());
        require(input != nullptr && output != nullptr);
        SSL_set_bio(server.get(), input, output);
        SSL_set_accept_state(server.get());
        // When: exchange genuine TLS handshake records, without sockets or mocks.
        for (int iteration = 0; iteration < 64; ++iteration) {
            const bool client_complete = client.doHandshake();
            transfer_to_server(client, input);
            const int result = SSL_do_handshake(server.get());
            const int error = SSL_get_error(server.get(), result);
            require(error == SSL_ERROR_NONE || error == SSL_ERROR_WANT_READ || error == SSL_ERROR_WANT_WRITE);
            transfer_to_client(output, client);
            if (client_complete && SSL_is_init_finished(server.get())) { break; }
        }
        // Then: both TLS endpoints finish and the server receives the receiver credential.
        require(client.isActive() && SSL_is_init_finished(server.get()));
        aa::tls::Certificate presented(SSL_get1_peer_certificate(server.get()), &X509_free);
        require(presented && X509_cmp(presented.get(), credentials.certificate.get()) == 0);
        if (test == "revoked") { approved = false; }
        if (test == "deinit") { client.deinit(); }
        const std::array<unsigned char, 4> payload{'t', 'e', 's', 't'};
        aasdk::common::Data encrypted;
        client.encrypt(encrypted, aasdk::common::DataConstBuffer(payload.data(), payload.size()));
        require(!encrypted.empty());
        require(BIO_write(input, encrypted.data(), static_cast<int>(encrypted.size())) == static_cast<int>(encrypted.size()));
        std::array<unsigned char, 16> plaintext{};
        require(SSL_read(server.get(), plaintext.data(), plaintext.size()) == static_cast<int>(payload.size()));
        require(std::equal(payload.begin(), payload.end(), plaintext.begin()));
        std::cout << "memory-bio-handshake=success receiver-credential-presented=yes encrypted-payload=yes tls-mode="
                  << policy.label() << '\n';
        return 0;
    } catch (const aa::tls::Error& error) {
        std::cerr << error.what() << '\n';
        return 1;
    } catch (const std::exception&) {
        std::cerr << "tls-test-operation-failed\n";
        return 2;
    }
}
