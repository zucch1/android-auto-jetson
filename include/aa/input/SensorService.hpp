// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <aa/core/Result.hpp>
#include <aa/input/Capability.hpp>

#include <cstdint>
#include <utility>
#include <vector>

namespace aa::input {

// Capability-gated sensor service (todo 26): NO-DEFAULT vehicle sensors.
//
// Structural isolation (gate r2): this production service is PHYSICALLY
// INCAPABLE of emitting simulated readings. Fixture enablement and synthetic
// emission do not exist in the production library or the production headers -
// they are a test-only build artifact under tests/input_sensors/, linked only
// into test binaries. The only sensor emission the production
// service can perform is the manual night-mode indication, which is produced
// exclusively by the manual toggle (gate r3: generic simulated emission can
// never impersonate night mode).
//
// Where a sensor indication came from. Nothing downstream may present a
// `simulated` value as vehicle state; `manual` values are explicit operator
// toggles, never sensor readings.
enum class SensorOrigin : std::uint8_t { manual, simulated };

// The AASDK sensor-source seam's request shape (project-owned).
struct SensorRequest final {
    SensorChannel channel{};
    std::uint64_t min_update_period_ms{};

    [[nodiscard]] friend constexpr bool operator==(const SensorRequest&,
                                                   const SensorRequest&) noexcept = default;
};

enum class SensorRequestStatus : std::uint8_t { accepted, unsupported };

// The typed request response. `unsupported` is the typed answer to every
// unknown sensor request; `accepted` is answered only for channels the
// capability descriptor actually serves (its vehicle-sensor advertisement,
// and the manual night-mode channel when the policy is manual).
struct SensorResponse final {
    SensorRequestStatus status{SensorRequestStatus::unsupported};

    [[nodiscard]] friend constexpr bool operator==(const SensorResponse&,
                                                   const SensorResponse&) noexcept = default;
};

// A sensor indication forwarded toward the session/phone fixture.
struct SensorEvent final {
    SensorChannel channel{};
    std::int64_t value{};  // 0|1 for manual night mode; simulated values are
                           // produced only by the test-only fixture
    SensorOrigin origin{};

    [[nodiscard]] friend constexpr bool operator==(const SensorEvent&,
                                                   const SensorEvent&) noexcept = default;
};

// The sensor-channel seam: indications flow here toward the session/phone
// fixture. Non-owning; must outlive the service.
class SensorChannelTarget {
public:
    virtual ~SensorChannelTarget() = default;
    SensorChannelTarget() = default;
    SensorChannelTarget(const SensorChannelTarget&) = delete;
    SensorChannelTarget& operator=(const SensorChannelTarget&) = delete;
    SensorChannelTarget(SensorChannelTarget&&) = delete;
    SensorChannelTarget& operator=(SensorChannelTarget&&) = delete;

    virtual void on_sensor_event(const SensorEvent& event) = 0;
};

// The sensor service. Caller-driven and thread-free. The service OWNS an
// immutable copy of its capability descriptor (gate r2 F1: taken BY VALUE and
// moved into the owned member - no reference ever binds a caller's
// descriptor, so passing the bench_defaults() temporary is safe by
// construction).
class SensorService final {
public:
    SensorService(CapabilityDescriptor descriptor, SensorChannelTarget& channel)
        : descriptor_(std::move(descriptor)), channel_(channel) {}

    // Request path (the AASDK SensorRequest -> SensorResponse seam). Accepted
    // ONLY for the manual night-mode channel under the manual policy - night
    // mode is a manual toggle, not a provider. Every other channel answers
    // with the typed unsupported response: acceptance requires an implemented
    // vehicle-sensor provider, and the installed provider set is empty. An
    // advertised vehicle-sensor channel is therefore NOT served until a real
    // provider exists behind it - advertising alone never makes a channel
    // deliverable (the bench descriptor advertises no vehicle sensors at all,
    // so every vehicle-sensor request is unsupported).
    [[nodiscard]] SensorResponse submit_request(const SensorRequest& request) const noexcept;

    // Manual night-mode toggle (the plan's night-mode handling: an explicit
    // operator action, never ambient-sensor-driven) and the ONLY path for
    // night-mode indications. The manual state always reflects the toggle and
    // the resulting SensorEvent{night_mode, 0|1, origin=manual} is forwarded
    // to the sensor seam.
    void set_night_mode(bool night);
    [[nodiscard]] bool night_mode() const noexcept { return night_; }

    [[nodiscard]] const CapabilityDescriptor& descriptor() const noexcept { return descriptor_; }
    [[nodiscard]] std::uint64_t emitted() const noexcept { return emitted_; }

private:
    const CapabilityDescriptor descriptor_;
    SensorChannelTarget& channel_;
    bool night_{false};
    std::uint64_t emitted_{0};
};

} // namespace aa::input
