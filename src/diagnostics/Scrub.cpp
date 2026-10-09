// SPDX-License-Identifier: GPL-3.0-or-later
#include <aa/diagnostics/Diagnostics.hpp>

#include "Policy.hpp"

#include <cstddef>
#include <string>
#include <string_view>

namespace aa::diagnostics {
namespace {

bool is_hex(char c) noexcept {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

bool is_key_char(char c) noexcept {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           c == '_' || c == '-' || c == '.';
}

bool is_value_stop(char c) noexcept {
    return c == ';' || c == '}' || c == '\n' || c == '"' || c == '\0';
}

bool is_mac_sep(char c) noexcept { return c == ':' || c == '-'; }

// 6 groups of 2 hex digits separated by ':' or '-' (Bluetooth MAC shape),
// 17 characters total. Separators may be mixed; every group must be 2 hex.
std::size_t mac_match_len(std::string_view t, std::size_t i) noexcept {
    if (i + 17 > t.size()) {
        return 0;
    }
    for (std::size_t g = 0; g < 6; ++g) {
        const std::size_t p = i + 3 * g;
        if (!is_hex(t[p]) || !is_hex(t[p + 1])) {
            return 0;
        }
        if (g < 5 && !is_mac_sep(t[p + 2])) {
            return 0;
        }
    }
    return 17;
}

// [-+]?DDD.DDD , [-+]?DDD.DDD  (a lat,lon coordinate pair).
std::size_t coord_match_len(std::string_view t, std::size_t i) noexcept {
    auto decimal = [&](std::size_t p) -> std::size_t {
        if (p < t.size() && (t[p] == '+' || t[p] == '-')) {
            ++p;
        }
        std::size_t int_digits = 0;
        while (p < t.size() && t[p] >= '0' && t[p] <= '9' && int_digits < 4) {
            ++p;
            ++int_digits;
        }
        if (int_digits == 0 || int_digits > 3) {
            return 0;
        }
        if (p >= t.size() || t[p] != '.') {
            return 0;
        }
        ++p;
        std::size_t frac_digits = 0;
        while (p < t.size() && t[p] >= '0' && t[p] <= '9') {
            ++p;
            ++frac_digits;
        }
        return frac_digits == 0 ? 0 : p;
    };
    const std::size_t after_first = decimal(i);
    if (after_first == 0) {
        return 0;
    }
    std::size_t p = after_first;
    while (p < t.size() && (t[p] == ' ' || t[p] == '\t')) {
        ++p;
    }
    if (p >= t.size() || t[p] != ',') {
        return 0;
    }
    ++p;
    while (p < t.size() && (t[p] == ' ' || t[p] == '\t')) {
        ++p;
    }
    const std::size_t after_second = decimal(p);
    return after_second == 0 ? 0 : after_second - i;
}

// A sensitive key=value or key:value match. prefix_len is the preserved part
// (key + spaces + separator); len spans through the end of the value run.
struct KvMatch {
    std::size_t len{0};
    std::size_t prefix_len{0};
};

KvMatch sensitive_kv_match(std::string_view t, std::size_t i) {
    std::size_t k = i;
    while (k < t.size() && is_key_char(t[k])) {
        ++k;
    }
    const std::size_t key_len = k - i;
    if (key_len == 0) {
        return {};
    }
    std::size_t s = k;
    while (s < t.size() && t[s] == ' ') {
        ++s;
    }
    if (s >= t.size() || (t[s] != '=' && t[s] != ':')) {
        return {};
    }
    if (!is_sensitive_key(normalize_key(t.substr(i, key_len)))) {
        return {};
    }
    const std::size_t sep = s;
    std::size_t v = sep + 1;
    while (v < t.size() && !is_value_stop(t[v])) {
        ++v;
    }
    return {v - i, sep + 1 - i};
}

// One well-formed UTF-8 sequence at i (RFC 3629: no overlong forms, no UTF-16
// surrogates, nothing above U+10FFFF). len is its byte length on success.
bool utf8_sequence_ok(std::string_view t, std::size_t i, std::size_t& len) noexcept {
    const auto byte = [&](std::size_t p) { return static_cast<unsigned char>(t[p]); };
    const unsigned char lead = byte(i);
    if (lead < 0x80U) {
        len = 1;
        return true;
    }
    std::size_t need = 0;
    unsigned char second_lo = 0x80U;
    unsigned char second_hi = 0xBFU;
    if (lead >= 0xC2U && lead <= 0xDFU) {
        need = 1;
    } else if (lead == 0xE0U) {
        need = 2;
        second_lo = 0xA0U;
    } else if (lead == 0xEDU) {
        need = 2;
        second_hi = 0x9FU;
    } else if ((lead >= 0xE1U && lead <= 0xECU) || (lead >= 0xEEU && lead <= 0xEFU)) {
        need = 2;
    } else if (lead == 0xF0U) {
        need = 3;
        second_lo = 0x90U;
    } else if (lead == 0xF4U) {
        need = 3;
        second_hi = 0x8FU;
    } else if (lead >= 0xF1U && lead <= 0xF3U) {
        need = 3;
    } else {
        return false;
    }
    if (i + need >= t.size()) {
        return false;
    }
    const unsigned char second = byte(i + 1);
    if (second < second_lo || second > second_hi) {
        return false;
    }
    for (std::size_t k = 2; k <= need; ++k) {
        const unsigned char cont = byte(i + k);
        if (cont < 0x80U || cont > 0xBFU) {
            return false;
        }
    }
    len = need + 1;
    return true;
}

// UTF-8 repair: every invalid byte becomes U+FFFD (one replacement per bad
// byte; no partial sequence is ever copied through), so text crossing this
// boundary is always valid UTF-8 and the serialized output is valid JSON.
std::string sanitize_utf8(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    std::size_t i = 0;
    while (i < text.size()) {
        std::size_t len = 0;
        if (utf8_sequence_ok(text, i, len)) {
            out.append(text.substr(i, len));
            i += len;
        } else {
            out += "\xEF\xBF\xBD";
            ++i;
        }
    }
    return out;
}

} // namespace

std::string scrub_text(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    std::size_t i = 0;
    while (i < text.size()) {
        if (const std::size_t len = mac_match_len(text, i); len != 0) {
            out += redact_identifier(text.substr(i, len));
            i += len;
            continue;
        }
        if (const KvMatch kv = sensitive_kv_match(text, i); kv.len != 0) {
            out.append(text.substr(i, kv.prefix_len));
            out += "[redacted]";
            i += kv.len;
            continue;
        }
        if (const std::size_t len = coord_match_len(text, i); len != 0) {
            out += "[redacted]";
            i += len;
            continue;
        }
        out.push_back(text[i]);
        ++i;
    }
    return sanitize_utf8(out);
}

} // namespace aa::diagnostics
