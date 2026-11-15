// SPDX-License-Identifier: GPL-3.0-or-later
#include "Crypto.hpp"
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/params.h>
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
core::Result<std::string> random_ap_secret() {
    std::array<unsigned char, 40> random{};
    if (RAND_bytes(random.data(), static_cast<int>(random.size())) != 1)
        return Error{ErrorCode::helper_unavailable};
    constexpr char digits[] = "0123456789abcdef";
    std::string result;
    for (const auto byte : random) { result += digits[byte >> 4]; result += digits[byte & 15]; }
    return result;
}
bool random_bytes(unsigned char* out, std::size_t length) {
    return out && length > 0 && length <= 4096 && RAND_bytes(out, static_cast<int>(length)) == 1;
}
core::Result<std::string> hmac_sha256(const unsigned char* key, std::size_t key_length, std::string_view message) {
    if (!key || key_length == 0) return Error{ErrorCode::helper_unavailable};
    std::unique_ptr<EVP_MAC, decltype(&EVP_MAC_free)> mac{EVP_MAC_fetch(nullptr, "HMAC", nullptr), EVP_MAC_free};
    if (!mac) return Error{ErrorCode::helper_unavailable};
    std::unique_ptr<EVP_MAC_CTX, decltype(&EVP_MAC_CTX_free)> ctx{EVP_MAC_CTX_new(mac.get()), EVP_MAC_CTX_free};
    if (!ctx) return Error{ErrorCode::helper_unavailable};
    char digest_name[] = "SHA256";
    OSSL_PARAM params[]{
        OSSL_PARAM_construct_utf8_string("digest", digest_name, 0),
        OSSL_PARAM_construct_end(),
    };
    if (EVP_MAC_init(ctx.get(), key, key_length, params) != 1 ||
        (!message.empty() &&
         EVP_MAC_update(ctx.get(), reinterpret_cast<const unsigned char*>(message.data()), message.size()) != 1))
        return Error{ErrorCode::helper_unavailable};
    std::array<unsigned char, 32> digest{};
    std::size_t out_length{};
    if (EVP_MAC_final(ctx.get(), digest.data(), &out_length, digest.size()) != 1 || out_length != 32) {
        OPENSSL_cleanse(digest.data(), digest.size());
        return Error{ErrorCode::helper_unavailable};
    }
    constexpr char hex[] = "0123456789abcdef";
    std::string result;
    result.reserve(64);
    for (const auto byte : digest) {
        result.push_back(hex[byte >> 4]);
        result.push_back(hex[byte & 15]);
    }
    OPENSSL_cleanse(digest.data(), digest.size());
    return result;
}
void cleanse(void* data, std::size_t length) noexcept {
    if (data && length) OPENSSL_cleanse(data, length);
}
}
