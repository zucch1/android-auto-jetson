// SPDX-License-Identifier: GPL-3.0-or-later
#include "MetadataGate.hpp"

#include <aa/diagnostics/Diagnostics.hpp>
#include <aa/replay/Schema.hpp>

#include "Json.hpp"

#include <cstdint>
#include <string>
#include <string_view>

namespace aa::replay {
namespace {

using diagnostics::Event;
using diagnostics::EventField;
using diagnostics::EventKind;

constexpr Error gate_reject() { return Error{ErrorCode::invalid_argument}; }

bool hex_digit(char c) noexcept {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

bool is_mac_shape(std::string_view text, std::size_t at) noexcept {
    if (at + 17 > text.size()) {
        return false;
    }
    for (std::size_t group = 0; group < 6; ++group) {
        const std::size_t p = at + 3 * group;
        if (!hex_digit(text[p]) || !hex_digit(text[p + 1])) {
            return false;
        }
        if (group < 5 && text[p + 2] != ':' && text[p + 2] != '-') {
            return false;
        }
    }
    return true;
}

std::size_t digit_run_at(std::string_view text, std::size_t at) noexcept {
    std::size_t run = 0;
    while (at + run < text.size() && text[at + run] >= '0' && text[at + run] <= '9') {
        ++run;
    }
    return run;
}

bool pseudonym_at(std::string_view text, std::size_t at) noexcept {
    constexpr std::string_view kPrefix = "id-";
    if (text.substr(at, kPrefix.size()) != kPrefix) {
        return false;
    }
    std::size_t hex = 0;
    while (at + kPrefix.size() + hex < text.size()
           && hex_digit(text[at + kPrefix.size() + hex])) {
        ++hex;
    }
    return hex == 16;
}

bool kind_name_valid(std::string_view name) noexcept {
    for (const EventKind kind : {EventKind::session_state_changed, EventKind::transport_stats,
                                 EventKind::decode_stats, EventKind::reconnect,
                                 EventKind::pairing_requested, EventKind::pairing_decided,
                                 EventKind::capability_negotiated, EventKind::helper_call}) {
        if (diagnostics::to_string(kind) == name) {
            return true;
        }
    }
    return false;
}

EventKind parse_kind(std::string_view name) noexcept {
    for (const EventKind kind : {EventKind::session_state_changed, EventKind::transport_stats,
                                 EventKind::decode_stats, EventKind::reconnect,
                                 EventKind::pairing_requested, EventKind::pairing_decided,
                                 EventKind::capability_negotiated, EventKind::helper_call}) {
        if (diagnostics::to_string(kind) == name) {
            return kind;
        }
    }
    return EventKind::session_state_changed;
}

core::Result<Event> rebuild(const detail::JsonValue& document) {
    if (!document.is_object()) {
        return gate_reject();
    }
    const auto& members = document.as_object();
    if (members.size() < 3) {
        return gate_reject();
    }
    if (members[0].first != "kind" || !members[0].second.is_string()
        || !kind_name_valid(members[0].second.as_string())) {
        return gate_reject();
    }
    std::int64_t session = 0;
    std::int64_t mono = 0;
    if (members[1].first != "session" || !members[1].second.int_value(session) || session < 0) {
        return gate_reject();
    }
    if (members[2].first != "mono_ns" || !members[2].second.int_value(mono)) {
        return gate_reject();
    }
    Event event;
    event.kind = parse_kind(members[0].second.as_string());
    event.session = core::SessionId{static_cast<std::uint64_t>(session)};
    event.monotonic_time = core::Nanoseconds{mono};
    for (std::size_t i = 3; i < members.size(); ++i) {
        const auto& [key, value] = members[i];
        if (key == "kind" || key == "session" || key == "mono_ns") {
            return gate_reject();
        }
        if (value.is_string()) {
            event.fields.push_back(EventField::text(key, value.as_string()));
        } else if (value.is_int()) {
            event.fields.push_back(EventField::count(key, value.as_int()));
        } else if (value.is_real()) {
            event.fields.push_back(EventField::real(key, value.as_real()));
        } else if (value.is_bool()) {
            event.fields.push_back(EventField::flag(key, value.as_bool()));
        } else {
            return gate_reject();
        }
    }
    return event;
}

} // namespace

bool has_identifier_shape(std::string_view text) noexcept {
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (is_mac_shape(text, i)) {
            return true;
        }
        const std::size_t run = digit_run_at(text, i);
        if (run >= 15) {
            return true;
        }
        if (run > 0) {
            i += run - 1;
        }
    }
    return false;
}

std::string fixedredact_metadata(std::string_view to_json_line) {
    std::string out;
    out.reserve(to_json_line.size());
    for (std::size_t i = 0; i < to_json_line.size();) {
        if (pseudonym_at(to_json_line, i)) {
            out += "[redacted]";
            i += 19;
        } else {
            out.push_back(to_json_line[i]);
            ++i;
        }
    }
    return out;
}

core::Result<void> reject_raw_identifiers(std::string_view to_json_line) {
    const auto parsed = detail::parse_json(to_json_line);
    if (!parsed || !parsed.value().is_object()) {
        return gate_reject();
    }
    const auto& members = parsed.value().as_object();
    for (std::size_t i = 3; i < members.size(); ++i) {
        const auto& [key, value] = members[i];
        if (key == "session" || key == "mono_ns") {
            continue;
        }
        if (value.is_string() && has_identifier_shape(value.as_string())) {
            return gate_reject();
        }
        if (value.is_int() && has_identifier_shape(std::to_string(value.as_int()))) {
            return gate_reject();
        }
    }
    return {};
}

namespace detail {

core::Result<void> metadata_gate(std::string_view to_json_line) {
    const auto parsed = parse_json(to_json_line);
    if (!parsed) {
        return parsed.error();
    }
    const auto event = rebuild(parsed.value());
    if (!event) {
        return event.error();
    }
    if (diagnostics::to_json(event.value()) != to_json_line) {
        return gate_reject();
    }
    return reject_raw_identifiers(to_json_line);
}

} // namespace detail
} // namespace aa::replay
