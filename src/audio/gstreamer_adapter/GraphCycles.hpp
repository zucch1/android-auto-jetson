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
        std::uint64_t suspend_loss_frames{};
    };

    // Some ALSA drivers run faster briefly after an xrun while their PLL
    // settles. Count that measured progress against the hold exactly once.
    void follow_driver() {
        relocking_ = true;
        stable_ns_ = 0;
        reference_.reset();
    }
    bool relocking() const { return relocking_; }

    void suspend() { resuming_ = expected_time_.has_value(); }

    Change observe(std::int64_t position, std::int64_t frames, std::uint32_t rate, int state,
                   std::int64_t ns, bool sink_running = true) {
        Change change;
        // A timed-out follower can finish after the driver has already
        // reported that cycle. PipeWire then publishes its final state
        // at the same graph position before advancing to the next cycle.
        const bool same_cycle = expected_ && position == *expected_ - previous_frames_
            && frames == previous_frames_ && rate == previous_rate_;
        if (same_cycle && (state != 3 || !missed_cycles_)) { return change; }
        const auto duration_ns = frames * 1'000'000'000 / rate;
        if (resuming_) {
            if (state != 3 || same_cycle) { return change; }
            // This period was consumed on a disconnected route or was in
            // flight at reactivation. The device cannot confirm those frames.
            change.suspend_loss_frames = static_cast<std::uint64_t>(frames);
            if (!sink_running) {
                *expected_time_ += duration_ns;
                expected_ = position + frames;
                previous_frames_ = frames;
                previous_rate_ = rate;
                return change;
            }
            // Start the new driver epoch at its measured time. A restarted
            // driver may begin earlier or later than its next scheduled tick.
            change.held_ns = ns - *expected_time_;
            *this = {};
        }
        expected_time_ = ns + duration_ns;
        if (!same_cycle && expected_ && (position != *expected_ || frames != previous_frames_ || rate != previous_rate_)) {
            change.discontinuity = true;
            missed_ns_ = 0;
            missed_cycles_ = 0;
        }
        const auto lag = ns - (position / rate * 1'000'000'000 + position % rate * 1'000'000'000 / rate);
        // A lost record or a new quantum moves position and time together.
        if (change.discontinuity && completed_lag_ && std::abs(lag - *completed_lag_) > 40'000'000) {
            completed_lag_.reset();
        }
        // Compare graph time and nominal frame duration using the timestamp
        // captured by the driver. Delivery time of the profiler batch plays
        // no part. Move the phase at most 500 ppm, in one-second windows.
        // The hold itself accounts for the driver during missed cycles.
        if (missed_cycles_ || state == 1 || state == 2) {
            reference_.reset();
        } else if (!reference_ || change.discontinuity || ns < reference_->time) {
            reference_ = Reference{position, ns, 0};
        } else if (ns - reference_->time >= (relocking_ ? 250'000'000 : 1'000'000'000)) {
            const auto elapsed = ns - reference_->time;
            const auto ticks = position - reference_->position;
            const auto nominal = ticks / rate * 1'000'000'000 + ticks % rate * 1'000'000'000 / rate;
            const auto error = elapsed - nominal + reference_->residual;
            if (relocking_ && std::abs(error) <= elapsed / 8) {
                change.drift_ns = error;
                reference_ = Reference{position, ns, 0};
                stable_ns_ = std::abs(error) <= elapsed / 2000 ? stable_ns_ + elapsed : 0;
                if (stable_ns_ >= 2'000'000'000) { relocking_ = false; }
            } else if (std::abs(error) <= 40'000'000) {
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
            // A driver refilling its device runs fast during the stall: what
            // the listener loses is the elapsed time, not the nominal ticks.
            if (missed_cycles_ >= 2) {
                change.held_ns = missed_ns_ + (completed_lag_ ? lag - *completed_lag_ : 0);
            }
            else { change.discontinuity = true; }
            missed_cycles_ = 0;
            missed_ns_ = 0;
        }
        if (state == 3) { completed_lag_ = lag; }
        return change;
    }
private:
    struct Reference { std::int64_t position, time, residual; };
    std::optional<Reference> reference_;
    std::optional<std::int64_t> expected_, completed_lag_;
    std::int64_t previous_frames_{}, missed_ns_{};
    std::uint32_t previous_rate_{};
    unsigned missed_cycles_{};
    std::optional<std::int64_t> expected_time_;
    bool resuming_{}, relocking_{};
    std::int64_t stable_ns_{};
};
}
