// SPDX-License-Identifier: GPL-3.0-or-later
#include "Json.hpp"

namespace aa::replay::detail {

bool JsonValue::is_null() const noexcept { return std::holds_alternative<std::monostate>(storage_); }
bool JsonValue::is_bool() const noexcept { return std::holds_alternative<bool>(storage_); }
bool JsonValue::is_int() const noexcept { return std::holds_alternative<std::int64_t>(storage_); }
bool JsonValue::is_real() const noexcept { return std::holds_alternative<double>(storage_); }
bool JsonValue::is_string() const noexcept {
    return std::holds_alternative<std::string>(storage_);
}
bool JsonValue::is_array() const noexcept { return std::holds_alternative<Array>(storage_); }
bool JsonValue::is_object() const noexcept { return std::holds_alternative<Object>(storage_); }

bool JsonValue::as_bool() const { return std::get<bool>(storage_); }
std::int64_t JsonValue::as_int() const { return std::get<std::int64_t>(storage_); }
double JsonValue::as_real() const { return std::get<double>(storage_); }
const std::string& JsonValue::as_string() const { return std::get<std::string>(storage_); }
const JsonValue::Array& JsonValue::as_array() const { return std::get<Array>(storage_); }
const JsonValue::Object& JsonValue::as_object() const { return std::get<Object>(storage_); }

bool JsonValue::int_value(std::int64_t& out) const {
    if (is_int()) {
        out = as_int();
        return true;
    }
    return false;
}

void write_json_string(std::string& out, std::string_view text) {
    out += "\"";
    for (const char c : text) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (static_cast<unsigned char>(c) < 0x20U) {
                static constexpr char kHex[] = "0123456789abcdef";
                out += "\\u00";
                out += kHex[(static_cast<unsigned char>(c) >> 4) & 0x0FU];
                out += kHex[static_cast<unsigned char>(c) & 0x0FU];
            } else {
                out.push_back(c);
            }
            break;
        }
    }
    out += "\"";
}

} // namespace aa::replay::detail
