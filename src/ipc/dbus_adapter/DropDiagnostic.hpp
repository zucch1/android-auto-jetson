// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <chrono>
#include <cstdint>
#include <optional>

namespace aa::ipc::dbus_adapter {

// One instance per connection. Explicit time keeps the fixed-window policy
// testable without sleeping; the transport retains its lifetime drop counter.
class DropDiagnostic final {
public:
    using Clock = std::chrono::steady_clock;
    static constexpr auto kInterval = std::chrono::seconds(10);

    [[nodiscard]] std::optional<std::uint64_t> record(Clock::time_point now) {
        ++pending_;
        if (next_ && now < *next_) return std::nullopt;
        const auto count = pending_;
        pending_ = 0;
        next_ = now + kInterval;
        return count;
    }

private:
    std::optional<Clock::time_point> next_;
    std::uint64_t pending_{};
};

} // namespace aa::ipc::dbus_adapter
