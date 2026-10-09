// SPDX-License-Identifier: GPL-3.0-or-later
#include <aa/diagnostics/Diagnostics.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <random>

namespace aa::diagnostics {
namespace {

// Process-lifetime seed for in-run correlation only. This noncryptographic
// construction does not guarantee resistance to offline guessing or cross-run
// linkage, including when random_device supplies weak entropy.
std::uint64_t salt() {
    static const std::uint64_t value = [] {
        std::random_device source;
        std::uint64_t s = (static_cast<std::uint64_t>(source()) << 32) ^
                          static_cast<std::uint64_t>(source());
        const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
        s ^= static_cast<std::uint64_t>(now);
        // ASLR-scrambled stack address: a per-process value random_device may
        // not provide on its own. The address is never dereferenced.
        const auto addr = reinterpret_cast<std::uintptr_t>(&s);
        s ^= (addr * 0x9E3779B97F4A7C15ULL);
        // Mix seed bits; this does not add entropy.
        s ^= s >> 33;
        s *= 0xFF51AFD7ED558CCDULL;
        s ^= s >> 33;
        return s;
    }();
    return value;
}

// Salt-seeded FNV-1a: stable within a process, not a cryptographic MAC.
std::uint64_t fnv1a(std::string_view text, std::uint64_t seed) {
    std::uint64_t hash = seed;
    for (const char c : text) {
        hash ^= static_cast<std::uint64_t>(static_cast<unsigned char>(c));
        hash *= 0x00000100000001B3ULL;
    }
    return hash;
}

std::string to_hex(std::uint64_t value) {
    static constexpr std::array kDigits{'0', '1', '2', '3', '4', '5', '6', '7',
                                        '8', '9', 'a', 'b', 'c', 'd', 'e', 'f'};
    std::string out(16, '0');
    for (std::size_t i = 0; i < 16; ++i) {
        out[15 - i] = kDigits[value & 0xFU];
        value >>= 4;
    }
    return out;
}

} // namespace

std::string redact_identifier(std::string_view raw) {
    if (raw.empty()) {
        return {};
    }
    return "id-" + to_hex(fnv1a(raw, salt()));
}

} // namespace aa::diagnostics
