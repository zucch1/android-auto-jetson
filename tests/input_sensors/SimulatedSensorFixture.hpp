// SPDX-License-Identifier: GPL-3.0-or-later
// TEST-ONLY BUILD ARTIFACT (gate r2): this header and its .cpp are compiled
// into aa_input_sensor_fixtures, which is linked ONLY into test binaries. The
// production library (aa_input_service) and the production headers
// (include/aa/input/) physically contain no fixture enablement and no
// synthetic emission - a production consumer cannot reach anything declared
// here (regression: tests/input_sensors/production_isolation_test.py).
#pragma once

#include <aa/core/Result.hpp>
#include <aa/input/Capability.hpp>
#include <aa/input/SensorService.hpp>

#include <cstdint>
#include <span>
#include <vector>

namespace inputsensorstest {

// A synthetic fixture value. Reaches the seam labelled SensorOrigin::simulated
// and is never presented as vehicle state.
struct SimulatedSensorEvent final {
    aa::input::SensorChannel channel{};
    std::int64_t value{};

    [[nodiscard]] friend constexpr bool operator==(const SimulatedSensorEvent&,
                                                   const SimulatedSensorEvent&) noexcept = default;
};

// Test-only simulated sensor source: explicitly enableable per channel, then
// able to emit synthetic values through the sensor-channel seam. night_mode
// is TYPED-REJECTED on every generic emission (gate r3): night-mode
// indications come exclusively from the production manual toggle.
class SimulatedSensorFixture final {
public:
    explicit SimulatedSensorFixture(aa::input::SensorChannelTarget& channel)
        : channel_(channel) {}
    SimulatedSensorFixture(const SimulatedSensorFixture&) = delete;
    SimulatedSensorFixture& operator=(const SimulatedSensorFixture&) = delete;

    // Explicit enablement of the simulated emission path per channel.
    void enable(std::span<const aa::input::SensorChannel> channels);

    // Synthetic emission for an explicitly enabled channel. Error contract:
    // night_mode -> invalid_argument ALWAYS (manual-only, even after
    // enablement); a channel whose emission path is not enabled ->
    // channel_unsupported_capability; both count in refused() and forward
    // nothing.
    [[nodiscard]] aa::core::Result<void> emit(const SimulatedSensorEvent& event);

    [[nodiscard]] bool enabled(aa::input::SensorChannel channel) const noexcept;
    [[nodiscard]] std::uint64_t emitted() const noexcept { return emitted_; }
    [[nodiscard]] std::uint64_t refused() const noexcept { return refused_; }

private:
    aa::input::SensorChannelTarget& channel_;
    std::vector<aa::input::SensorChannel> enabled_{};
    std::uint64_t emitted_{0};
    std::uint64_t refused_{0};
};

} // namespace inputsensorstest
