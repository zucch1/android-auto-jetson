// SPDX-License-Identifier: GPL-3.0-or-later
#include <aa/input/SensorService.hpp>

namespace aa::input {

SensorResponse SensorService::submit_request(const SensorRequest& request) const noexcept {
    if (request.channel == SensorChannel::night_mode
        && descriptor_.night_mode() == NightModePolicy::manual) {
        return SensorResponse{SensorRequestStatus::accepted};
    }
    // No vehicle-sensor provider is installed (the production provider set is
    // empty), so NO vehicle channel can ever deliver a reading. Acceptance
    // requires an implemented provider; without one the only honest answer is
    // the typed unsupported response - never an accepted subscription that
    // can never fire.
    return SensorResponse{SensorRequestStatus::unsupported};
}

void SensorService::set_night_mode(bool night) {
    night_ = night;
    channel_.on_sensor_event(
        SensorEvent{SensorChannel::night_mode, night ? 1 : 0, SensorOrigin::manual});
    ++emitted_;
}

} // namespace aa::input
