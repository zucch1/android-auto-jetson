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
// Hand-over (#229). Until the sink activates this clock the router's
// write-lateness fallback handles pauses. Every hold it applies arrives here
// through fallback_held(), and the clock then forgets everything it measured:
// its phase baseline comes only from the eight requests after the most recent
// fallback hold (which the router applies once the blocked write has
// returned). Nothing measured before or during a fallback hold reaches the
// router, so a pause is held exactly once. Whatever the fallback's estimate got
// wrong stays as a residual A/V offset (docs/wired-live-qa.md bounds it).
//
// One applied record. Everything this clock hands the router (phase steps,
// drift, starvation recovery) is a change to the same schedule. The router's
// plan for the newest written byte (scheduled()) plus what is pending here
// is that record; starvation recovery measures against it and confirms a
// pending phase step instead of applying the same displacement a second time.
class PulseReadClock final {
public:
    void reset(std::int64_t bytes_per_second) {
        std::lock_guard lock(mutex_);
        bytes_per_second_ = bytes_per_second;
        write_end_bytes_.reset();
        granted_.reset();
        last_request_ = 0;
        forget();
        active_ = false;
        skew_ppm_ = 0.0;
        scheduled_end_.reset();
        starvations_ = 0;
        starved_ns_ = 0;
    }

    void submitted(std::int64_t end) {
        std::lock_guard lock(mutex_);
        write_end_bytes_ = end;
    }

    // A flush of [offset, end). A stamp that jumped ahead (a DISCONT: an
    // ingress move, or the part of a fallback shift kept out of the hold)
    // flushes at another offset, or past what the server last granted. A
    // credit the server computed before that flush reached it would read the
    // jump as a forward step: the next requests do not count.
    void submitted(std::int64_t offset, std::int64_t end) {
        std::lock_guard lock(mutex_);
        if ((write_end_bytes_ && offset != *write_end_bytes_) || (granted_ && end > *granted_)) {
            stale_requests_ = kStaleRequests;
        }
        write_end_bytes_ = end;
    }

    // Where the router plans the end of the newest handed-over write to play
    // (observation time), with every adjustment it has taken from here.
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
        granted_ = bytes;
        const auto read_time = to_ns(bytes);
        const auto phase = now - read_time;
        const auto elapsed = now - std::exchange(last_request_, now);
        if (stale_requests_ > 0) {
            --stale_requests_;
            return;
        }
        latest_phase_ = phase;
        if (mapped_phase_) {
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
        update_rate(now, reference);
        if (!mapped_phase_) {
            mapped_phase_ = reference;
            return;
        }
        // Keep the router's wall-clock mapping on the read clock as the PCM
        // rate follows it: at most 500 ppm of the time since the last request.
        const auto limit = std::min<std::int64_t>(elapsed, 100'000'000) / 2'000;
        const auto drift = std::clamp(reference - *mapped_phase_, -limit, limit);
        *mapped_phase_ += drift;
        drift_ += drift;
    }

    // The server's read index (pulsesink's latency update, about every 1.5 s
    // once a stream runs). It does not depend on Pulse's target length.
    // Past the newest written byte while more than two further segments are
    // committed, the ring has starved: what is written lands behind the
    // reader. A stream that ended flushes at most one more segment.
    void read_position(std::int64_t read_bytes, std::int64_t now) {
        std::lock_guard lock(mutex_);
        if (bytes_per_second_ == 0 || !write_end_bytes_) { return; }
        const auto ahead = to_ns(read_bytes - *write_end_bytes_);
        if (ahead <= kStarvedNs) {
            starving_ = false;
            suspect_.reset();
            return;
        }
        if (starving_ || !scheduled_end_) { return; }
        // The router plans its newest byte at scheduled_end_ (plus what it
        // has not yet taken from here); the reader passed it `ahead` ago. A
        // plan that has passed too is an underrun the ingress move repairs.
        const auto error = *scheduled_end_ + held_ + drift_ - (now - ahead);
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
        starved_ns_ += ahead;
        // A forward step the requests already show is this displacement:
        // confirm it now, so the step path cannot apply it a second time.
        if (candidate_ && candidate_->direction < 0) {
            const auto step = latest_phase_ - *mapped_phase_;
            apply_step(latest_phase_);
            if (-step >= error) { return; }
            held_ -= error + step;
            return;
        }
        held_ -= error;
    }

    // The router's write-lateness fallback moved the schedule by `by` while
    // this clock was not yet driving it. Everything measured so far is about
    // the old schedule, or straddles the pause: forget it, and take the
    // baseline from requests after this hold only.
    void fallback_held(std::int64_t by) {
        std::lock_guard lock(mutex_);
        if (by == 0 || active_) { return; }
        forget();
    }

