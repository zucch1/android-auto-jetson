// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <aa/core/Time.hpp>

#include <chrono>

namespace aa::session {

// Narrow monotonic-clock seam for lifecycle deadlines. Injected so timeout
// behaviour is deterministic: production wraps std::chrono::steady_clock, tests
// drive a ManualClock. Wall-clock time never enters a budget (see core/Time).
class MonotonicClock {
public:
    virtual ~MonotonicClock() = default;
    MonotonicClock() = default;
    MonotonicClock(const MonotonicClock&) = delete;
    MonotonicClock& operator=(const MonotonicClock&) = delete;
    MonotonicClock(MonotonicClock&&) = delete;
    MonotonicClock& operator=(MonotonicClock&&) = delete;

    [[nodiscard]] virtual core::Nanoseconds now() const noexcept = 0;
};

// Production source: std::chrono::steady_clock is monotonic by contract.
class SteadyClock final : public MonotonicClock {
public:
    [[nodiscard]] core::Nanoseconds now() const noexcept override {
        const auto ticks = std::chrono::steady_clock::now().time_since_epoch();
        return core::Nanoseconds{
            std::chrono::duration_cast<std::chrono::nanoseconds>(ticks).count()};
    }
};

} // namespace aa::session
