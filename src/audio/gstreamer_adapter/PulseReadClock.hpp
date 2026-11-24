// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "../AudioSinkFactory.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <mutex>
#include <optional>
#include <utility>

namespace aa::audio::detail {

// Absolute write endpoint plus Pulse's writable credit tracks the server's
// read position with a constant origin. The interpolated GstAudioClock can
// keep advancing during a pause and take seconds to settle after it.
//
// Until the sink activates this clock the router's write-lateness fallback
// handles pauses, and every hold it applies is passed to fallback_held(). The
// clock moves its baseline by that hold, so it never confirms the same pause a
// second time, and once eight requests after the hold have arrived it hands
// the fallback's estimation error back as drift (#229 P1, P2-2).
class PulseReadClock final {
public:
    void reset(std::int64_t bytes_per_second) {
        std::lock_guard lock(mutex_);
        bytes_per_second_ = bytes_per_second;
        write_end_bytes_.reset();
        phase_count_ = 0;
        phase_index_ = 0;
        last_request_ = 0;
        candidate_.reset();
        held_ = 0;
        drift_ = 0;
        mapped_phase_.reset();
        baseline_.reset();
        reconcile_ = false;
        active_ = false;
        rate_reference_.reset();
        skew_ppm_ = 0.0;
        scheduled_end_.reset();
        starving_ = false;
        suspect_.reset();
        starvations_ = 0;
        starved_ns_ = 0;
    }

    void submitted(std::int64_t end) {
        std::lock_guard lock(mutex_);
        write_end_bytes_ = end;
    }

    // Pulse's negotiated target length (the stream's tlength), in bytes.
    // Kept across reset(): the Pulse stream outlives the router's streams.
    void set_target_length(std::int64_t bytes) {
        std::lock_guard lock(mutex_);
        target_bytes_ = bytes;
    }

    // Where the router plans the end of the newest handed-over write to play
    // (observation time). Starvation is measured against it.
    void scheduled(std::int64_t end) {
        std::lock_guard lock(mutex_);
        scheduled_end_ = end;
    }

    void requested(std::int64_t writable, std::int64_t now) {
        std::lock_guard lock(mutex_);
        if (bytes_per_second_ == 0 || writable <= 0 || !write_end_bytes_
            || *write_end_bytes_ > INT64_MAX - writable) {
            return;
        }
        const auto bytes = *write_end_bytes_ + writable;
        const auto read_time = to_ns(bytes);
        const auto phase = now - read_time;
        const auto elapsed = now - std::exchange(last_request_, now);
        observe_starvation(writable, now);
        if (mapped_phase_ && !reconcile_) {
            const auto delta = phase - *mapped_phase_;
            const int direction = delta > kPhaseStepNs ? 1 : delta < -kPhaseStepNs ? -1 : 0;
            if (direction != 0) {
                observe_step(direction, phase, now, read_time);
                return;
            }
            candidate_.reset();
        }

        remember(phase);
        if (phase_count_ != phases_.size()) { return; }
        const auto reference = *std::min_element(phases_.begin(), phases_.end());
        if (reconcile_) {
            // Eight requests after a fallback hold: the router's schedule
            // moved by that hold, the reader by the real pause. Hand the
            // difference over as drift: it moves only the wall-clock mapping.
            if (const auto base = mapped_phase_ ? mapped_phase_ : baseline_) {
                drift_ += reference - *base;
            }
            mapped_phase_ = reference;
            baseline_.reset();
            reconcile_ = false;
            rate_reference_.reset();
            return;
        }
        update_rate(now, reference);
        if (!mapped_phase_) { mapped_phase_ = reference; }
        // Keep the router's wall-clock mapping on the read clock as the PCM
        // rate follows it. Short gaps accumulate against this applied phase.
        const auto limit = std::min<std::int64_t>(elapsed, 100'000'000) / 2'000;
        const auto drift = std::clamp(reference - *mapped_phase_, -limit, limit);
        *mapped_phase_ += drift;
        drift_ += drift;
    }

