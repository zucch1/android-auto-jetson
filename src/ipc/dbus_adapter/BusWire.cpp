// SPDX-License-Identifier: GPL-3.0-or-later
#include "BusWire.hpp"

namespace aa::ipc::dbus_adapter {

namespace {

constexpr std::size_t kFixedHeaderBytes = 16;
constexpr std::uint32_t kProtocolVersion = 1;

void pad_to(std::vector<std::uint8_t>& buffer, std::size_t boundary) {
    while (buffer.size() % boundary != 0) {
        buffer.push_back(0);
    }
}

void push_u8(std::vector<std::uint8_t>& buffer, std::uint8_t value) {
    buffer.push_back(value);
}

void push_u32(std::vector<std::uint8_t>& buffer, std::uint32_t value) {
    for (int shift = 0; shift < 32; shift += 8) {
        buffer.push_back(static_cast<std::uint8_t>((value >> shift) & 0xFFU));
    }
}

void push_u64(std::vector<std::uint8_t>& buffer, std::uint64_t value) {
    for (int shift = 0; shift < 64; shift += 8) {
        buffer.push_back(static_cast<std::uint8_t>((value >> shift) & 0xFFU));
    }
}

void push_string(std::vector<std::uint8_t>& buffer, std::string_view value) {
    pad_to(buffer, 4);
    push_u32(buffer, static_cast<std::uint32_t>(value.size()));
    buffer.insert(buffer.end(), value.begin(), value.end());
    buffer.push_back(0);
}

void push_signature(std::vector<std::uint8_t>& buffer, std::string_view value) {
    push_u8(buffer, static_cast<std::uint8_t>(value.size()));
    buffer.insert(buffer.end(), value.begin(), value.end());
    buffer.push_back(0);
}

} // namespace

MessageWriter& MessageWriter::path(std::string_view value) {
    field_string(HeaderField::path, value);
    return *this;
}

MessageWriter& MessageWriter::interface_name(std::string_view value) {
    field_string(HeaderField::interface_name, value);
    return *this;
}

MessageWriter& MessageWriter::member(std::string_view value) {
    field_string(HeaderField::member, value);
    return *this;
}

MessageWriter& MessageWriter::destination(std::string_view value) {
    field_string(HeaderField::destination, value);
    return *this;
}

MessageWriter& MessageWriter::body_signature(std::string_view value) {
    if (value.empty()) {
        return *this;
    }
    pad_to(fields_, 8);
    push_u8(fields_, static_cast<std::uint8_t>(HeaderField::signature));
    push_signature(fields_, "g");
    push_signature(fields_, value);
    return *this;
}

MessageWriter& MessageWriter::reply_serial(std::uint32_t value) {
    pad_to(fields_, 8);
    push_u8(fields_, static_cast<std::uint8_t>(HeaderField::reply_serial));
    push_signature(fields_, "u");
    pad_to(fields_, 4);
    push_u32(fields_, value);
    return *this;
}

MessageWriter& MessageWriter::error_name(std::string_view value) {
    field_string(HeaderField::error_name, value);
    return *this;
}

void MessageWriter::field_string(HeaderField field, std::string_view value) {
    const std::string_view type = field == HeaderField::path ? "o" : "s";
    pad_to(fields_, 8);
    push_u8(fields_, static_cast<std::uint8_t>(field));
    push_signature(fields_, type);
    push_string(fields_, value);
}

MessageWriter& MessageWriter::put_u32(std::uint32_t value) {
    pad_to(body_, 4);
    push_u32(body_, value);
    return *this;
}

MessageWriter& MessageWriter::put_u64(std::uint64_t value) {
    pad_to(body_, 8);
    push_u64(body_, value);
    return *this;
}

MessageWriter& MessageWriter::put_string(std::string_view value) {
    push_string(body_, value);
    return *this;
}

MessageWriter& MessageWriter::put_string_map(const std::map<std::string, std::string>& entries) {
    pad_to(body_, 4);
    const std::size_t length_at = body_.size();
    push_u32(body_, 0);
    pad_to(body_, 8);
    const std::size_t elements_at = body_.size();
    for (const auto& entry : entries) {
        pad_to(body_, 8);
        push_string(body_, entry.first);
        push_string(body_, entry.second);
    }
    const auto length = static_cast<std::uint32_t>(body_.size() - elements_at);
    for (int shift = 0; shift < 32; shift += 8) {
        body_[length_at + static_cast<std::size_t>(shift / 8)] =
            static_cast<std::uint8_t>((length >> shift) & 0xFFU);
    }
    return *this;
}

std::vector<std::uint8_t> MessageWriter::build(MessageType type, std::uint32_t serial) const {
    std::vector<std::uint8_t> out;
    out.reserve(kFixedHeaderBytes + fields_.size() + 8 + body_.size());
    push_u8(out, 'l');
    push_u8(out, static_cast<std::uint8_t>(type));
    push_u8(out, 0);
    push_u8(out, static_cast<std::uint8_t>(kProtocolVersion));
    push_u32(out, static_cast<std::uint32_t>(body_.size()));
    push_u32(out, serial);
    push_u32(out, static_cast<std::uint32_t>(fields_.size()));
    out.insert(out.end(), fields_.begin(), fields_.end());
    pad_to(out, 8);
    out.insert(out.end(), body_.begin(), body_.end());
    return out;
}

} // namespace aa::ipc::dbus_adapter
