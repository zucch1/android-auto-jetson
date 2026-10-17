// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <aa/input/SensorService.hpp>
#include <aa/protocol/Protocol.hpp>

#include <functional>
#include <optional>
#include <vector>

namespace aa::channels {

// Session-thread-only. The send callback uses the owning session's encrypted
// channel send path; the policy service is the sole authority for manual state.
class SensorSink final : public protocol::MessageSink, private input::SensorChannelTarget {
public:
    using Send = std::function<core::Result<void>(std::vector<std::byte>)>;
    SensorSink(input::CapabilityDescriptor descriptor, Send send)
        : send_(std::move(send)), service_(std::move(descriptor), *this) {}

    [[nodiscard]] core::Result<void> on_message(const protocol::MessageView& message) override;
    [[nodiscard]] core::Result<void> set_night_mode(bool night);
    [[nodiscard]] std::uint64_t unhandled() const noexcept { return unhandled_; }

private:
    void on_sensor_event(const input::SensorEvent& event) override;
    Send send_;
    input::SensorService service_;
    std::optional<Error> event_error_;
    std::uint64_t unhandled_{0};
};

} // namespace aa::channels
