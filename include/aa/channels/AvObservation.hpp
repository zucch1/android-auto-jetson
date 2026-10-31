// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <aa/protocol/Messages.hpp>
#include <aa/core/Error.hpp>

#include <chrono>
#include <optional>

namespace aa::channels {
struct AvDeliveryId final {
    protocol::ChannelRole role;
    std::uint64_t generation;
    std::uint64_t sequence;
    std::int32_t session_id;
    [[nodiscard]] friend bool operator==(const AvDeliveryId&, const AvDeliveryId&) = default;
};

enum class AvCreditEvent : std::uint8_t {
    arrival, accepted, rejected, reset, ready, ack, deferred, granted, taken,
    receive_entry, receive_return, count
};

struct AvObservation final {
    AvCreditEvent event{};
    AvDeliveryId delivery{};
    bool session_active{};
    std::uint64_t message_sequence{};
    std::uint16_t message_id{};
    std::uint16_t ack_for{};
    std::size_t bytes{};
    std::optional<AvDeliveryId> pending;
    std::optional<ErrorCode> error;
    std::uint32_t count{};
    std::int64_t at_ns{};
    [[nodiscard]] friend bool operator==(const AvObservation&, const AvObservation&) = default;
};

inline std::int64_t av_observation_now() noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
} // namespace aa::channels
