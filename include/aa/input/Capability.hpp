// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <aa/protocol/Protocol.hpp>

#include <cstdint>
#include <string_view>
#include <utility>
#include <vector>

namespace aa::input {

// Capability-gated input and sensor services (plan todo 26). This descriptor
// is the SINGLE SOURCE OF TRUTH for what the receiver advertises on the
// input/sensor surface: bench input only (the plan's bench input set -
// keyboard/D-pad and mouse-to-touch), NO vehicle sensors by default, and
// night mode as a manual toggle. Tests assert it EXACTLY; nothing may add a
// capability silently.
//
// Immutability (gate r2): the descriptor is immutable AFTER CONSTRUCTION -
// there is no mutator, and the vehicle-sensor list can only be supplied at
// construction (empty in the shipped bench defaults). Services own a copy of
// their descriptor (never a borrow), so no caller-supplied lifetime matters.
//
// Guardrails (plan): no fabricated vehicle state and no physical touch
// qualification claim. `mouse_to_touch` is synthesized touch from the bench
// mouse - it is NOT a claim of a physical touch surface, and no source below
// ever represents physical touch.

// The bench input set. Physical touch is deliberately absent: mouse-to-touch
// is the only touch-shaped source and is synthesized, never qualified.
enum class InputSource : std::uint8_t { keyboard_dpad, mouse_to_touch };

// Vehicle-sensor channels on the AASDK sensor-source seam (the SensorType
// vocabulary, project-owned). The bench descriptor advertises NONE of these:
// no-default vehicle sensors. Requests for a channel outside the advertised
// set are answered with the typed unsupported response.
enum class SensorChannel : std::uint8_t {
    location,
    compass,
    speed,
    rpm,
    odometer,
    fuel,
    parking_brake,
    gear,
    obdii_diagnostic_code,
    night_mode,
    environment_data,
    hvac_data,
    driving_status_data,
    dead_reckoning_data,
    passenger_data,
    door_data,
    light_data,
    tire_pressure_data,
    accelerometer_data,
    gyroscope_data,
    gps_satellite_data,
    toll_card,
};

// Night-mode handling: a manual toggle only. No ambient or light sensor is
// ever consulted or fabricated.
enum class NightModePolicy : std::uint8_t { manual };

// Mouse-to-touch coordinate space: normalized coordinates are pixels clamped
// into [0, width-1] x [0, height-1] (the bench projection viewport).
struct TouchViewport final {
    std::uint32_t width{};
    std::uint32_t height{};

    [[nodiscard]] friend constexpr bool operator==(const TouchViewport&,
                                                   const TouchViewport&) noexcept = default;
};

// Immutable-after-construction capability descriptor. The bench defaults
// (bench_defaults()) are what the receiver ships; tests assert them EXACTLY.
class CapabilityDescriptor final {
public:
    CapabilityDescriptor(std::vector<InputSource> sources,
                         std::vector<std::int32_t> keycodes,
                         TouchViewport touch_viewport,
                         std::vector<SensorChannel> vehicle_sensors,
                         NightModePolicy night_mode)
        : sources_(std::move(sources)), keycodes_(std::move(keycodes)),
          touch_viewport_(touch_viewport), vehicle_sensors_(std::move(vehicle_sensors)),
          night_mode_(night_mode) {}

    [[nodiscard]] static CapabilityDescriptor bench_defaults();

    // Advertised bench input. The bench descriptor is EXACTLY
    // {keyboard_dpad, mouse_to_touch} - no physical touch source exists.
    [[nodiscard]] const std::vector<InputSource>& sources() const noexcept { return sources_; }
    // Advertised keyboard/D-pad keycodes (Android keycode space). Only these
    // are forwarded; anything else is rejected at the capability gate.
    [[nodiscard]] const std::vector<std::int32_t>& keycodes() const noexcept { return keycodes_; }
    // Mouse-to-touch normalization target.
    [[nodiscard]] const TouchViewport& touch_viewport() const noexcept { return touch_viewport_; }
    // Advertised vehicle sensors: EMPTY in the bench defaults. The list is
    // fixed at construction and has no mutator; the shipped descriptor is
    // asserted exactly, so a silent addition cannot hide here.
    [[nodiscard]] const std::vector<SensorChannel>& vehicle_sensors() const noexcept {
        return vehicle_sensors_;
    }
    [[nodiscard]] NightModePolicy night_mode() const noexcept { return night_mode_; }

    [[nodiscard]] friend bool operator==(const CapabilityDescriptor&,
                                         const CapabilityDescriptor&) = default;

private:
    std::vector<InputSource> sources_;
    std::vector<std::int32_t> keycodes_;
    TouchViewport touch_viewport_;
    std::vector<SensorChannel> vehicle_sensors_;
    NightModePolicy night_mode_;
};

[[nodiscard]] constexpr std::string_view to_string(InputSource source) noexcept {
    switch (source) {
    case InputSource::keyboard_dpad: return "keyboard_dpad";
    case InputSource::mouse_to_touch: return "mouse_to_touch";
    }
    return "unknown";
}

[[nodiscard]] constexpr std::string_view to_string(NightModePolicy policy) noexcept {
    switch (policy) {
    case NightModePolicy::manual: return "manual";
    }
    return "unknown";
}

} // namespace aa::input
