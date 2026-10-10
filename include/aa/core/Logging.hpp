// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace aa::core {

// Logging boundary. Modules never talk to a logging backend directly; they log
// through a Logger and the process-owned LogSink (implementation is a private
// adapter). Rules enforced by contract and by review:
//  - `message` is a stable literal (it is a diagnostics schema field); dynamic
//    data goes into `fields`.
//  - every field value has already crossed the diagnostics redaction boundary
//    (see aa/diagnostics/Diagnostics.hpp): phone identifiers, Bluetooth MACs,
//    locations and credentials are pseudonyms, never raw.
//  - levels are chosen by consumer: error = operator/release gate, warning =
//    degraded-but-continuing, info = session lifecycle, debug/trace = developers.
enum class LogLevel { trace, debug, info, warning, error };

struct LogField final {
    std::string key;
    std::string value;
};

struct LogEvent final {
    LogLevel level;
    std::string_view message;
    std::vector<LogField> fields;
};

// Implementations live in private adapters and must tolerate write() calls from
// any thread. Ownership: the sink is owned by the process entry point; Logger
// holds a non-owning reference and never outlives it.
class LogSink {
public:
    virtual ~LogSink() = default;
    LogSink() = default;
    LogSink(const LogSink&) = delete;
    LogSink& operator=(const LogSink&) = delete;
    LogSink(LogSink&&) = delete;
    LogSink& operator=(LogSink&&) = delete;

    virtual void write(const LogEvent& event) = 0;
};

// Copyable, non-owning facade. Safe to call from any thread; filtering happens
// before the sink sees the event.
class Logger final {
public:
    explicit Logger(LogSink& sink, LogLevel minimum = LogLevel::info) noexcept
        : sink_(&sink), minimum_(minimum) {}

    void log(LogLevel level, std::string_view message,
             std::vector<LogField> fields = {}) const;

    [[nodiscard]] LogLevel minimum() const noexcept { return minimum_; }

private:
    LogSink* sink_;
    LogLevel minimum_;
};

} // namespace aa::core
