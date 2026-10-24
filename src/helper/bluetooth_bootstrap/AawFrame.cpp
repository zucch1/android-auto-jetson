// SPDX-License-Identifier: GPL-3.0-or-later
#include <aa/helper/AawFrame.hpp>

namespace aa::helper {
namespace {
constexpr std::size_t kHeaderSize = 4;
constexpr std::size_t kMaxPayload = 4096;

std::uint16_t read_u16(std::span<const std::uint8_t> bytes) {
    return static_cast<std::uint16_t>((static_cast<unsigned>(bytes[0]) << 8) | bytes[1]);
}

std::optional<AawError> check_header(std::span<const std::uint8_t> bytes) {
    if (read_u16(bytes) > kMaxPayload) return AawError::oversize;
    const auto id = read_u16(bytes.subspan(2));
    if (id < 1 || id > 7) return AawError::unknown_id;
    return std::nullopt;
}
}

std::optional<std::vector<std::uint8_t>> encode_aaw(
    std::uint16_t id, std::span<const std::uint8_t> payload) {
    if (payload.size() > kMaxPayload || id < 1 || id > 7) return std::nullopt;
    std::vector<std::uint8_t> wire;
    wire.reserve(kHeaderSize + payload.size());
    wire.push_back(static_cast<std::uint8_t>(payload.size() >> 8));
    wire.push_back(static_cast<std::uint8_t>(payload.size() & 0xff));
    wire.push_back(static_cast<std::uint8_t>(id >> 8));
    wire.push_back(static_cast<std::uint8_t>(id & 0xff));
    wire.insert(wire.end(), payload.begin(), payload.end());
    return wire;
}

void AawDecoder::feed(std::span<const std::uint8_t> bytes) {
    if (error_ || finished_) return;
    buffer_.insert(buffer_.end(), bytes.begin(), bytes.end());
}

AawDecodeResult AawDecoder::next() {
    if (error_) return *error_;
    if (buffer_.size() < kHeaderSize) return std::monostate{};
    const std::span<const std::uint8_t> bytes{buffer_};
    error_ = check_header(bytes);
    if (error_) return *error_;
    const auto size = kHeaderSize + read_u16(bytes);
    if (bytes.size() < size) return std::monostate{};
    const auto payload = bytes.subspan(kHeaderSize, size - kHeaderSize);
    AawRecord record{read_u16(bytes.subspan(2)), {payload.begin(), payload.end()}};
    buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(size));
    return record;
}

std::optional<AawError> AawDecoder::finish() {
    finished_ = true;
    if (error_) return error_;
    std::span<const std::uint8_t> remaining{buffer_};
    while (!remaining.empty()) {
        if (remaining.size() < kHeaderSize) {
            error_ = AawError::truncated;
            return error_;
        }
        error_ = check_header(remaining);
        if (error_) return error_;
        const auto size = kHeaderSize + read_u16(remaining);
        if (remaining.size() < size) {
            error_ = AawError::truncated;
            return error_;
        }
        remaining = remaining.subspan(size);
    }
    return std::nullopt;
}
}
