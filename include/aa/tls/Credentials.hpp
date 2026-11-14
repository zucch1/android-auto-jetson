// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <memory>
#include <exception>
#include <string>
#include <utility>
#include <vector>
#include <openssl/ssl.h>

namespace aa::tls {
enum class Failure { unset, unreadable, insecure, malformed, mismatch, unknown_phone,
                     peer_rejected, inactive, configuration };

class Error final : public std::exception {
public:
    explicit Error(Failure failure) noexcept : failure_(failure) {}
    Error(Failure failure, std::string detail)
        : failure_(failure), detail_(std::move(detail)) {}
    [[nodiscard]] Failure failure() const noexcept { return failure_; }
    [[nodiscard]] const char* what() const noexcept override;
private:
    Failure failure_;
    std::string detail_;
};

using Certificate = std::unique_ptr<X509, decltype(&X509_free)>;
using PrivateKey = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>;

struct Credentials {
    Certificate certificate;
    PrivateKey private_key;
    std::vector<Certificate> chain;
};

[[nodiscard]] Credentials load_credentials();
}
