// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <aa/input/InputService.hpp>
#include <aa/input/SensorService.hpp>

#include <vector>

namespace inputsensorstest {

// The phone fixture (todo 26 QA): records normalized input and sensor
// indications exactly as they arrive on the todo-18 channel seams. In-memory
// target only - the production target is the session's wire adapter.
struct PhoneFixture final : aa::input::InputChannelTarget, aa::input::SensorChannelTarget {
    std::vector<aa::input::NormalizedInput> input{};
    std::vector<aa::input::SensorEvent> sensors{};

    aa::core::Result<void> on_normalized(const aa::input::NormalizedInput& event) override {
        input.push_back(event);
        return {};
    }

    void on_sensor_event(const aa::input::SensorEvent& event) override {
        sensors.push_back(event);
    }
};

} // namespace inputsensorstest
