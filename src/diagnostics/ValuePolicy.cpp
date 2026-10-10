// SPDX-License-Identifier: GPL-3.0-or-later
#include "Policy.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <string_view>
#include <system_error>
#include <variant>

namespace aa::diagnostics {
namespace {

bool one_of(std::string_view value, std::initializer_list<std::string_view> choices) {
    return std::find(choices.begin(), choices.end(), value) != choices.end();
}

// Versions contain only bounded decimal components, with these two fixed
// product prefixes. No arbitrary vendor/build suffix or freeform text passes.
bool version_ok(std::string_view value) {
    if (value.starts_with("jetson-r")) {
        value.remove_prefix(8);
    } else if (value.starts_with("Android ")) {
        value.remove_prefix(8);
        const auto split = value.find(" AA ");
        if (split == std::string_view::npos || split == 0 || split > 2) return false;
        for (const char c : value.substr(0, split)) {
            if (c < '0' || c > '9') return false;
        }
        value.remove_prefix(split + 4);
    }
    std::size_t parts = 0;
    while (!value.empty()) {
        const auto dot = value.find('.');
        const auto part = value.substr(0, dot);
        if (part.empty() || part.size() > 6) return false;
        for (const char c : part) {
            if (c < '0' || c > '9') return false;
        }
        if (++parts > 4) return false;
        if (dot == std::string_view::npos) return parts >= 2;
        value.remove_prefix(dot + 1);
    }
    return false;
}

struct ValueValidator {
    TelemetryType type;

    bool operator()(const std::string& value) const {
        switch (type) {
        case TelemetryType::state:
            return one_of(value, {"disconnected", "discovering", "pairing", "connecting",
                                 "negotiating", "active", "degraded", "reconnecting",
                                 "stopping", "failed"});
        case TelemetryType::version: return version_ok(value);
        case TelemetryType::reason:
            return one_of(value, {"link lost", "timeout", "requested", "peer disconnected",
                                 "transport error", "protocol error", "shutdown"});
        case TelemetryType::status:
            return one_of(value, {"ok", "success", "failed", "pending", "accepted", "rejected"});
        case TelemetryType::decision:
            return one_of(value, {"allow", "deny", "accept", "reject", "retry", "stop"});
        case TelemetryType::transport: return one_of(value, {"usb", "tcp", "wired", "wireless"});
        case TelemetryType::codec: return one_of(value, {"h264", "pcm", "aac"});
        case TelemetryType::unknown:
        case TelemetryType::count:
        case TelemetryType::number:
        case TelemetryType::flag:
        case TelemetryType::count_or_flag: return false;
        }
        return false;
    }

    bool operator()(std::int64_t value) const {
        switch (type) {
        case TelemetryType::count:
        case TelemetryType::number:
        case TelemetryType::count_or_flag: return value >= 0;
        case TelemetryType::unknown:
        case TelemetryType::state:
        case TelemetryType::version:
        case TelemetryType::reason:
        case TelemetryType::status:
        case TelemetryType::decision:
        case TelemetryType::transport:
        case TelemetryType::codec:
        case TelemetryType::flag: return false;
        }
        return false;
    }
    bool operator()(double value) const {
        switch (type) {
        case TelemetryType::number: return std::isfinite(value) && value >= 0;
        case TelemetryType::unknown:
        case TelemetryType::state:
        case TelemetryType::version:
        case TelemetryType::reason:
        case TelemetryType::status:
        case TelemetryType::decision:
        case TelemetryType::transport:
        case TelemetryType::codec:
        case TelemetryType::count:
        case TelemetryType::flag:
        case TelemetryType::count_or_flag: return false;
        }
        return false;
    }
    bool operator()(bool) const {
        switch (type) {
        case TelemetryType::flag:
        case TelemetryType::count_or_flag: return true;
        case TelemetryType::unknown:
        case TelemetryType::state:
        case TelemetryType::version:
        case TelemetryType::reason:
        case TelemetryType::status:
        case TelemetryType::decision:
        case TelemetryType::transport:
        case TelemetryType::codec:
        case TelemetryType::count:
        case TelemetryType::number: return false;
        }
        return false;
    }
};

template <typename T>
bool numeric_log_value(std::string_view value, TelemetryType type) {
    if (value.empty()) return false;
    T parsed{};
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), parsed);
    return error == std::errc{} && end == value.data() + value.size() &&
           ValueValidator{type}(parsed);
}

} // namespace

bool is_safe_value(std::string_view key, const FieldValue& value) {
    return std::visit(ValueValidator{telemetry_type(key)}, value);
}

bool is_safe_log_value(TelemetryType type, std::string_view value) {
    switch (type) {
    case TelemetryType::unknown: return false;
    case TelemetryType::count: return numeric_log_value<std::int64_t>(value, type);
    case TelemetryType::number: return numeric_log_value<double>(value, type);
    case TelemetryType::count_or_flag:
        return one_of(value, {"true", "false"}) || numeric_log_value<std::int64_t>(value, type);
    case TelemetryType::flag: return one_of(value, {"true", "false"});
    case TelemetryType::state:
    case TelemetryType::version:
    case TelemetryType::reason:
    case TelemetryType::status:
    case TelemetryType::decision:
    case TelemetryType::transport:
    case TelemetryType::codec: return ValueValidator{type}(std::string{value});
    }
    return false;
}

std::string_view safe_log_message(std::string_view message) noexcept {
    // Reuse the event vocabulary: new message names require an explicit schema change.
    for (const EventKind kind : {EventKind::session_state_changed, EventKind::transport_stats,
                                EventKind::decode_stats, EventKind::reconnect,
                                EventKind::pairing_requested, EventKind::pairing_decided,
                                EventKind::capability_negotiated, EventKind::helper_call}) {
        if (message == to_string(kind)) return to_string(kind);
    }
    return "[redacted]";
}
} // namespace aa::diagnostics
