// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <aa/diagnostics/Diagnostics.hpp>

#include <string>
#include <string_view>

namespace aa::diagnostics {

// Shared emission policy: exact known names, declared sensitivity, then typed
// value validation. Unknown names are dropped because the name can be a secret.
enum class FieldPolicy {
    drop,
    pseudonym,
    redact,
    pass,
};

enum class TelemetryType { unknown, state, version, reason, status, decision,
                           transport, codec, count, number, flag, count_or_flag };

// Normalization is only for the pattern scrubber, never for emission names.
[[nodiscard]] std::string normalize_key(std::string_view key);
[[nodiscard]] bool is_sensitive_key(const std::string& normalized_key) noexcept;
[[nodiscard]] TelemetryType telemetry_type(std::string_view key) noexcept;
[[nodiscard]] FieldPolicy field_policy(std::string_view key, Sensitivity declared);
[[nodiscard]] bool is_safe_value(std::string_view key, const FieldValue& value);
[[nodiscard]] bool is_safe_log_value(TelemetryType type, std::string_view value);
[[nodiscard]] std::string_view safe_log_message(std::string_view message) noexcept;

} // namespace aa::diagnostics
