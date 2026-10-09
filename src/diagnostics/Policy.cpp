// SPDX-License-Identifier: GPL-3.0-or-later
#include "Policy.hpp"

#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>

namespace aa::diagnostics {
namespace {

// Sensitive key shapes: credentials, phone identifiers and location. Matched by
// substring on the normalized key so "api_key", "auth-token", "serial_number"
// and "bluetooth_mac" are all caught.
constexpr std::string_view kSensitiveContains[] = {
    "password", "passwd", "pwd", "passphrase", "psk", "secret", "token",
    "credential", "bearer", "authorization", "cookie", "apikey", "privatekey",
    "accesskey", "secretkey", "sessionkey", "encryptionkey", "nonce", "salt",
    "signature", "hmac",
    "imei", "imsi", "meid", "serial", "deviceid", "androidid", "phoneid",
    "subscriberid", "btaddr", "bluetooth", "udid", "macaddr", "macaddress",
    "latitude", "longitude", "location", "gps", "coord", "geolocation", "geoloc",
};
constexpr std::string_view kSensitiveEnds[] = {"mac", "uuid", "guid"};

// Exact emission names, not substring/normalized matches. In particular a
// sensitive-looking unknown key is still unknown and must not be copied out.
constexpr std::string_view kSensitiveNames[] = {
    "password", "passwd", "pwd", "passphrase", "psk", "secret", "token",
    "credential", "authorization", "cookie", "api_key", "private_key",
    "imei", "imsi", "meid", "serial", "serial_number", "device_id",
    "android_id", "phone_id", "phone_mac", "bluetooth_mac", "mac", "uuid",
    "gps", "latitude", "longitude", "location", "coordinates", "secret_flag",
};

template <std::size_t N>
bool table_contains(const std::string_view (&table)[N], std::string_view key) noexcept {
    return std::find(std::begin(table), std::end(table), key) !=
           std::end(table);
}

} // namespace

std::string normalize_key(std::string_view key) {
    std::string out;
    out.reserve(key.size());
    for (const char c : key) {
        if (c == '_' || c == '-') {
            continue;
        }
        out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    return out;
}

bool is_sensitive_key(const std::string& normalized_key) noexcept {
    for (const std::string_view needle : kSensitiveContains) {
        if (normalized_key.find(needle) != std::string::npos) {
            return true;
        }
    }
    for (const std::string_view needle : kSensitiveEnds) {
        if (normalized_key.size() >= needle.size() &&
            normalized_key.compare(normalized_key.size() - needle.size(), needle.size(),
                                   needle) == 0) {
            return true;
        }
    }
    return false;
}

TelemetryType telemetry_type(std::string_view key) noexcept {
    static constexpr std::string_view states[] = {
        "state", "from_state", "to_state", "prev_state", "next_state", "old_state", "new_state"};
    static constexpr std::string_view versions[] = {
        "version", "target_version", "phone_version", "aa_version", "proto_version",
        "sdk_version", "os_version", "sw_version", "hw_version"};
    static constexpr std::string_view reasons[] = {"reason", "reconnect_reason"};
    static constexpr std::string_view statuses[] = {"status", "result", "outcome"};
    static constexpr std::string_view decisions[] = {"decision", "action"};
    static constexpr std::string_view counts[] = {
        "frames", "packets", "bytes", "samples", "count", "total", "retries", "drops",
        "errors", "warnings", "events", "width", "height", "size", "capacity", "depth",
        "queue", "inflight", "offset", "index", "seq", "sequence", "code", "error_code"};
    static constexpr std::string_view numbers[] = {
        "fps", "bitrate", "latency_ms", "p50_latency_ms", "p95_latency_ms", "p99_latency_ms",
        "jitter_ms", "rtt_ms", "elapsed_ms", "duration_ms", "update_ms", "interval_ms",
        "window_ms", "age_ms", "timeout_ms", "gap_ms"};
    static constexpr std::string_view flags[] = {
        "ok", "success", "connected", "active", "enabled", "ready", "valid", "complete"};
    if (table_contains(states, key)) return TelemetryType::state;
    if (table_contains(versions, key)) return TelemetryType::version;
    if (table_contains(reasons, key)) return TelemetryType::reason;
    if (table_contains(statuses, key)) return TelemetryType::status;
    if (table_contains(decisions, key)) return TelemetryType::decision;
    if (key == "transport") return TelemetryType::transport;
    if (key == "codec") return TelemetryType::codec;
    if (table_contains(counts, key)) return TelemetryType::count;
    if (table_contains(numbers, key)) return TelemetryType::number;
    if (table_contains(flags, key)) return TelemetryType::flag;
    if (key == "dropped") return TelemetryType::count_or_flag;
    return TelemetryType::unknown;
}

FieldPolicy field_policy(std::string_view key, Sensitivity declared) {
    const bool sensitive = table_contains(kSensitiveNames, key);
    if (!sensitive && telemetry_type(key) == TelemetryType::unknown) {
        return FieldPolicy::drop;
    }
    switch (declared) {
    case Sensitivity::identifier:
        return FieldPolicy::pseudonym;
    case Sensitivity::location:
    case Sensitivity::credential:
        return FieldPolicy::redact;
    case Sensitivity::plain:
        break;
    }
    if (sensitive) {
        return FieldPolicy::redact;
    }
    return FieldPolicy::pass;
}

} // namespace aa::diagnostics
