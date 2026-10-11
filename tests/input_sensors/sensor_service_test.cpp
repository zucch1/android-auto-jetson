// SPDX-License-Identifier: GPL-3.0-or-later
// Task 26 sensor service: NO-DEFAULT vehicle sensors. The typed unsupported
// response answers every request outside the capability descriptor; the ONLY
// production emission is the manual night-mode toggle; simulated readings are
// reachable exclusively through the TEST-ONLY fixture (structurally absent
// from the production library) and can never impersonate night mode.
#include "phone_fixture.hpp"
#include "SimulatedSensorFixture.hpp"

#include <aa/input/SensorService.hpp>

#include <array>
#include <vector>

#include <gtest/gtest.h>

namespace {
using namespace aa::input;

using inputsensorstest::PhoneFixture;
using inputsensorstest::SimulatedSensorEvent;
using inputsensorstest::SimulatedSensorFixture;

TEST(InputSensorsSensor, UnknownSensorRequestReceivesTypedUnsupportedResponse) {
    // Given: the default sensor service - the bench descriptor advertises no
    // vehicle sensors.
    PhoneFixture phone;
    SensorService service(CapabilityDescriptor::bench_defaults(), phone);

    // When: unknown vehicle-sensor requests arrive.
    const auto speed = service.submit_request(SensorRequest{SensorChannel::speed, 100});
    const auto location = service.submit_request(SensorRequest{SensorChannel::location, 1000});
    const auto rpm = service.submit_request(SensorRequest{SensorChannel::rpm, 100});

    // Then: each receives the typed unsupported response.
    EXPECT_EQ(speed, (SensorResponse{SensorRequestStatus::unsupported}));
    EXPECT_EQ(location, (SensorResponse{SensorRequestStatus::unsupported}));
    EXPECT_EQ(rpm, (SensorResponse{SensorRequestStatus::unsupported}));
    EXPECT_TRUE(phone.sensors.empty());
}

TEST(InputSensorsSensor, AdvertisedVehicleSensorsAreServedOthersAreNot) {
    // Given: a descriptor whose construction-time advertisement names speed.
    PhoneFixture phone;
    const CapabilityDescriptor advertised{
        {InputSource::keyboard_dpad},
        {19},
        TouchViewport{1280, 720},
        {SensorChannel::speed},
        NightModePolicy::manual,
    };
    SensorService service(advertised, phone);

    // When: requests name the advertised and an unadvertised channel.
    const auto served = service.submit_request(SensorRequest{SensorChannel::speed, 100});
    const auto missing = service.submit_request(SensorRequest{SensorChannel::rpm, 100});

    // Then: the capability descriptor decides - accepted for the advertised
    // channel, typed unsupported for the unknown one.
    EXPECT_EQ(served, (SensorResponse{SensorRequestStatus::accepted}));
    EXPECT_EQ(missing, (SensorResponse{SensorRequestStatus::unsupported}));
}

TEST(InputSensorsSensor, NightModeIsAManualToggleOnly) {
    // Given: the default sensor service.
    PhoneFixture phone;
    SensorService service(CapabilityDescriptor::bench_defaults(), phone);

    // When: the phone requests the manual night-mode channel and the operator
    // toggles night mode on and off.
    const auto response = service.submit_request(SensorRequest{SensorChannel::night_mode, 0});
    service.set_night_mode(true);
    service.set_night_mode(false);

    // Then: the channel is served (manual policy) and the phone fixture
    // received manual indications - never simulated, never an ambient/light
    // sensor reading - and the state tracks the toggle.
    EXPECT_EQ(response, (SensorResponse{SensorRequestStatus::accepted}));
    const std::vector<SensorEvent> expected{
        SensorEvent{SensorChannel::night_mode, 1, SensorOrigin::manual},
        SensorEvent{SensorChannel::night_mode, 0, SensorOrigin::manual},
    };
    EXPECT_EQ(phone.sensors, expected);
    EXPECT_FALSE(service.night_mode());
    EXPECT_EQ(service.emitted(), 2U);
}

TEST(InputSensorsSensor, NoSimulatedEventIsEmittedWithoutFixtureEnablement) {
    // Given: the test-only fixture with no channel explicitly enabled.
    PhoneFixture phone;
    SimulatedSensorFixture fixture(phone);

    // When: a synthetic emission is attempted.
    const auto refused = fixture.emit(SimulatedSensorEvent{SensorChannel::speed, 65});

    // Then: the emission path is unreachable without explicit enablement -
    // typed refusal, and the seam sees nothing.
    ASSERT_FALSE(refused);
    EXPECT_EQ(refused.error().code(), aa::ErrorCode::channel_unsupported_capability);
    EXPECT_TRUE(phone.sensors.empty());
    EXPECT_EQ(fixture.emitted(), 0U);
    EXPECT_EQ(fixture.refused(), 1U);
}

TEST(InputSensorsSensor, ExplicitlyEnabledFixtureEmitsSimulatedLabeledEvents) {
    // Given: an explicitly enabled test-only fixture for speed.
    PhoneFixture phone;
    SimulatedSensorFixture fixture(phone);
    const std::array<SensorChannel, 1> channels{SensorChannel::speed};
    fixture.enable(channels);

    // When: a synthetic value is emitted.
    ASSERT_TRUE(fixture.emit(SimulatedSensorEvent{SensorChannel::speed, 65}));

    // Then: the phone fixture received it labelled simulated - synthetic
    // fixture data, never vehicle state.
    ASSERT_EQ(phone.sensors.size(), 1U);
    EXPECT_EQ(phone.sensors.front(),
              (SensorEvent{SensorChannel::speed, 65, SensorOrigin::simulated}));
    EXPECT_EQ(fixture.emitted(), 1U);
    EXPECT_EQ(fixture.refused(), 0U);
}

TEST(InputSensorsSensor, SimulatedEmissionCannotBypassManualNightMode) {
    // Given: a test-only fixture with night_mode explicitly enabled - the
    // most enabling any caller can do to the simulated path.
    PhoneFixture phone;
    SensorService service(CapabilityDescriptor::bench_defaults(), phone);
    SimulatedSensorFixture fixture(phone);
    const std::array<SensorChannel, 2> channels{SensorChannel::night_mode, SensorChannel::speed};
    fixture.enable(channels);

    // When: generic simulated emission tries to impersonate night mode.
    const auto bypass = fixture.emit(SimulatedSensorEvent{SensorChannel::night_mode, 1});

    // Then: the bypass is typed-rejected even AFTER fixture enablement and
    // nothing reaches the seam.
    ASSERT_FALSE(bypass);
    EXPECT_EQ(bypass.error().code(), aa::ErrorCode::invalid_argument);
    EXPECT_TRUE(phone.sensors.empty());

    // And: the manual toggle remains the only path for night-mode indications.
    service.set_night_mode(true);
    ASSERT_EQ(phone.sensors.size(), 1U);
    EXPECT_EQ(phone.sensors.front(),
              (SensorEvent{SensorChannel::night_mode, 1, SensorOrigin::manual}));
}

TEST(InputSensorsSensor, SimulatedNightModeIsRejectedWithoutEnablementToo) {
    // Given: an untouched test-only fixture.
    PhoneFixture phone;
    SimulatedSensorFixture fixture(phone);

    // When: generic simulated emission targets night_mode.
    const auto bypass = fixture.emit(SimulatedSensorEvent{SensorChannel::night_mode, 0});

    // Then: it is invalid for this API regardless of enablement (checked
    // before the enablement gate), and the manual toggle is unaffected.
    ASSERT_FALSE(bypass);
    EXPECT_EQ(bypass.error().code(), aa::ErrorCode::invalid_argument);
    EXPECT_TRUE(phone.sensors.empty());
}

TEST(InputSensorsSensor, NightModeToggleNeverChangesTheCapabilityDescriptor) {
    // Given: a service with toggles applied and fixture traffic alongside.
    PhoneFixture phone;
    SensorService service(CapabilityDescriptor::bench_defaults(), phone);
    SimulatedSensorFixture fixture(phone);
    const std::array<SensorChannel, 2> channels{SensorChannel::speed, SensorChannel::rpm};
    fixture.enable(channels);
    ASSERT_TRUE(fixture.emit(SimulatedSensorEvent{SensorChannel::speed, 65}));
    service.set_night_mode(true);

    // Then: the advertised descriptor is still exactly the bench defaults -
    // neither the toggle nor test fixtures are a silent advertisement.
    EXPECT_EQ(service.descriptor(), CapabilityDescriptor::bench_defaults());
    EXPECT_TRUE(service.descriptor().vehicle_sensors().empty());
}

} // namespace
