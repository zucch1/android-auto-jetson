// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Minimal, strict D-Bus wire codec (task 21) for the peer-credential lookup
// adapter: exactly the little-endian message shapes the lookup needs (method
// calls with s/u bodies and their s/u replies). Private to src/ipc/dbus_adapter;
// no external library types, only bounded project-owned buffers. Untrusted bus
// bytes are parsed once here ("parse, don't validate") into ParsedMessage and
// every read is bounds-checked against fixed ceilings; malformed or oversize
// traffic is rejected with a typed transport error, never trusted.

#include <aa/core/Result.hpp>

#include <cstddef>
#include <cstdint>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace aa::ipc::dbus_adapter {

inline constexpr std::size_t kMaxMessageBytes = 64UL * 1024;
inline constexpr std::size_t kMaxHeaderFields = 16;

enum class MessageType : std::uint8_t {
    method_call = 1,
    method_return = 2,
    error_reply = 3,
    signal = 4,
};

enum class HeaderField : std::uint8_t {
    path = 1,
    interface_name = 2,
    member = 3,
    error_name = 4,
    reply_serial = 5,
    destination = 6,
    sender = 7,
    signature = 8,
    unix_fds = 9,
};

// Assembles one little-endian D-Bus message: fixed header, header-field array
// (yv structs) and a typed body. Alignment follows the wire spec relative to
// the message origin (both field and body buffers start 8-aligned).
class MessageWriter final {
public:
    MessageWriter& path(std::string_view value);
    MessageWriter& interface_name(std::string_view value);
    MessageWriter& member(std::string_view value);
    MessageWriter& destination(std::string_view value);
    MessageWriter& body_signature(std::string_view value);
    MessageWriter& reply_serial(std::uint32_t value);
    MessageWriter& error_name(std::string_view value);

    MessageWriter& put_u32(std::uint32_t value);
    MessageWriter& put_u64(std::uint64_t value);
    MessageWriter& put_string(std::string_view value);
    MessageWriter& put_string_map(const std::map<std::string, std::string>& entries);
    // Marshals `value` as a D-Bus variant holding a string: contained signature
    // "s" as a signature value, then the 4-aligned string ("v" on the wire).
    MessageWriter& put_string_variant(std::string_view value);
    // Marshals `entries` as a D-Bus a{sv}: 4-aligned array length EXCLUDING the
    // gap before the first entry, 8-aligned dict entries of string key and
    // string-variant value.
    MessageWriter& put_property_dict(const std::map<std::string, std::string>& entries);

    [[nodiscard]] std::vector<std::uint8_t> build(MessageType type, std::uint32_t serial) const;

private:
    void field_string(HeaderField field, std::string_view value);

    std::vector<std::uint8_t> fields_{};
    std::vector<std::uint8_t> body_{};
};

// One fully parsed message. Header-field strings are owned and bounded.
struct ParsedMessage final {
    MessageType type{MessageType::method_return};
    std::uint32_t serial{0};
    std::uint32_t reply_serial{0};
    std::string path{};
    std::string interface_name{};
    std::string member{};
    std::string destination{};
    std::string sender{};
    std::string error_name{};
    std::string signature{};
    std::vector<std::uint8_t> body{};
};

// Strict byte cursor with wire alignment and typed reads.
class Cursor final {
public:
    explicit Cursor(std::span<const std::uint8_t> data) : data_(data) {}

    [[nodiscard]] core::Result<void> align(std::size_t boundary);
    [[nodiscard]] core::Result<std::uint8_t> read_u8();
    [[nodiscard]] core::Result<std::uint32_t> read_u32();
    [[nodiscard]] core::Result<std::uint64_t> read_u64();
    [[nodiscard]] core::Result<std::string> read_string();
    [[nodiscard]] core::Result<std::string> read_signature();
    // Skips one complete value of the given complete type (bounded depth).
    [[nodiscard]] core::Result<void> skip_value(std::string_view type);

    [[nodiscard]] std::size_t position() const noexcept { return pos_; }
    [[nodiscard]] bool done() const noexcept { return pos_ == data_.size(); }

private:
    // Recursion entry for skip_value: the depth budget propagates through
    // variant values, array elements and struct members so runtime-nested
    // variants cannot bypass kMaxTypeDepth.
    [[nodiscard]] core::Result<void> skip_value_at(std::string_view type, int depth);

    std::span<const std::uint8_t> data_;
    std::size_t pos_{0};
};

// Parses one complete message from exact message bytes (the framing length is
// computed by message_size() before reading the rest off the socket).
[[nodiscard]] core::Result<ParsedMessage> parse_message(std::span<const std::uint8_t> bytes);

// Total message size from the first 16 bytes, or a typed error when the
// framing is malformed/oversize.
[[nodiscard]] core::Result<std::size_t> message_size(std::span<const std::uint8_t> head16);

// Strict typed body readers for method-return replies: the declared signature
// must match exactly and the body must be fully consumed.
[[nodiscard]] core::Result<std::uint32_t> reply_u32(const ParsedMessage& message);
[[nodiscard]] core::Result<std::string> reply_string(const ParsedMessage& message);

} // namespace aa::ipc::dbus_adapter
