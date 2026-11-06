// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Exit status of aa-dummy-display after its event loop returns (#60, PR #84).
// Both telemetry writers are closed with a bounded timeout; a lost event on
// either one changes the exit status, so the stdout events and the
// human-readable stderr diagnostics are each accounted for.
//
//   3  a sink timed out: its stream is stuck, and stdout and stderr may share a
//      destination, so nothing more is written to either
//   1  events were undelivered (full queue or failed write) and the loop
//      itself succeeded; a short note goes to `report`
//   otherwise the event loop's own outcome

#include <aa/display/TelemetryWriter.hpp>

#include <chrono>
#include <cstddef>
#include <functional>
#include <string>

namespace aa::display::shutdown {

inline constexpr int kSinkTimedOut = 3;
inline constexpr int kEventsUndelivered = 1;

inline int finish(int outcome, TelemetryWriter& diagnostics, TelemetryWriter& telemetry,
                  const std::function<void(const std::string&)>& report,
                  std::chrono::milliseconds timeout = std::chrono::seconds{2}) {
    const bool diagnostics_ok = diagnostics.close(timeout);
    const bool telemetry_ok = telemetry.close(timeout);
    if (diagnostics.timed_out() || telemetry.timed_out()) {
        return kSinkTimedOut;
    }
    if (!telemetry_ok) {
        report("aa-dummy-display: " + std::to_string(telemetry.undelivered()) +
               " telemetry event(s) undelivered\n");
    }
    if (!diagnostics_ok) {
        // The stderr sink failed or overflowed; this note may not arrive either,
        // which is why the exit status carries it.
        report("aa-dummy-display: " + std::to_string(diagnostics.undelivered()) +
               " diagnostic line(s) undelivered\n");
    }
    if ((!telemetry_ok || !diagnostics_ok) && outcome == 0) {
        return kEventsUndelivered;
    }
    return outcome;
}

}  // namespace aa::display::shutdown
