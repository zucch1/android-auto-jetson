// SPDX-License-Identifier: GPL-3.0-or-later
#include "JsonLex.hpp"

#include <charconv>
#include <cstdint>
#include <system_error>

namespace aa::replay::detail {
namespace {

constexpr Error bad_token() { return Error{ErrorCode::invalid_argument}; }

void append_utf8(std::string& out, std::uint32_t unit) {
    if (unit < 0x80U) {
        out.push_back(static_cast<char>(unit));
    } else if (unit < 0x800U) {
        out.push_back(static_cast<char>(0xC0U | (unit >> 6U)));
        out.push_back(static_cast<char>(0x80U | (unit & 0x3FU)));
    } else {
        out.push_back(static_cast<char>(0xE0U | (unit >> 12U)));
        out.push_back(static_cast<char>(0x80U | ((unit >> 6U) & 0x3FU)));
        out.push_back(static_cast<char>(0x80U | (unit & 0x3FU)));
    }
}

core::Result<std::uint32_t> hex4(std::string_view text, std::size_t& pos) {
    if (pos + 4 > text.size()) {
        return bad_token();
    }
    std::uint32_t value = 0;
    for (std::size_t i = 0; i < 4; ++i) {
        const char c = text[pos++];
        value <<= 4U;
        if (c >= '0' && c <= '9') {
            value |= static_cast<std::uint32_t>(c - '0');
        } else if (c >= 'a' && c <= 'f') {
            value |= static_cast<std::uint32_t>(c - 'a' + 10);
        } else if (c >= 'A' && c <= 'F') {
            value |= static_cast<std::uint32_t>(c - 'A' + 10);
        } else {
            return bad_token();
        }
    }
    return value;
}

bool continuation(unsigned char c) noexcept { return (c & 0xC0U) == 0x80U; }

// Width of one well-formed UTF-8 sequence at `at` (RFC 3629: no overlong
// forms, no surrogates, nothing above U+10FFFF), or 0 when invalid.
std::size_t utf8_width(std::string_view text, std::size_t at) noexcept {
    const auto lead = static_cast<unsigned char>(text[at]);
    const auto byte = [&](std::size_t offset) {
        return static_cast<unsigned char>(text[at + offset]);
    };
    if (lead < 0x80U) {
        return 1;
    }
    if (lead >= 0xC2U && lead <= 0xDFU) {
        return (at + 1 < text.size() && continuation(byte(1))) ? 2 : 0;
    }
    if (lead == 0xE0U) {
        return (at + 2 < text.size() && byte(1) >= 0xA0U && byte(1) <= 0xBFU
                && continuation(byte(2)))
                   ? 3
                   : 0;
    }
    if (lead >= 0xE1U && lead <= 0xECU) {
        return (at + 2 < text.size() && continuation(byte(1)) && continuation(byte(2))) ? 3 : 0;
    }
    if (lead == 0xEDU) {
        return (at + 2 < text.size() && byte(1) >= 0x80U && byte(1) <= 0x9FU
                && continuation(byte(2)))
                   ? 3
                   : 0;
    }
    if (lead >= 0xEEU && lead <= 0xEFU) {
        return (at + 2 < text.size() && continuation(byte(1)) && continuation(byte(2))) ? 3 : 0;
    }
    if (lead == 0xF0U) {
        return (at + 3 < text.size() && byte(1) >= 0x90U && byte(1) <= 0xBFU
                && continuation(byte(2)) && continuation(byte(3)))
                   ? 4
                   : 0;
    }
    if (lead >= 0xF1U && lead <= 0xF3U) {
        return (at + 3 < text.size() && continuation(byte(1)) && continuation(byte(2))
                && continuation(byte(3)))
                   ? 4
                   : 0;
    }
    if (lead == 0xF4U) {
        return (at + 3 < text.size() && byte(1) >= 0x80U && byte(1) <= 0x8FU
                && continuation(byte(2)) && continuation(byte(3)))
                   ? 4
                   : 0;
    }
    return 0;
}

} // namespace

core::Result<std::string> lex_string(std::string_view text, std::size_t& pos,
                                     const JsonLimits& limits) {
    ++pos; // opening quote
    std::string out;
    while (pos < text.size()) {
        const char c = text[pos];
        if (c == '"') {
            ++pos;
            if (out.size() > limits.max_string) {
                return bad_token();
            }
            return out;
        }
        if (static_cast<unsigned char>(c) < 0x20U) {
            return bad_token();
        }
        if (c != '\\') {
            const std::size_t width = utf8_width(text, pos);
            if (width == 0) {
                return bad_token();
            }
            out.append(text.substr(pos, width));
            pos += width;
            if (out.size() > limits.max_string) {
                return bad_token();
            }
            continue;
        }
        ++pos;
        if (pos >= text.size()) {
            return bad_token();
        }
        const char esc = text[pos++];
        switch (esc) {
        case '"': out.push_back('"'); break;
        case '\\': out.push_back('\\'); break;
        case '/': out.push_back('/'); break;
        case 'b': out.push_back('\b'); break;
        case 'f': out.push_back('\f'); break;
        case 'n': out.push_back('\n'); break;
        case 'r': out.push_back('\r'); break;
        case 't': out.push_back('\t'); break;
        case 'u': {
            auto code = hex4(text, pos);
            if (!code) {
                return code.error();
            }
            const std::uint32_t unit = code.value();
            if (unit >= 0xD800U && unit <= 0xDFFFU) {
                return bad_token();
            }
            append_utf8(out, unit);
            break;
        }
        default: return bad_token();
        }
        if (out.size() > limits.max_string) {
            return bad_token();
        }
    }
    return bad_token();
}

bool is_valid_utf8(std::string_view text) noexcept {
    std::size_t at = 0;
    while (at < text.size()) {
        const std::size_t width = utf8_width(text, at);
        if (width == 0) {
            return false;
        }
        at += width;
    }
    return true;
}

core::Result<JsonValue> lex_number(std::string_view text, std::size_t& pos) {
    const std::size_t start = pos;
    if (pos < text.size() && text[pos] == '-') {
        ++pos;
    }
    std::size_t digits = 0;
    while (pos < text.size() && text[pos] >= '0' && text[pos] <= '9') {
        ++pos;
        ++digits;
    }
    if (digits == 0) {
        return bad_token();
    }
    bool integral = true;
    if (pos < text.size() && text[pos] == '.') {
        integral = false;
        ++pos;
        std::size_t frac = 0;
        while (pos < text.size() && text[pos] >= '0' && text[pos] <= '9') {
            ++pos;
            ++frac;
        }
        if (frac == 0) {
            return bad_token();
        }
    }
    if (pos < text.size() && (text[pos] == 'e' || text[pos] == 'E')) {
        integral = false;
        ++pos;
        if (pos < text.size() && (text[pos] == '+' || text[pos] == '-')) {
            ++pos;
        }
        std::size_t exp = 0;
        while (pos < text.size() && text[pos] >= '0' && text[pos] <= '9') {
            ++pos;
            ++exp;
        }
        if (exp == 0) {
            return bad_token();
        }
    }
    const std::string_view token = text.substr(start, pos - start);
    if (integral) {
        std::int64_t value{};
        const auto [ptr, ec] = std::from_chars(token.data(), token.data() + token.size(), value);
        if (ec != std::errc{} || ptr != token.data() + token.size()) {
            return bad_token();
        }
        return JsonValue{JsonValue::Storage{value}};
    }
    double value{};
    const auto [ptr, ec] = std::from_chars(token.data(), token.data() + token.size(), value);
    if (ec != std::errc{} || ptr != token.data() + token.size()) {
        return bad_token();
    }
    return JsonValue{JsonValue::Storage{value}};
}

} // namespace aa::replay::detail
