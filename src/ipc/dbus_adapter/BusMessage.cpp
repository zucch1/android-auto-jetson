// SPDX-License-Identifier: GPL-3.0-or-later
#include "BusWire.hpp"

#include <algorithm>

namespace aa::ipc::dbus_adapter {

namespace {

constexpr std::size_t kFixedHeaderBytes = 16;
constexpr std::uint32_t kProtocolVersion = 1;

} // namespace

core::Result<std::size_t> message_size(std::span<const std::uint8_t> head16) {
    if (head16.size() < kFixedHeaderBytes || head16[0] != 'l' ||
        head16[3] != kProtocolVersion) {
        return Error{ErrorCode::transport_malformed_frame};
    }
    if (head16[1] < 1 || head16[1] > 4) {
        return Error{ErrorCode::transport_malformed_frame};
    }
    const auto le32 = [&head16](std::size_t offset) -> std::uint32_t {
        return static_cast<std::uint32_t>(head16[offset]) |
               (static_cast<std::uint32_t>(head16[offset + 1]) << 8U) |
               (static_cast<std::uint32_t>(head16[offset + 2]) << 16U) |
               (static_cast<std::uint32_t>(head16[offset + 3]) << 24U);
    };
    const std::uint32_t body_len = le32(4);
    const std::uint32_t fields_len = le32(12);
    std::size_t total = kFixedHeaderBytes + fields_len;
    total += (8 - (total % 8)) % 8;
    total += body_len;
    if (total > kMaxMessageBytes || total < kFixedHeaderBytes) {
        return Error{ErrorCode::transport_oversize_frame};
    }
    return total;
}

core::Result<ParsedMessage> parse_message(std::span<const std::uint8_t> bytes) {
    const auto expected = message_size(bytes.first(std::min(bytes.size(), kFixedHeaderBytes)));
    if (!expected.has_value()) {
        return expected.error();
    }
    if (expected.value() != bytes.size()) {
        return Error{ErrorCode::transport_malformed_frame};
    }
    Cursor cursor(bytes);
    ParsedMessage message;
    const auto endian = cursor.read_u8();
    const auto type = cursor.read_u8();
    const auto flags = cursor.read_u8();
    const auto version = cursor.read_u8();
    const auto body_len = cursor.read_u32();
    const auto serial = cursor.read_u32();
    const auto fields_len = cursor.read_u32();
    if (!endian.has_value() || endian.value() != 'l' || !type.has_value() || type.value() < 1 ||
        type.value() > 4 || !flags.has_value() || !version.has_value() ||
        version.value() != kProtocolVersion || !body_len.has_value() || !serial.has_value() ||
        serial.value() == 0 || !fields_len.has_value()) {
        return Error{ErrorCode::transport_malformed_frame};
    }
    message.type = static_cast<MessageType>(type.value());
    message.serial = serial.value();

    const std::size_t fields_end = kFixedHeaderBytes + fields_len.value();
    if (fields_end > bytes.size()) {
        return Error{ErrorCode::transport_malformed_frame};
    }
    std::size_t field_count = 0;
    while (cursor.position() < fields_end) {
        if (++field_count > kMaxHeaderFields) {
            return Error{ErrorCode::transport_malformed_frame};
        }
        const auto padded = cursor.align(8);
        const auto code = cursor.read_u8();
        const auto sig = cursor.read_signature();
        if (!padded.has_value() || !code.has_value() || !sig.has_value()) {
            return Error{ErrorCode::transport_malformed_frame};
        }
        switch (code.value()) {
        case 1:
        case 2:
        case 3:
        case 4:
        case 6:
        case 7: {
            if (sig.value() != "s" && sig.value() != "o") {
                return Error{ErrorCode::transport_malformed_frame};
            }
            const auto value = cursor.read_string();
            if (!value.has_value()) {
                return value.error();
            }
            switch (code.value()) {
            case 1: message.path = value.value(); break;
            case 2: message.interface_name = value.value(); break;
            case 3: message.member = value.value(); break;
            case 4: message.error_name = value.value(); break;
            case 6: message.destination = value.value(); break;
            default: message.sender = value.value(); break;
            }
            break;
        }
        case 5: {
            if (sig.value() != "u") {
                return Error{ErrorCode::transport_malformed_frame};
            }
            const auto value = cursor.read_u32();
            if (!value.has_value()) {
                return value.error();
            }
            message.reply_serial = value.value();
            break;
        }
        case 8: {
            if (sig.value() != "g") {
                return Error{ErrorCode::transport_malformed_frame};
            }
            const auto value = cursor.read_signature();
            if (!value.has_value()) {
                return value.error();
            }
            message.signature = value.value();
            break;
        }
        default: {
            const auto skipped = cursor.skip_value(sig.value());
            if (!skipped.has_value()) {
                return skipped.error();
            }
            break;
        }
        }
    }
    if (cursor.position() != fields_end) {
        return Error{ErrorCode::transport_malformed_frame};
    }
    const auto body_aligned = cursor.align(8);
    if (!body_aligned.has_value() || cursor.position() + body_len.value() != bytes.size()) {
        return Error{ErrorCode::transport_malformed_frame};
    }
    message.body.assign(bytes.begin() + static_cast<std::ptrdiff_t>(cursor.position()),
                        bytes.end());
    return message;
}

core::Result<std::uint32_t> reply_u32(const ParsedMessage& message) {
    if (message.type == MessageType::error_reply) {
        return Error{ErrorCode::transport_io};
    }
    if (message.signature != "u") {
        return Error{ErrorCode::transport_malformed_frame};
    }
    Cursor cursor(message.body);
    auto value = cursor.read_u32();
    if (!value.has_value() || !cursor.done()) {
        return Error{ErrorCode::transport_malformed_frame};
    }
    return value;
}

core::Result<std::string> reply_string(const ParsedMessage& message) {
    if (message.type == MessageType::error_reply) {
        return Error{ErrorCode::transport_io};
    }
    if (message.signature != "s") {
        return Error{ErrorCode::transport_malformed_frame};
    }
    Cursor cursor(message.body);
    auto value = cursor.read_string();
    if (!value.has_value() || !cursor.done()) {
        return Error{ErrorCode::transport_malformed_frame};
    }
    return value;
}

} // namespace aa::ipc::dbus_adapter
