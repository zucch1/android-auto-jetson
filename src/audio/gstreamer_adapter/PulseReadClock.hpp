// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "../AudioSinkFactory.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <mutex>
#include <optional>
#include <utility>

namespace aa::audio::detail {

// Absolute write endpoint plus Pulse's writable credit tracks the server's
// read position with a constant origin. The interpolated GstAudioClock can
// keep advancing during a pause and take seconds to settle after it.
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
        rate_reference_.reset();
        skew_ppm_ = 0.0;
    }

    void submitted(std::int64_t end) {
        std::lock_guard lock(mutex_);
        write_end_bytes_ = end;
    }

    void requested(std::int64_t writable, std::int64_t now) {
        std::lock_guard lock(mutex_);
        if (bytes_per_second_ == 0 || writable <= 0 || !write_end_bytes_
            || *write_end_bytes_ > INT64_MAX - writable) {
            return;
        }
        const auto bytes = *write_end_bytes_ + writable;
        const auto read_time = bytes / bytes_per_second_ * 1'000'000'000
            + bytes % bytes_per_second_ * 1'000'000'000 / bytes_per_second_;
        const auto phase = now - read_time;
        const auto elapsed = now - std::exchange(last_request_, now);
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
        if (!mapped_phase_) { mapped_phase_ = reference; }
        // Keep the router's wall-clock mapping on the read clock as the PCM
        // rate follows it. Short gaps accumulate against this applied phase.
        const auto limit = std::min<std::int64_t>(elapsed, 100'000'000) / 2'000;
        const auto drift = std::clamp(reference - *mapped_phase_, -limit, limit);
        *mapped_phase_ += drift;
        drift_ += drift;
    }

    [[nodiscard]] SinkTimingAdjustment take_adjustment() {
        std::lock_guard lock(mutex_);
        return {core::Nanoseconds{std::exchange(held_, 0)}, {}, core::Nanoseconds{std::exchange(drift_, 0)}};
    }

    [[nodiscard]] bool is_playing(std::int64_t now) const {
        std::lock_guard lock(mutex_);
        // A step confirmed between writes has not yet reached the router.
        // Leave the sink stamps contiguous until it has moved the schedule.
        return mapped_phase_ && !candidate_ && held_ == 0 && now - last_request_ < 85'000'000;
    }

    [[nodiscard]] bool ready() const {
        std::lock_guard lock(mutex_);
        return mapped_phase_.has_value();
    }

    [[nodiscard]] double skew_ppm() const {
        std::lock_guard lock(mutex_);
        return skew_ppm_;
    }

private:
    struct Candidate {
        int direction;
        std::int64_t since;
        std::int64_t read_start;
        std::array<std::int64_t, 3> recent;
        std::size_t count{1};
    };

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
    std::optional<std::pair<std::int64_t, std::int64_t>> rate_reference_{};
    double skew_ppm_{};
};

} // namespace aa::audio::detail
