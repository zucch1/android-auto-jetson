// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <compare>
#include <cstdint>

namespace aa::core {

// Strong time units. Contract budgets (pairing window, reconnect, A/V sync,
// audio latency) are declared in these units so milliseconds and nanoseconds
// never mix silently. Monotonic clocks only; wall-clock time never enters a
// budget or a frame timestamp.
struct Milliseconds final {
    std::int64_t count{};

    [[nodiscard]] friend constexpr bool operator==(Milliseconds,
                                                   Milliseconds) noexcept = default;
    [[nodiscard]] friend constexpr auto operator<=>(Milliseconds,
                                                    Milliseconds) noexcept = default;
};

struct Nanoseconds final {
    std::int64_t count{};

    [[nodiscard]] friend constexpr bool operator==(Nanoseconds, Nanoseconds) noexcept = default;
    [[nodiscard]] friend constexpr auto operator<=>(Nanoseconds, Nanoseconds) noexcept = default;
};

// Budget-scale values only (well below 2^63 / 10^6 ms), so the multiply cannot
// overflow for anything a contract declares.
[[nodiscard]] constexpr Nanoseconds to_nanoseconds(Milliseconds milliseconds) noexcept {
    return Nanoseconds{milliseconds.count * 1'000'000};
}

[[nodiscard]] constexpr Milliseconds to_milliseconds_floor(Nanoseconds nanoseconds) noexcept {
    return Milliseconds{nanoseconds.count / 1'000'000};
}

} // namespace aa::core
