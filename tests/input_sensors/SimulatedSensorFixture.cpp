// SPDX-License-Identifier: GPL-3.0-or-later
// TEST-ONLY BUILD ARTIFACT: linked only into test binaries (see the header).
#include "SimulatedSensorFixture.hpp"

#include <algorithm>

namespace inputsensorstest {

void SimulatedSensorFixture::enable(std::span<const aa::input::SensorChannel> channels) {
    for (const aa::input::SensorChannel channel : channels) {
        if (std::find(enabled_.begin(), enabled_.end(), channel) == enabled_.end()) {
            enabled_.push_back(channel);
        }
    }
}

aa::core::Result<void> SimulatedSensorFixture::emit(const SimulatedSensorEvent& event) {
    if (event.channel == aa::input::SensorChannel::night_mode) {
        ++refused_;
        return aa::Error{aa::ErrorCode::invalid_argument};
    }
    const bool served = std::find(enabled_.begin(), enabled_.end(), event.channel) != enabled_.end();
    if (!served) {
        ++refused_;
        return aa::Error{aa::ErrorCode::channel_unsupported_capability};
    }
    channel_.on_sensor_event(aa::input::SensorEvent{
        event.channel, event.value, aa::input::SensorOrigin::simulated});
    ++emitted_;
    return {};
}

bool SimulatedSensorFixture::enabled(aa::input::SensorChannel channel) const noexcept {
    return std::find(enabled_.begin(), enabled_.end(), channel) != enabled_.end();
}

} // namespace inputsensorstest
