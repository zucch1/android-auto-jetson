// SPDX-License-Identifier: GPL-3.0-or-later
#include "Json.hpp"

#include <algorithm>

namespace aa::trust::detail {

namespace {

bool is_ws(char c) noexcept {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

class Parser final {
public:
    Parser(std::string_view text, const JsonLimits& limits) : text_(text), limits_(limits) {}

    core::Result<JsonValue> run() {
        if (text_.size() > limits_.max_bytes) {
            return Error{ErrorCode::invalid_argument};
        }
        skip_ws();
        auto value = parse_value(0);
        if (!value.has_value()) {
            return value.error();
        }
        skip_ws();
        if (pos_ != text_.size()) {
            return Error{ErrorCode::invalid_argument};
        }
        return value;
    }

private:
    void skip_ws() {
        while (pos_ < text_.size() && is_ws(text_[pos_])) {
            ++pos_;
        }
    }

    [[nodiscard]] bool take(char expected) {
        if (pos_ < text_.size() && text_[pos_] == expected) {
            ++pos_;
            return true;
        }
        return false;
    }

    core::Result<JsonValue> parse_value(int depth) {
        if (depth > static_cast<int>(limits_.max_depth)) {
            return Error{ErrorCode::invalid_argument};
        }
        if (pos_ >= text_.size()) {
            return Error{ErrorCode::invalid_argument};
        }
        switch (text_[pos_]) {
        case '{': return parse_object(depth);
        case '[': return parse_array(depth);
        case '"': {
            auto string = parse_string();
            if (!string.has_value()) {
                return string.error();
            }
            return JsonValue{JsonValue::Storage{string.value()}};
        }
        default: return parse_uint();
        }
    }

    core::Result<JsonValue> parse_object(int depth) {
        ++pos_;  // '{'
        JsonValue::Object members;
        skip_ws();
        if (take('}')) {
            return JsonValue{JsonValue::Storage{std::move(members)}};
        }
        while (true) {
            skip_ws();
            auto key = parse_string();
            if (!key.has_value()) {
                return key.error();
            }
            const auto duplicate = std::find_if(
                members.begin(), members.end(),
                [&key](const JsonValue::Member& member) { return member.first == key.value(); });
            if (duplicate != members.end()) {
                return Error{ErrorCode::invalid_argument};
            }
            skip_ws();
            if (!take(':')) {
                return Error{ErrorCode::invalid_argument};
            }
            skip_ws();
            auto value = parse_value(depth + 1);
            if (!value.has_value()) {
                return value.error();
            }
            members.emplace_back(key.value(), std::move(value.value()));
            if (members.size() > limits_.max_members) {
                return Error{ErrorCode::invalid_argument};
            }
            skip_ws();
            if (take('}')) {
                return JsonValue{JsonValue::Storage{std::move(members)}};
            }
            if (!take(',')) {
                return Error{ErrorCode::invalid_argument};
            }
        }
    }

    core::Result<JsonValue> parse_array(int depth) {
        ++pos_;  // '['
        JsonValue::Array items;
        skip_ws();
        if (take(']')) {
            return JsonValue{JsonValue::Storage{std::move(items)}};
        }
        while (true) {
            skip_ws();
            auto value = parse_value(depth + 1);
            if (!value.has_value()) {
                return value.error();
            }
            items.push_back(std::move(value.value()));
            if (items.size() > limits_.max_array) {
                return Error{ErrorCode::invalid_argument};
            }
            skip_ws();
            if (take(']')) {
                return JsonValue{JsonValue::Storage{std::move(items)}};
            }
            if (!take(',')) {
                return Error{ErrorCode::invalid_argument};
            }
        }
    }

    core::Result<std::string> parse_string() {
        if (!take('"')) {
            return Error{ErrorCode::invalid_argument};
        }
        std::string out;
        while (pos_ < text_.size()) {
            const char c = text_[pos_++];
            if (c == '"') {
                if (out.size() > limits_.max_string) {
                    return Error{ErrorCode::invalid_argument};
                }
                return out;
            }
            if (static_cast<unsigned char>(c) < 0x20U) {
                return Error{ErrorCode::invalid_argument};
            }
            if (c != '\\') {
                out.push_back(c);
            } else {
                if (pos_ >= text_.size()) {
                    return Error{ErrorCode::invalid_argument};
                }
                const char escaped = text_[pos_++];
                switch (escaped) {
                case '"': out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case '/': out.push_back('/'); break;
                case 'n': out.push_back('\n'); break;
                case 't': out.push_back('\t'); break;
                case 'r': out.push_back('\r'); break;
                default: return Error{ErrorCode::invalid_argument};  // \u and friends rejected
                }
            }
            if (out.size() > limits_.max_string) {
                return Error{ErrorCode::invalid_argument};
            }
        }
        return Error{ErrorCode::invalid_argument};
    }

    core::Result<JsonValue> parse_uint() {
        if (pos_ >= text_.size() || text_[pos_] < '0' || text_[pos_] > '9') {
            return Error{ErrorCode::invalid_argument};  // no sign, floats, bools or null
        }
        // RFC 8259 numbers carry no leading zeros: "02" is as forbidden as a
        // float, so a tampered document can never alias one integer spelling
        // onto another.
        if (text_[pos_] == '0' && pos_ + 1 < text_.size() && text_[pos_ + 1] >= '0' &&
            text_[pos_ + 1] <= '9') {
            return Error{ErrorCode::invalid_argument};
        }
        std::uint64_t value = 0;
        while (pos_ < text_.size() && text_[pos_] >= '0' && text_[pos_] <= '9') {
            const auto digit = static_cast<std::uint64_t>(text_[pos_] - '0');
            if (value > (UINT64_MAX - digit) / 10U) {
                return Error{ErrorCode::invalid_argument};
            }
            value = value * 10U + digit;
            ++pos_;
        }
        if (pos_ < text_.size() && (text_[pos_] == '.' || text_[pos_] == 'e' || text_[pos_] == 'E')) {
            return Error{ErrorCode::invalid_argument};
        }
        return JsonValue{JsonValue::Storage{value}};
    }

    std::string_view text_;
    JsonLimits limits_;
    std::size_t pos_{0};
};

} // namespace

const std::string& JsonValue::as_string() const { return std::get<0>(storage_); }
std::uint64_t JsonValue::as_uint() const { return std::get<1>(storage_); }
const JsonValue::Array& JsonValue::as_array() const { return std::get<2>(storage_); }
const JsonValue::Object& JsonValue::as_object() const { return std::get<3>(storage_); }

core::Result<JsonValue> parse_json(std::string_view text, const JsonLimits& limits) {
    Parser parser(text, limits);
    return parser.run();
}

void append_json_string(std::string& out, std::string_view text) {
    static constexpr char kHex[] = "0123456789abcdef";
    out.push_back('"');
    for (const char c : text) {
        const auto byte = static_cast<unsigned char>(c);
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (byte < 0x20U) {
                out += "\\u00";
                out.push_back(kHex[(byte >> 4U) & 0xFU]);
                out.push_back(kHex[byte & 0xFU]);
            } else {
                out.push_back(c);
            }
        }
    }
    out.push_back('"');
}

} // namespace aa::trust::detail
