// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <optional>

namespace aa::audio::detail {

// Where the Pulse reader lies on the driver's timeline.
//
// A sample the follower reads in a cycle is presented a fixed number of
// periods later, so while every cycle completes the presentation delay moves
// only with the driver: its time against its ticks, and the period. That gives
// the displacement of a driver stall, restart or new quantum as soon as the
// driver runs evenly again.
//
// A Pulse latency reply measures the same thing independently: the endpoint
// read through the period it was sampled in, against that period. Replies are
// seconds apart on a steady stream, so they do not time the step; they close
// what the cycle records left out. The frame offset between ring and graph is
// an exact integer between faults, so distinct reads that agree confirm it
// however the driver clock is slewing.
class ReaderPhase final {
public:
    struct Read { std::int64_t at, endpoint_ns; };
    struct Cycle {
        std::int64_t at, tick, period;
        bool completed{}; // the follower finished this cycle,
        bool missed{};    // or never woke for it
        std::int64_t finished{}; // when the follower completed this cycle
    };
    struct Change {
        std::int64_t step{};  // a displacement of the reader, applied at once
        std::int64_t drift{}; // a remainder, or driver progress
    };

    // The driver stalled, restarted or changed its quantum: cycle records no
    // longer tell which periods were consumed.
    void ambiguous_epoch(std::int64_t at) {
        if (!ambiguous_since_) { ambiguous_since_ = at; }
        settled_ = 0;
        agreeing_ = 0;
    }
    bool reconciling() const { return ambiguous_since_.has_value() && shape_.has_value(); }

    Change cycle(const Cycle& now, bool relocking, std::optional<Read> read) {
        Change change;
        const auto shape = now.at - now.tick + 2 * now.period;
        const bool repeated = previous_ && now.tick == previous_->tick;
        const bool even = previous_ && std::abs(now.tick - previous_->tick - previous_->period) <= 2
            && now.at - previous_->at < previous_->period * 3 / 2;
        if (!now.completed) {
            // One that woke late in a burst still reads its period.
            if (now.missed && !repeated) { missed_ += now.period; }
            settled_ = 0;
        } else if (!ambiguous_since_) {
            shape_ = shape;
            missed_ = 0;
        } else {
            settled_ = even ? settled_ + 1 : 0;
            if (!shape_ || settled_ >= kSettledCycles) {
                if (shape_) { change.step = shape + missed_ - *shape_; }
                held(change.step);
                shape_ = shape;
                missed_ = 0;
                ambiguous_since_.reset();
                verify_ = true;
            } else if (now.at - *ambiguous_since_ > kGiveUpNs) {
                shape_.reset();
            }
        }
        if (!ambiguous_since_ && previous_ && read && usable(*previous_, now, *read)) {
            const auto offset = previous_->tick + 2 * previous_->period - read->endpoint_ns;
            agreeing_ = agreeing_ && std::abs(offset - offset_) < kAgreeNs ? agreeing_ + 1 : 1;
            offset_ = offset;
            const auto phase = previous_->at - previous_->tick + offset;
            if (agreeing_ >= 2 && !applied_) {
                applied_ = phase;
            } else if (agreeing_ >= 2) {
                // A displacement no record showed needs a third read. What
                // integrating the driver left over while it relocks does not,
                // and measurement noise is smoothed.
                const auto error = phase - *applied_;
                const bool exact = std::abs(error) > kNoiseNs ? verify_ || agreeing_ >= 3 : relocking;
                verify_ = verify_ && !exact && std::abs(error) > kNoiseNs;
                change.drift = exact ? error : std::clamp(error / 8, -kSlewNs, kSlewNs);
                *applied_ += change.drift;
            }
        }
        previous_ = now;
        return change;
    }
    // The timeline moved by `ns` on the cycle record's account. Missed cycles
    // change the frame offset, so it has to be confirmed again.
    void held(std::int64_t ns) {
        drifted(ns);
        agreeing_ = 0;
    }
    void drifted(std::int64_t ns) {
        if (applied_) { *applied_ += ns; }
    }
private:
    static constexpr int kSettledCycles = 8;
    static constexpr std::int64_t kAgreeNs = 100'000;
    static constexpr std::int64_t kNoiseNs = 3'000'000;
    static constexpr std::int64_t kSlewNs = 200'000;
    static constexpr std::int64_t kGiveUpNs = 5'000'000'000;

    // Keep clear of both period edges, where a latency reply and the
    // server's process completion can cross.
    static bool usable(const Cycle& from, const Cycle& to, const Read& read) {
        const auto edge = std::min<std::int64_t>(1'000'000, from.period / 8);
        return from.completed && read.at > std::max(from.at, from.finished) + edge && read.at < to.at - edge
            && to.at - from.at < from.period * 3 / 2;
    }
    std::optional<Cycle> previous_;
    std::optional<std::int64_t> shape_, applied_, ambiguous_since_;
    std::int64_t offset_{}, missed_{};
    unsigned agreeing_{};
    int settled_{};
    bool verify_{}; // the next confirmed reads check a step taken from cycle records
};
}
