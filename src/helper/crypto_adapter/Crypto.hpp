// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <aa/core/Result.hpp>
#include <chrono>
#include <span>
#include <string>

namespace aa::helper::crypto_adapter {
core::Result<std::string> hash_executable(int fd, std::chrono::steady_clock::time_point deadline);
core::Result<std::uint64_t> random_id();
// Fills `out` in place with random lowercase hex digits (at most 64): no
// intermediate std::string, and the raw random bytes are cleansed (#89 L9).
core::Result<void> random_hex(std::span<char> out);
}
