// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <optional>

namespace aa::audio::detail {

// Pair the Pulse read endpoint with the driver period in which it was
// sampled. Endpoints count through that period and the async output link
// presents its buffer in the next period. Both offsets change with quantum.
class ReaderPhase final {
public:
    struct Read { std::int64_t at, endpoint_ns; };

    void ambiguous_epoch(std::int64_t at) { reconcile_after_ = at; }
    bool reconciling() const { return reconcile_after_.has_value() && baseline_.has_value(); }

    std::int64_t cycle(std::int64_t ns, std::int64_t period, std::optional<Read> read) {
        std::int64_t correction = 0;
        if (read && previous_ && read->at > previous_->at + 500'000 &&
            read->at < ns - 500'000 && ns - previous_->at < previous_->period * 3 / 2) {
            const auto phase = previous_->at + 2 * previous_->period - read->endpoint_ns;
            if (!baseline_) {
                baseline_ = phase;
            } else if (reconcile_after_ && previous_->at > *reconcile_after_ + period * 2) {
                correction = phase - *baseline_;
                reconcile_after_.reset();
                baseline_ = phase;
            } else if (!reconcile_after_) {
                baseline_ = phase;
            }
        }
        previous_ = Cycle{ns, period};
        return correction;
    }
    void held(std::int64_t ns) { if (baseline_) { *baseline_ += ns; } }
private:
    struct Cycle { std::int64_t at, period; };
    std::optional<Cycle> previous_;
    std::optional<std::int64_t> baseline_, reconcile_after_;
};
}
