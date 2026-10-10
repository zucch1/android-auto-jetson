// SPDX-License-Identifier: GPL-3.0-or-later
#include <aa/tls/Policy.hpp>
#include <iostream>
#include <memory>
#include <string_view>

int main(int argc, char** argv) {
    if (argc != 2) { return 2; }
    const std::string_view test(argv[1]);
    // Given: real OpenSSL instances and explicit policy inputs.
    std::unique_ptr<SSL_CTX, decltype(&SSL_CTX_free)> context(SSL_CTX_new(TLS_client_method()), &SSL_CTX_free);
    std::unique_ptr<SSL, decltype(&SSL_free)> ssl(SSL_new(context.get()), &SSL_free);
    aa::tls::Policy policy;
    if (test == "approved") { policy = aa::tls::Policy([] { return true; }); }
    if (test == "unknown") { policy = aa::tls::Policy([] { return false; }); }
    if (test == "throwing") { policy = aa::tls::Policy([]() -> bool { throw 42; }); }
    if (test == "verified") {
        policy = aa::tls::Policy([] { return true; }, aa::tls::Mode::verified_peer_test_only);
    }
    // When: configure TLS and evaluate the independent approved-phone seam.
    try {
        policy.configure(ssl.get());
        policy.require_approved();
    } catch (const aa::tls::Error& error) {
        // Then: absent, unknown and failing predicates all deny with one class.
        if ((test == "absent" || test == "unknown" || test == "throwing") &&
            error.failure() == aa::tls::Failure::unknown_phone) {
            std::cout << error.what() << '\n';
            return 0;
        }
        return 1;
    }
    if (test != "approved" && test != "verified") { return 1; }
    const int expected = test == "verified" ? SSL_VERIFY_PEER : SSL_VERIFY_NONE;
    if (SSL_get_verify_mode(ssl.get()) != expected || SSL_get_min_proto_version(ssl.get()) != TLS1_2_VERSION) {
        return 1;
    }
    std::cout << "tls-mode=" << policy.label() << " phone-approved=yes\n";
    return 0;
}
