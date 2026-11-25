// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <optional>

namespace aa::audio::detail {

// Called only from the PipeWire loop. Duration and position are graph ticks
// from the profiler record that contains this follower's activation state.
class GraphCycles final {
public:
    struct Change {
        std::int64_t held_ns{};
        bool discontinuity{};
        std::int64_t drift_ns{};
    };

    Change observe(std::int64_t position, std::int64_t frames, std::uint32_t rate, int state, std::int64_t ns) {
        Change change;
        // A timed-out follower can finish after the driver has already
        // reported that cycle. PipeWire then publishes its final state
        // at the same graph position before advancing to the next cycle.
        const bool same_cycle = expected_ && position == *expected_ - previous_frames_
            && frames == previous_frames_ && rate == previous_rate_;
        if (same_cycle && (state != 3 || !missed_cycles_)) { return change; }
        if (!same_cycle && expected_ && (position != *expected_ || frames != previous_frames_ || rate != previous_rate_)) {
            change.discontinuity = true;
            missed_ns_ = 0;
            missed_cycles_ = 0;
        }
        // Compare graph time and nominal frame duration using the timestamp
        // captured by the driver. Delivery time of the profiler batch plays
        // no part. Move the phase at most 500 ppm, in one-second windows.
        if (!reference_ || change.discontinuity || ns < reference_->time) {
            reference_ = Reference{position, ns, 0};
        } else if (ns - reference_->time >= 1'000'000'000) {
            const auto elapsed = ns - reference_->time;
            const auto ticks = position - reference_->position;
            const auto nominal = ticks / rate * 1'000'000'000 + ticks % rate * 1'000'000'000 / rate;
            const auto error = elapsed - nominal + reference_->residual;
            if (std::abs(error) <= 40'000'000) {
                change.drift_ns = std::clamp(error, -elapsed / 2000, elapsed / 2000);
                reference_ = Reference{position, ns, error - change.drift_ns};
            } else {
                change.discontinuity = true;
                reference_ = Reference{position, ns, 0};
            }
        }
        expected_ = position + frames;
        previous_frames_ = frames;
        previous_rate_ = rate;
        // A triggered or awake follower did not finish this graph cycle.
        if (state == 1 || state == 2) {
            ++missed_cycles_;
            missed_ns_ += frames * 1'000'000'000 / rate;
        } else if (state == 3 && missed_cycles_) {
            if (missed_cycles_ >= 2) { change.held_ns = missed_ns_; }
            else { change.discontinuity = true; }
            missed_cycles_ = 0;
            missed_ns_ = 0;
        }
        return change;
    }
private:
    struct Reference { std::int64_t position, time, residual; };
    std::optional<Reference> reference_;
    std::optional<std::int64_t> expected_;
    std::int64_t previous_frames_{}, missed_ns_{};
    std::uint32_t previous_rate_{};
    unsigned missed_cycles_{};
};
}