    // The clock drives the router's schedule from the first on-time write
    // after it is ready. A write that just blocked must first reach the
    // router's lateness fallback.
    bool activate(bool write_on_time) {
        std::lock_guard lock(mutex_);
        if (!active_ && write_on_time && mapped_phase_ && !candidate_) { active_ = true; }
        return active_;
    }

    [[nodiscard]] bool active() const {
        std::lock_guard lock(mutex_);
        return active_;
    }

    [[nodiscard]] SinkTimingAdjustment take_adjustment() {
        std::lock_guard lock(mutex_);
        if (!active_) { return {}; }
        const auto held = std::exchange(held_, 0);
        const auto drift = std::exchange(drift_, 0);
        // The router's plan for the newest byte moves with what it takes.
        if (scheduled_end_) { *scheduled_end_ += held + drift; }
        return {core::Nanoseconds{held}, {}, core::Nanoseconds{drift}};
    }

    [[nodiscard]] bool is_playing(std::int64_t now) const {
        std::lock_guard lock(mutex_);
        // A step confirmed between writes has not yet reached the router.
        // Leave the sink stamps contiguous until it has moved the schedule.
        return mapped_phase_ && !candidate_ && held_ == 0
            && now - last_request_ < 85'000'000;
    }

    [[nodiscard]] bool ready() const {
        std::lock_guard lock(mutex_);
        return mapped_phase_.has_value();
    }

    [[nodiscard]] double skew_ppm() const {
        std::lock_guard lock(mutex_);
        return skew_ppm_;
    }

    // Starvation episodes (the reader passed the newest written byte while
    // writes went on) and the total time the reader ran ahead, since the last
    // call.
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

    // Drop the mapping and everything measured against it.
    void forget() {
        stale_requests_ = 0;
        phase_count_ = 0;
        phase_index_ = 0;
        candidate_.reset();
        held_ = 0;
        drift_ = 0;
        mapped_phase_.reset();
        rate_reference_.reset();
        starving_ = false;
        suspect_.reset();
    }

    void apply_step(std::int64_t settled) {
        const auto step = settled - *mapped_phase_;
        held_ += step;
        mapped_phase_ = settled;
        phases_.fill(settled);
        phase_count_ = phases_.size();
        phase_index_ = 0;
        candidate_.reset();
        rate_reference_.reset();
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
        apply_step(*std::min_element(step.recent.begin(), step.recent.end()));
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
    // Credit callbacks after a discontinuous flush that may predate it.
    static constexpr int kStaleRequests = 2;
    // The server advances its read index a 1,024-frame quantum (21.3 ms) at
    // a time; past the written end by more than this is starvation.
    static constexpr std::int64_t kStarvedNs = 25'000'000;
    // Two 1,024-frame stereo segments.
    static constexpr std::int64_t kStarvedCommitBytes = 8'192;
    mutable std::mutex mutex_{};
    std::int64_t bytes_per_second_{};
    std::optional<std::int64_t> write_end_bytes_{};
    std::int64_t last_request_{};
    int stale_requests_{};
    // The write index the server's latest credit allows (write end + credit).
    std::optional<std::int64_t> granted_{};
    std::int64_t latest_phase_{};
    std::array<std::int64_t, 8> phases_{};
    std::size_t phase_count_{};
    std::size_t phase_index_{};
    std::optional<Candidate> candidate_{};
    std::int64_t held_{};
    std::int64_t drift_{};
    std::optional<std::int64_t> mapped_phase_{};
    bool active_{};
    std::optional<std::pair<std::int64_t, std::int64_t>> rate_reference_{};
    double skew_ppm_{};
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
    explicit DeviceRateEstimate(std::int64_t window_ns = 15'000'000'000) : window_ns_(window_ns) {}

    void reset() {
        reference_.reset();
        ppm_ = 0.0;
    }

    // A pause or hold inside a window would read as skew. Samples taken
    // before `now` (an asynchronous sample still in flight) are ignored.
    void restart(std::int64_t now) {
        reference_.reset();
        not_before_ = now;
    }

    void sample(std::int64_t now, std::int64_t device) {
        if (now < not_before_) { return; }
        if (!reference_) {
            reference_ = std::pair{now, device};
            return;
        }
        const auto elapsed = now - reference_->first;
        if (elapsed < window_ns_) { return; }
        const auto behind = elapsed - (device - reference_->second);
        const double ppm = static_cast<double>(behind) * 1e6 / static_cast<double>(elapsed);
        ppm_ += (std::clamp(ppm, -500.0, 500.0) - ppm_) / 2.0;
        reference_ = std::pair{now, device};
    }

    [[nodiscard]] double ppm() const { return ppm_; }

private:
    std::int64_t window_ns_;
    std::int64_t not_before_{std::numeric_limits<std::int64_t>::min()};
    std::optional<std::pair<std::int64_t, std::int64_t>> reference_{};
    double ppm_{};
};

} // namespace aa::audio::detail
