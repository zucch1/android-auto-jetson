// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <memory>
#include <exception>
#include <openssl/ssl.h>

namespace aa::tls {
enum class Failure { unset, unreadable, insecure, malformed, mismatch, unknown_phone,
                     peer_rejected, inactive, configuration };

class Error final : public std::exception {
public:
    explicit Error(Failure failure) noexcept : failure_(failure) {}
    [[nodiscard]] Failure failure() const noexcept { return failure_; }
    [[nodiscard]] const char* what() const noexcept override;
private:
    Failure failure_;
};

using Certificate = std::unique_ptr<X509, decltype(&X509_free)>;
using PrivateKey = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>;

struct Credentials {
    Certificate certificate;
    PrivateKey private_key;
};

[[nodiscard]] Credentials load_credentials();
}
