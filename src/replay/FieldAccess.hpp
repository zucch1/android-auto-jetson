// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Shared strict field accessors for the fixture schema parsers. Every value
// crossing the loader boundary is checked into a typed value here (parse,
// don't validate downstream); missing, duplicate or mistyped members are
// typed invalid_argument rejections and no accessor allocates beyond the
// already-bounded JSON tree.

#include <aa/core/Result.hpp>

#include "FixtureValidate.hpp"
#include "Json.hpp"

#include <cstdint>
#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

namespace aa::replay::detail {

inline Error malformed_field() { return Error{ErrorCode::invalid_argument}; }

inline core::Result<void> expect_keys(const JsonValue& value,
                                      std::initializer_list<std::string_view> keys) {
    if (!value.is_object()) {
        return malformed_field();
    }
    const auto& members = value.as_object();
    if (members.size() != keys.size()) {
        return malformed_field();
    }
    for (const std::string_view key : keys) {
        std::size_t seen = 0;
        for (const auto& member : members) {
            if (member.first == key) {
                ++seen;
            }
        }
        if (seen != 1) {
            return malformed_field();
        }
    }
    return {};
}

inline const JsonValue& field(const JsonValue& object, std::string_view key) {
    static const JsonValue null{};
    for (const auto& member : object.as_object()) {
        if (member.first == key) {
            return member.second;
        }
    }
    return null;
}

inline const JsonValue::Array* array_of(const JsonValue& value) {
    return value.is_array() ? &value.as_array() : nullptr;
}

inline core::Result<std::int64_t> int_of(const JsonValue& value) {
    std::int64_t out = 0;
    if (!value.int_value(out)) {
        return malformed_field();
    }
    return out;
}

inline core::Result<std::string> string_of(const JsonValue& value) {
    if (!value.is_string()) {
        return malformed_field();
    }
    return value.as_string();
}

inline core::Result<std::uint64_t> u64_of(const JsonValue& value) {
    const auto number = int_of(value);
    if (!number || number.value() < 0) {
        return malformed_field();
    }
    return static_cast<std::uint64_t>(number.value());
}

inline core::Result<std::uint32_t> u32_of(const JsonValue& value) {
    const auto number = int_of(value);
    if (!number || number.value() < 0 || number.value() > 4'294'967'295LL) {
        return malformed_field();
    }
    return static_cast<std::uint32_t>(number.value());
}

inline core::Result<std::uint16_t> u16_of(const JsonValue& value) {
    const auto number = int_of(value);
    if (!number || number.value() < 0 || number.value() > 65'535) {
        return malformed_field();
    }
    return static_cast<std::uint16_t>(number.value());
}

inline core::Result<std::uint8_t> u8_of(const JsonValue& value) {
    const auto number = int_of(value);
    if (!number || number.value() < 0 || number.value() > 255) {
        return malformed_field();
    }
    return static_cast<std::uint8_t>(number.value());
}

inline core::Result<void> blob_of(const JsonValue& value, std::vector<std::byte>& storage) {
    const auto text = string_of(value);
    if (!text) {
        return text.error();
    }
    auto bytes = from_hex(text.value());
    if (!bytes) {
        return bytes.error();
    }
    storage = std::move(bytes).value();
    return {};
}

inline core::Result<void> policy_ok(const JsonValue& value) {
    const auto text = string_of(value);
    if (!text || text.value() != kOpaquePolicy) {
        return malformed_field();
    }
    return {};
}

inline core::Result<protocol::ChannelRole> role_of(const JsonValue& value) {
    const auto text = string_of(value);
    if (!text) {
        return text.error();
    }
    return parse_role(text.value());
}

inline core::Result<audio::Role> audio_role_of(const JsonValue& value) {
    const auto text = string_of(value);
    if (!text) {
        return text.error();
    }
    return parse_audio_role(text.value());
}

} // namespace aa::replay::detail
