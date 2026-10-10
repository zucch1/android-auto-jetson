// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Test-only fixture file surgery for gate-proofing. `tamper` mutates the
// canonical body text; `reseal` recomputes the envelope SHA-256 over the
// mutated body so a rejection can only come from content validation, never
// from a stale checksum. The reseal hash engine is the project SHA-256; its
// correctness is proven independently by the Python hashlib cross-check in
// tests/replay/test_fixture_qa.py. flip_hash_char keeps the stale-hash case.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "../../src/replay/Sha256.hpp"

namespace replaytest {

inline std::string to_text(std::span<const std::byte> bytes) {
    return std::string{reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

inline std::vector<std::byte> to_bytes(std::string_view text) {
    return std::vector<std::byte>{reinterpret_cast<const std::byte*>(text.data()),
                                  reinterpret_cast<const std::byte*>(text.data()) + text.size()};
}

inline std::string hex_of(const std::vector<std::byte>& bytes) {
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string out(bytes.size() * 2, '0');
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        const auto value = static_cast<std::uint8_t>(bytes[i]);
        out[2 * i] = kDigits[value >> 4U];
        out[2 * i + 1] = kDigits[value & 0x0FU];
    }
    return out;
}

inline std::optional<std::vector<std::byte>> tamper(std::span<const std::byte> file,
                                                   std::string_view from,
                                                   std::string_view to) {
    std::string text = to_text(file);
    const auto at = text.find(from);
    if (at == std::string::npos) {
        return std::nullopt;
    }
    text.replace(at, from.size(), to);
    return to_bytes(text);
}

// Recomputes the envelope hash over the file's body while preserving every
// other byte - including a deliberately wrong envelope - so a later rejection
// can only come from the content gates, never from a stale checksum.
inline std::optional<std::vector<std::byte>> reseal(std::span<const std::byte> file) {
    constexpr std::string_view kSeparator = "\",\"body\":";
    const std::string text = to_text(file);
    const std::size_t separator_at = text.rfind(kSeparator);
    if (separator_at == std::string::npos || separator_at < 64 || text.size() < separator_at + 2
        || text.back() != '}') {
        return std::nullopt;
    }
    const std::string body = text.substr(separator_at + kSeparator.size(),
                                         text.size() - separator_at - kSeparator.size() - 1);
    std::string hash_input = "aa-replay-fixture-v1\n";
    hash_input += body;
    const auto span = std::span<const std::byte>{
        reinterpret_cast<const std::byte*>(hash_input.data()), hash_input.size()};
    std::string out = text.substr(0, separator_at - 64);
    out += aa::replay::detail::sha256_hex(span);
    out += text.substr(separator_at);
    return to_bytes(out);
}

inline std::optional<std::vector<std::byte>> tamper_resealed(std::span<const std::byte> file,
                                                            std::string_view from,
                                                            std::string_view to) {
    const auto mutated = tamper(file, from, to);
    if (!mutated) {
        return std::nullopt;
    }
    return reseal(*mutated);
}

inline std::optional<std::vector<std::byte>> flip_hash_char(std::span<const std::byte> file) {
    constexpr std::string_view kPrefix = "{\"schema\":\"aa-replay-fixture-v1\",\"sha256\":\"";
    if (file.size() <= kPrefix.size()) {
        return std::nullopt;
    }
    std::vector<std::byte> out(file.begin(), file.end());
    const auto at = kPrefix.size();
    const char c = static_cast<char>(out[at]);
    out[at] = static_cast<std::byte>(c == '0' ? '1' : '0');
    return out;
}

} // namespace replaytest
