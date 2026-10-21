// SPDX-License-Identifier: GPL-3.0-or-later
#include "Crypto.hpp"
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <array>
#include <memory>
#include <unistd.h>

namespace aa::helper::crypto_adapter {
core::Result<std::string> hash_executable(int fd, std::chrono::steady_clock::time_point deadline) {
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> ctx{EVP_MD_CTX_new(), EVP_MD_CTX_free};
    if (!ctx || EVP_DigestInit_ex(ctx.get(), EVP_sha256(), nullptr) != 1)
        return Error{ErrorCode::helper_unavailable};
    std::array<unsigned char, 16384> bytes{};
    std::size_t total{};
    for (;;) {
        if (std::chrono::steady_clock::now() >= deadline) return Error{ErrorCode::helper_rejected};
        const auto n = ::read(fd, bytes.data(), bytes.size());
        if (n < 0) return Error{ErrorCode::helper_rejected};
        if (n == 0) break;
        total += static_cast<std::size_t>(n);
        if (total > 128UL * 1024 * 1024 ||
            EVP_DigestUpdate(ctx.get(), bytes.data(), static_cast<std::size_t>(n)) != 1)
            return Error{ErrorCode::helper_rejected};
    }
    std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
    unsigned int length{};
    if (EVP_DigestFinal_ex(ctx.get(), digest.data(), &length) != 1 || length != 32)
        return Error{ErrorCode::helper_rejected};
    constexpr char hex[] = "0123456789abcdef";
    std::string result;
    for (unsigned int i = 0; i < length; ++i) {
        result.push_back(hex[digest[i] >> 4]);
        result.push_back(hex[digest[i] & 15]);
    }
    return result;
}
core::Result<std::uint64_t> random_id() {
    std::uint64_t id{};
    if (RAND_bytes(reinterpret_cast<unsigned char*>(&id), sizeof(id)) != 1 || !id)
        return Error{ErrorCode::helper_unavailable};
    return id;
}
}
