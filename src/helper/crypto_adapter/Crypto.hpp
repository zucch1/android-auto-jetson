// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <aa/core/Result.hpp>
#include <chrono>
#include <cstddef>
#include <string>
#include <string_view>

namespace aa::helper::crypto_adapter {
core::Result<std::string> hash_executable(int fd, std::chrono::steady_clock::time_point deadline);
core::Result<std::uint64_t> random_id();
core::Result<std::string> random_ap_secret();
bool random_bytes(unsigned char* out, std::size_t length);
core::Result<std::string> hmac_sha256(const unsigned char* key, std::size_t key_length, std::string_view message);
void cleanse(void* data, std::size_t length) noexcept;
}
