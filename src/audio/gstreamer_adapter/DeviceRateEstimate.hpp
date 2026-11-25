// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <cstdint>
#include <limits>
#include <optional>
#include <utility>

namespace aa::audio::detail {

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
