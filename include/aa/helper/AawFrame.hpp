// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <variant>
#include <vector>

namespace aa::helper {
enum class AawError { oversize, unknown_id, truncated };
struct AawRecord {
    std::uint16_t id;
    std::vector<std::uint8_t> payload;
};
using AawDecodeResult = std::variant<std::monostate, AawRecord, AawError>;

std::optional<std::vector<std::uint8_t>> encode_aaw(
    std::uint16_t id, std::span<const std::uint8_t> payload);

class AawDecoder {
public:
    void feed(std::span<const std::uint8_t> bytes);
    // monostate means no complete record is available (including drained EOF).
    AawDecodeResult next();
    // Ends input and validates all queued records without consuming them.
    // Any failure is sticky; feed() after finish() is ignored.
    std::optional<AawError> finish();

private:
    std::vector<std::uint8_t> buffer_;
    std::optional<AawError> error_;
    bool finished_{};
};
}
