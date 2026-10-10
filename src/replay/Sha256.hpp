// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Project-owned SHA-256 (FIPS 180-4) for fixture content integrity. Deterministic
// over the fixed canonical bytes; never salted. Not an authenticity signature.

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace aa::replay::detail {

class Sha256 final {
public:
    Sha256() = default;
    void update(std::span<const std::byte> bytes) noexcept;
    [[nodiscard]] std::array<std::uint8_t, 32> finish() noexcept;

private:
    void compress(const std::uint8_t block[64]) noexcept;
    std::uint64_t length_bytes_{0};
    std::array<std::uint8_t, 64> buffer_{};
    std::size_t buffered_{0};
    std::array<std::uint32_t, 8> state_{
        0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
        0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U};
};

// Lowercase hex of the digest of one byte string.
[[nodiscard]] std::string sha256_hex(std::span<const std::byte> bytes);

} // namespace aa::replay::detail
