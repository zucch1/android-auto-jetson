// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <cstdint>

namespace aa::audio::detail {

// Smallest value of the last 4 to 8 seconds (the playout depth window).
class RecentLow final {
public:
    void sample(std::int64_t at, std::int64_t value) {
        if (!started_ || at - since_ >= kSpanNs) {
            previous_ = started_ && at - since_ < 2 * kSpanNs ? current_ : value;
            current_ = value;
            since_ = at;
            started_ = true;
        }
        current_ = std::min(current_, value);
    }
    std::int64_t low() const { return started_ ? std::min(previous_, current_) : 0; }
private:
    static constexpr std::int64_t kSpanNs = 4'000'000'000;
    std::int64_t since_{}, current_{}, previous_{};
    bool started_{};
};
}
