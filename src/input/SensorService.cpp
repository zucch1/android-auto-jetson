// SPDX-License-Identifier: GPL-3.0-or-later
#include <aa/input/SensorService.hpp>

namespace aa::input {

SensorResponse SensorService::submit_request(const SensorRequest& request) const noexcept {
    if (request.channel == SensorChannel::night_mode
        && descriptor_.night_mode() == NightModePolicy::manual) {
        return SensorResponse{SensorRequestStatus::accepted};
    }
    const auto& advertised = descriptor_.vehicle_sensors();
    const bool served =
        std::find(advertised.begin(), advertised.end(), request.channel) != advertised.end();
    return SensorResponse{
        served ? SensorRequestStatus::accepted : SensorRequestStatus::unsupported};
}

void SensorService::set_night_mode(bool night) {
    night_ = night;
    channel_.on_sensor_event(
        SensorEvent{SensorChannel::night_mode, night ? 1 : 0, SensorOrigin::manual});
    ++emitted_;
}

} // namespace aa::input