    // The router's write-lateness fallback moved the schedule by `by` while
    // this clock was not yet driving it. Phases observed so far belong to the
    // old schedule; a step already confirmed but not handed over is the same
    // pause and is dropped in favour of the fallback's hold.
    void fallback_held(std::int64_t by) {
        std::lock_guard lock(mutex_);
        if (by == 0 || active_) { return; }
        candidate_.reset();
        rate_reference_.reset();
        if (mapped_phase_) {
            *mapped_phase_ += by - std::exchange(held_, 0);
            reconcile_ = true;
        } else {
            // At least two requests are needed to trust a pre-pause phase.
            std::optional<std::int64_t> low = baseline_;
            if (phase_count_ >= 2) {
                const auto observed = *std::min_element(phases_.begin(), phases_.begin()
                    + static_cast<std::ptrdiff_t>(phase_count_));
                low = low ? std::min(*low, observed) : observed;
            }
            if (low) { baseline_ = *low + by; }
            reconcile_ = baseline_.has_value();
        }
        phase_count_ = 0;
        phase_index_ = 0;
    }

    // The clock drives the router's schedule from the first on-time write
    // after it is ready. A write that just blocked must first reach the
    // router's lateness fallback. Until then the clock keeps what it measured
    // (a step the fallback did not apply, the fallback's error, drift).
    bool activate(bool write_on_time) {
        std::lock_guard lock(mutex_);
        if (!active_ && write_on_time && mapped_phase_ && !reconcile_) { active_ = true; }
        return active_;
    }

    [[nodiscard]] bool active() const {
        std::lock_guard lock(mutex_);
        return active_;
    }

    [[nodiscard]] SinkTimingAdjustment take_adjustment() {
        std::lock_guard lock(mutex_);
        if (!active_) { return {}; }
        return {core::Nanoseconds{std::exchange(held_, 0)}, {}, core::Nanoseconds{std::exchange(drift_, 0)}};
    }

    [[nodiscard]] bool is_playing(std::int64_t now) const {
        std::lock_guard lock(mutex_);
        // A step confirmed between writes has not yet reached the router.
        // Leave the sink stamps contiguous until it has moved the schedule.
        return mapped_phase_ && !candidate_ && !reconcile_ && held_ == 0
            && now - last_request_ < 85'000'000;
    }

    [[nodiscard]] bool ready() const {
        std::lock_guard lock(mutex_);
        return mapped_phase_.has_value() && !reconcile_;
    }

    [[nodiscard]] double skew_ppm() const {
        std::lock_guard lock(mutex_);
        return skew_ppm_;
    }

    // Starvation episodes (the reader passed the newest written byte) and
    // the total time the reader ran ahead, since the last call.
    [[nodiscard]] std::pair<std::uint64_t, std::int64_t> take_starvation() {
        std::lock_guard lock(mutex_);
        return {std::exchange(starvations_, 0), std::exchange(starved_ns_, 0)};
    }

private:
    struct Candidate {
        int direction;
        std::int64_t since;
        std::int64_t read_start;
        std::array<std::int64_t, 3> recent;
        std::size_t count{1};
    };

    [[nodiscard]] std::int64_t to_ns(std::int64_t bytes) const {
        return bytes / bytes_per_second_ * 1'000'000'000
            + bytes % bytes_per_second_ * 1'000'000'000 / bytes_per_second_;
    }

    // Pulse grants at most tlength while the write index leads the read
    // index. More credit than that means the reader is past the newest byte:
    // everything written at the current offsets lands behind it and is
    // discarded, with no phase change (#229 P1). Report it and move the
    // router's schedule onto the reader; the sink's ingress move then puts the
    // next buffer ahead of it again.
    void observe_starvation(std::int64_t writable, std::int64_t now) {
        if (target_bytes_ <= 0) { return; }
        const auto behind = to_ns(writable - target_bytes_);
        if (writable <= target_bytes_ || behind <= kStarvedNs) {
            starving_ = false;
            suspect_.reset();
            return;
        }
        if (starving_ || !scheduled_end_) { return; }
        // The newest byte played `behind` ago; the router plans it at
        // scheduled_end_. A reader that ran out at the end of a stream agrees
        // with that plan, give or take the segment pulsesink has not flushed.
        // One that stays past the write index while more than two further
        // segments are committed has starved: what is written lands behind
        // the reader. At the end of a stream pulsesink flushes at most one.
        const auto error = *scheduled_end_ - (now - behind);
        if (error <= kStarvedNs) {
            suspect_.reset();
            return;
        }
        if (!suspect_) {
            suspect_ = *write_end_bytes_;
            return;
        }
        if (*write_end_bytes_ - *suspect_ <= kStarvedCommitBytes) { return; }
        starving_ = true;
        suspect_.reset();
        ++starvations_;
        starved_ns_ += behind;
        // The reader itself kept its phase: only the router's schedule moves.
        held_ -= error;
    }

