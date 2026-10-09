// SPDX-License-Identifier: GPL-3.0-or-later
#include <aa/diagnostics/Diagnostics.hpp>

#include "Policy.hpp"

#include <charconv>
#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>

namespace aa::diagnostics {
namespace {

// RFC 8259 string escaping: quote, backslash and control characters are
// escaped; UTF-8 bytes >= 0x20 pass through verbatim (valid in JSON strings).
std::string json_escape(std::string_view text) {
    std::string out;
    out.reserve(text.size());
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
    return out;
}

// Locale-independent JSON number; non-finite doubles are not valid JSON and
// degrade to null rather than emitting "inf"/"nan".
std::string format_double(double value) {
    if (!std::isfinite(value)) {
        return "null";
    }
    char buffer[32];
    const auto [ptr, ec] =
        std::to_chars(buffer, buffer + sizeof(buffer), value, std::chars_format::general);
    if (ec != std::errc{}) {
        return "null";
    }
    return std::string(buffer, ptr);
}

void write_quoted(std::string& out, std::string_view text) {
    out += "\"";
    out += json_escape(scrub_text(text));
    out += "\"";
}

// Textual form of any typed value: the input to the identifier pseudonymizer,
// so numeric and boolean variants of a typed identifier are pseudonymized
// exactly like strings instead of leaking their raw form.
struct ValueText {
    std::string operator()(const std::string& raw) const { return raw; }
    std::string operator()(std::int64_t value) const { return std::to_string(value); }
    std::string operator()(double value) const { return format_double(value); }
    std::string operator()(bool value) const { return value ? "true" : "false"; }
};

// Only schema-validated values reach this visitor.
struct ValueWriter {
    std::string& out;

    void operator()(const std::string& raw) const { write_quoted(out, raw); }
    void operator()(std::int64_t value) const { out += std::to_string(value); }
    void operator()(double value) const { out += format_double(value); }
    void operator()(bool value) const { out += value ? "true" : "false"; }
};

} // namespace

std::string_view to_string(EventKind kind) noexcept {
    switch (kind) {
    case EventKind::session_state_changed: return "session_state_changed";
    case EventKind::transport_stats: return "transport_stats";
    case EventKind::decode_stats: return "decode_stats";
    case EventKind::reconnect: return "reconnect";
    case EventKind::pairing_requested: return "pairing_requested";
    case EventKind::pairing_decided: return "pairing_decided";
    case EventKind::capability_negotiated: return "capability_negotiated";
    case EventKind::helper_call: return "helper_call";
    }
    return "unknown";
}

EventField EventField::text(std::string key, std::string value) {
    EventField field;
    field.key = std::move(key);
    field.sensitivity = Sensitivity::plain;
    field.value = std::move(value);
    return field;
}

EventField EventField::identifier(std::string key, std::string raw) {
    EventField field;
    field.key = std::move(key);
    field.sensitivity = Sensitivity::identifier;
    field.value = std::move(raw);
    return field;
}

EventField EventField::location(std::string key, std::string raw) {
    EventField field;
    field.key = std::move(key);
    field.sensitivity = Sensitivity::location;
    field.value = std::move(raw);
    return field;
}

EventField EventField::credential(std::string key, std::string raw) {
    EventField field;
    field.key = std::move(key);
    field.sensitivity = Sensitivity::credential;
    field.value = std::move(raw);
    return field;
}

EventField EventField::count(std::string key, std::int64_t value) {
    EventField field;
    field.key = std::move(key);
    field.sensitivity = Sensitivity::plain;
    field.value = value;
    return field;
}

EventField EventField::real(std::string key, double value) {
    EventField field;
    field.key = std::move(key);
    field.sensitivity = Sensitivity::plain;
    field.value = value;
    return field;
}

EventField EventField::flag(std::string key, bool value) {
    EventField field;
    field.key = std::move(key);
    field.sensitivity = Sensitivity::plain;
    field.value = value;
    return field;
}

std::string to_json(const Event& event) {
    std::string out = "{\"kind\":\"";
    out += to_string(event.kind);
    out += "\",\"session\":";
    out += std::to_string(event.session.value);
    out += ",\"mono_ns\":";
    out += std::to_string(event.monotonic_time.count);
    for (const EventField& field : event.fields) {
        const FieldPolicy policy = field_policy(field.key, field.sensitivity);
        if (policy == FieldPolicy::drop) {
            continue;
        }
        out += ",\"";
        out += json_escape(field.key);
        out += "\":";
        switch (policy) {
        case FieldPolicy::drop:
            break;
        case FieldPolicy::redact:
            out += "\"[redacted]\"";
            break;
        case FieldPolicy::pseudonym:
            write_quoted(out, redact_identifier(std::visit(ValueText{}, field.value)));
            break;
        case FieldPolicy::pass:
            if (is_safe_value(field.key, field.value)) {
                std::visit(ValueWriter{out}, field.value);
            } else {
                out += "\"[redacted]\"";
            }
            break;
        }
    }
    out += "}";
    return out;
}

} // namespace aa::diagnostics
