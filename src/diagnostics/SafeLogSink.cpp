// SPDX-License-Identifier: GPL-3.0-or-later
#include <aa/diagnostics/SafeLogSink.hpp>

#include <aa/diagnostics/Diagnostics.hpp>

#include "Policy.hpp"

#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace aa::diagnostics {
namespace {

std::optional<std::string> enforce(const core::LogField& field) {
    switch (field_policy(field.key, Sensitivity::plain)) {
    case FieldPolicy::drop:
        return std::nullopt;
    case FieldPolicy::pass:
        return is_safe_log_value(telemetry_type(field.key), field.value)
                   ? field.value : "[redacted]";
    case FieldPolicy::pseudonym:
        return redact_identifier(field.value);
    case FieldPolicy::redact:
        return std::string{"[redacted]"};
    }
    return std::nullopt;
}

} // namespace

void SafeLogSink::write(const core::LogEvent& event) {
    const std::string_view safe_message = safe_log_message(event.message);
    std::vector<core::LogField> safe_fields;
    safe_fields.reserve(event.fields.size());
    for (const core::LogField& field : event.fields) {
        std::optional<std::string> value = enforce(field);
        if (!value) {
            continue;
        }
        safe_fields.push_back(core::LogField{field.key, std::move(*value)});
    }
    downstream_->write(core::LogEvent{event.level, safe_message, std::move(safe_fields)});
}

} // namespace aa::diagnostics