    void observe_step(int direction, std::int64_t phase, std::int64_t now, std::int64_t read_time) {
        if (!candidate_ || candidate_->direction != direction) {
            candidate_ = Candidate{direction, now, read_time, {phase}};
            return;
        }
        auto& step = *candidate_;
        step.recent[step.count++ % step.recent.size()] = phase;
        // Pulse can batch credit messages and vary request sizes with graph
        // timing. Seek a new phase over real read progress and wall time so
        // multiple callbacks in one burst cannot confirm a step.
        if (step.count < step.recent.size() || now - step.since < 160'000'000
            || read_time - step.read_start < 64'000'000) { return; }
        const auto settled = *std::min_element(step.recent.begin(), step.recent.end());
        held_ += settled - *mapped_phase_;
        mapped_phase_ = settled;
        phases_.fill(settled);
        phase_count_ = phases_.size();
        phase_index_ = 0;
        candidate_.reset();
        rate_reference_.reset();
    }

    void remember(std::int64_t phase) {
        phases_[phase_index_] = phase;
        phase_index_ = (phase_index_ + 1) % phases_.size();
        phase_count_ = std::min(phase_count_ + 1, phases_.size());
    }

    void update_rate(std::int64_t now, std::int64_t phase) {
        if (!rate_reference_) { rate_reference_ = std::pair{now, phase}; }
        const auto elapsed = now - rate_reference_->first;
        if (elapsed >= 15'000'000'000) {
            const auto delta = phase - rate_reference_->second;
            const double ppm = static_cast<double>(delta) * 1e6 / static_cast<double>(elapsed);
            skew_ppm_ += (std::clamp(ppm, -500.0, 500.0) - skew_ppm_) / 2.0;
            rate_reference_ = std::pair{now, phase};
        }
    }

    static constexpr std::int64_t kPhaseStepNs = 24'000'000;
    // Credit beyond tlength by more than this is starvation, not batching.
    static constexpr std::int64_t kStarvedNs = 10'000'000;
    // Two 1,024-frame stereo segments.
    static constexpr std::int64_t kStarvedCommitBytes = 8'192;
    mutable std::mutex mutex_{};
    std::int64_t bytes_per_second_{};
    std::optional<std::int64_t> write_end_bytes_{};
    std::int64_t last_request_{};
    std::array<std::int64_t, 8> phases_{};
    std::size_t phase_count_{};
    std::size_t phase_index_{};
    std::optional<Candidate> candidate_{};
    std::int64_t held_{};
    std::int64_t drift_{};
    std::optional<std::int64_t> mapped_phase_{};
    // Before the first mapping: the pre-hold phase in the current schedule.
    std::optional<std::int64_t> baseline_{};
    bool reconcile_{};
    bool active_{};
    std::optional<std::pair<std::int64_t, std::int64_t>> rate_reference_{};
    double skew_ppm_{};
    std::int64_t target_bytes_{};
    std::optional<std::int64_t> scheduled_end_{};
    bool starving_{};
    // The write index when the reader was first seen past it.
    std::optional<std::int64_t> suspect_{};
    std::uint64_t starvations_{};
    std::int64_t starved_ns_{};
};

// Device rate without the request trace: the pulsesink's own clock follows
// the server's playback position. Long windows that contain no hold give the
// device skew against monotonic time (positive: the device reads slower).
class DeviceRateEstimate final {
public:
    void reset() {
        reference_.reset();
        ppm_ = 0.0;
    }

    // A pause or hold inside a window would read as skew.
    void restart() { reference_.reset(); }

    void sample(std::int64_t now, std::int64_t device) {
        if (!reference_) {
            reference_ = std::pair{now, device};
            return;
        }
        const auto elapsed = now - reference_->first;
        if (elapsed < kWindowNs) { return; }
        const auto behind = elapsed - (device - reference_->second);
        const double ppm = static_cast<double>(behind) * 1e6 / static_cast<double>(elapsed);
        ppm_ += (std::clamp(ppm, -500.0, 500.0) - ppm_) / 2.0;
        reference_ = std::pair{now, device};
    }

    [[nodiscard]] double ppm() const { return ppm_; }

private:
    static constexpr std::int64_t kWindowNs = 15'000'000'000;
    std::optional<std::pair<std::int64_t, std::int64_t>> reference_{};
    double ppm_{};
};

} // namespace aa::audio::detail
