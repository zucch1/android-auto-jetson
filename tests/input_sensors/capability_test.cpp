// SPDX-License-Identifier: GPL-3.0-or-later
// Task 26 capability descriptor: the SINGLE SOURCE OF TRUTH for what the
// input/sensor surface advertises. Tests assert it EXACTLY (no silent
// additions) - bench input only, no vehicle sensors, night mode manual.
#include <aa/input/Capability.hpp>
#include <aa/protocol/Messages.hpp>

#include <cstdint>
#include <vector>

#include <gtest/gtest.h>

namespace {
using namespace aa::input;

TEST(InputSensorsCapability, BenchDescriptorMatchesExpectedExactly) {
    // Given: the exact descriptor this receiver advertises (written out
    // literally here - never derived from the implementation under test).
    const std::vector<std::int32_t> bench_keycodes{
        7, 8, 9, 10, 11, 12, 13, 14, 15, 16,
        19, 20, 21, 22,
        29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42,
        43, 44, 45, 46, 47, 48, 49, 50, 51, 52, 53, 54,
        62, 66, 67, 111};
    const CapabilityDescriptor expected{
        {InputSource::keyboard_dpad, InputSource::mouse_to_touch},
        bench_keycodes,
        TouchViewport{1280, 720},
        {},
        NightModePolicy::manual,
    };

    // When: the bench defaults are produced.
    const auto descriptor = CapabilityDescriptor::bench_defaults();

    // Then: the descriptor is exactly the advertised set - nothing more.
    EXPECT_EQ(descriptor, expected);
}

TEST(InputSensorsCapability, BenchInputIsKeyboardDpadAndMouseToTouchOnly) {
    // Given/When: the advertised bench descriptor.
    const auto descriptor = CapabilityDescriptor::bench_defaults();

    // Then: the plan's bench input set is advertised and nothing else - the
    // exact list is the whole of the claim, so a physical touch source (which
    // the InputSource vocabulary deliberately does not even contain) can
    // never be added silently.
    EXPECT_EQ(descriptor.sources(),
              (std::vector<InputSource>{InputSource::keyboard_dpad, InputSource::mouse_to_touch}));
}

TEST(InputSensorsCapability, NoVehicleSensorsAndNightModeIsManual) {
    // Given/When: the advertised bench descriptor.
    const auto descriptor = CapabilityDescriptor::bench_defaults();

    // Then: no vehicle sensor is advertised by default and night mode is a
    // manual toggle policy - never ambient-sensor-driven.
    EXPECT_TRUE(descriptor.vehicle_sensors().empty());
    EXPECT_EQ(descriptor.night_mode(), NightModePolicy::manual);
}

TEST(InputSensorsCapability, TouchViewportIsTheBenchProjectionSpace) {
    // Given/When: the advertised bench descriptor.
    const auto descriptor = CapabilityDescriptor::bench_defaults();

    // Then: mouse-to-touch normalizes into the bench projection viewport and
    // the advertised keycodes fit the channel's qualified wire range.
    EXPECT_EQ(descriptor.touch_viewport(), (TouchViewport{1280, 720}));
    ASSERT_FALSE(descriptor.keycodes().empty());
    EXPECT_LE(descriptor.keycodes().size(), aa::protocol::kMaxButtonKeycodes);
    for (const std::int32_t keycode : descriptor.keycodes()) {
        EXPECT_GT(keycode, 0);
        EXPECT_LE(keycode, 65535);
    }
}

TEST(InputSensorsCapability, VehicleSensorListIsFixedAtConstruction) {
    // Given: a descriptor constructed with a vehicle-sensor advertisement.
    const CapabilityDescriptor descriptor{
        {InputSource::keyboard_dpad},
        {19},
        TouchViewport{1280, 720},
        {SensorChannel::speed},
        NightModePolicy::manual,
    };

    // When: the list is read back through the const accessor (the type has no
    // mutator - immutability after construction is enforced by the class shape).
    const auto& read_back = descriptor.vehicle_sensors();

    // Then: every reader observes exactly the construction-time list.
    EXPECT_EQ(read_back, (std::vector<SensorChannel>{SensorChannel::speed}));
}

} // namespace
