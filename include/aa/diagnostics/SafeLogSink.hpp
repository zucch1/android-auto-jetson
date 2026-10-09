// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <aa/core/Logging.hpp>

namespace aa::diagnostics {

// Safe core::LogSink adapter (diagnostics-owned). core::Logger's contract asks
// callers to pass only pre-redacted fields; that is a caller promise, and this
// adapter turns it into enforcement: every message and every field key/value
// crosses the diagnostics exact-name/value schema before the backend sees it.
// Arbitrary messages are replaced by "[redacted]", unknown field names drop,
// and arbitrary strings on known names redact.
//
// Wiring: place SafeLogSink in front of the process-owned backend sink and
// hand the SafeLogSink to core::Logger. Reserved envelope keys (kind/session/
// mono_ns) are dropped from fields, matching the serializer's schema. The
// message must exactly equal an EventKind's to_string() name. Fields share the
// serializer's schema; the string-only core API's counts/rates/flags are parsed
// completely before being accepted. Numeric covert data cannot be identified
// as secret; authorized telemetry must be used for its declared purpose.
//
// Threading: same contract as core::LogSink - write() may be called from any
// thread and must be forwarded to the downstream sink synchronously (the
// redacted message view is valid only for the duration of that call).
class SafeLogSink final : public core::LogSink {
public:
    explicit SafeLogSink(core::LogSink& downstream) noexcept : downstream_(&downstream) {}

    void write(const core::LogEvent& event) override;

private:
    core::LogSink* downstream_;
};

} // namespace aa::diagnostics
