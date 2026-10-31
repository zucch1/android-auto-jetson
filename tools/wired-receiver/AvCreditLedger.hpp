// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <aa/channels/AvObservation.hpp>

#include <array>
#include <algorithm>

namespace aa::wired_receiver {
using channels::AvCreditEvent;
using channels::AvObservation;

struct AvReservoir final {
    std::size_t slots{}, bytes{}, units{}, credits{}, deferred{}, completions{};
    std::size_t slot_capacity{}, byte_capacity{}, unit_capacity{}, next_envelope_bytes{};
    [[nodiscard]] friend bool operator==(const AvReservoir&, const AvReservoir&) = default;
};
struct AvCreditState final {
    std::array<AvReservoir, 3> roles{};
    bool pending_available{}, pending_deferred{};
    [[nodiscard]] friend bool operator==(const AvCreditState&, const AvCreditState&) = default;
};
struct AvCreditEntry final {
    std::uint64_t ordinal{};
    std::int64_t sampled_ns{};
    AvObservation observation;
    AvCreditState worker;
    [[nodiscard]] friend bool operator==(const AvCreditEntry&, const AvCreditEntry&) = default;
};
struct AvGenerationTotals final {
    std::uint64_t generation{};
    std::array<std::uint64_t, static_cast<std::size_t>(AvCreditEvent::count)> events{};
    std::array<std::uint64_t, 5> arrivals{}, accepted{};
    std::uint64_t data_acks{}, config_acks{}, failed_acks{};
    std::uint32_t ready_max_unacked{};
    AvObservation last_data{}, last_ready{};
    [[nodiscard]] friend bool operator==(const AvGenerationTotals&, const AvGenerationTotals&) = default;
};
struct AvCreditSnapshot final {
    std::array<AvCreditEntry, 64> events{};
    std::size_t size{};
    std::uint64_t total_events{};
    std::array<AvGenerationTotals, 3> generations{}, previous_generations{};
    [[nodiscard]] friend bool operator==(const AvCreditSnapshot&, const AvCreditSnapshot&) = default;
};

class AvCreditLedger final {
public:
    static std::optional<std::size_t> role_index(protocol::ChannelRole role) noexcept {
        switch (role) {
        case protocol::ChannelRole::media_audio: return 0;
        case protocol::ChannelRole::guidance_audio: return 1;
        case protocol::ChannelRole::system_audio: return 2;
        default: return {};
        }
    }
    void record(AvObservation observation, const AvCreditState& state) noexcept {
        if (frozen_) { return; }
        const auto role = role_index(observation.delivery.role);
        if (!role) { return; }
        if (observation.at_ns == 0) { observation.at_ns = channels::av_observation_now(); }
        auto& totals = live_.generations[*role];
        if (observation.delivery.generation > totals.generation) {
            live_.previous_generations[*role] = totals;
            totals = {};
            totals.generation = observation.delivery.generation;
        }
        if (observation.delivery.generation == totals.generation) { count(totals, observation); }
        live_.events[next_] = {++live_.total_events, channels::av_observation_now(), observation, state};
        next_ = (next_ + 1) % live_.events.size();
        live_.size = std::min(live_.size + 1, live_.events.size());
        if (observation.event == AvCreditEvent::rejected &&
            observation.error == ErrorCode::ipc_queue_full) {
            frozen_ = snapshot();
        }
    }
    [[nodiscard]] AvCreditSnapshot snapshot() const noexcept {
        auto result = live_;
        const auto first = live_.size == live_.events.size() ? next_ : 0;
        for (std::size_t i = 0; i < live_.size; ++i) {
            result.events[i] = live_.events[(first + i) % live_.events.size()];
        }
        return result;
    }
    [[nodiscard]] const std::optional<AvCreditSnapshot>& frozen() const noexcept { return frozen_; }
    [[nodiscard]] AvObservation delivery_observation(channels::AvDeliveryId id) const noexcept {
        if (const auto role = role_index(id.role)) {
            const auto& last = live_.generations[*role].last_data;
            if (last.delivery == id) { return last; }
        }
        AvObservation value{};
        value.delivery = id;
        return value;
    }
private:
    static std::optional<std::size_t> message_index(std::uint16_t id) noexcept {
        switch (id) {
        case 0x8000: return 0;
        case 0x0001: return 1;
        case 0x8001: return 2;
        case 0x8002: return 3;
        case 0x0000: return 4;
        default: return {};
        }
    }
    static void count(AvGenerationTotals& totals, const AvObservation& value) noexcept {
        ++totals.events[static_cast<std::size_t>(value.event)];
        const auto message = message_index(value.message_id);
        switch (value.event) {
        case AvCreditEvent::arrival:
            if (message) { ++totals.arrivals[*message]; }
            break;
        case AvCreditEvent::accepted:
            if (message) { ++totals.accepted[*message]; }
            if (value.message_id == 0) { totals.last_data = value; }
            break;
        case AvCreditEvent::ack:
            if (value.error) { ++totals.failed_acks; }
            else if (value.ack_for == 0) { totals.data_acks += value.count; }
            else if (value.ack_for == 1) { totals.config_acks += value.count; }
            break;
        case AvCreditEvent::ready:
            totals.last_ready = value;
            if (!value.error) { totals.ready_max_unacked = value.count; }
            break;
        case AvCreditEvent::rejected: case AvCreditEvent::reset:
        case AvCreditEvent::deferred: case AvCreditEvent::granted: case AvCreditEvent::taken:
        case AvCreditEvent::receive_entry: case AvCreditEvent::receive_return: case AvCreditEvent::count:
            break;
        }
    }
    AvCreditSnapshot live_{};
    std::size_t next_{};
    std::optional<AvCreditSnapshot> frozen_;
};
} // namespace aa::wired_receiver
