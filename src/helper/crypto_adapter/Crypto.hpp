// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <aa/core/Result.hpp>
#include <chrono>
#include <string>

namespace aa::helper::crypto_adapter {
core::Result<std::string> hash_executable(int fd, std::chrono::steady_clock::time_point deadline);
core::Result<std::uint64_t> random_id();
core::Result<std::string> random_ap_secret();
}
