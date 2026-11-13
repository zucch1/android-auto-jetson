// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "AvCreditLedger.hpp"
#include <string>

namespace aa::wired_receiver {
inline const char* av_credit_name(AvCreditEvent event) noexcept {
    switch (event) {
    case AvCreditEvent::arrival: return "arrival";
    case AvCreditEvent::accepted: return "accepted";
    case AvCreditEvent::rejected: return "rejected";
    case AvCreditEvent::reset: return "reset";
    case AvCreditEvent::ready: return "ready";
    case AvCreditEvent::ack: return "ack";
    case AvCreditEvent::deferred: return "deferred";
    case AvCreditEvent::granted: return "granted";
    case AvCreditEvent::taken: return "taken";
    case AvCreditEvent::receive_entry: return "receive_entry";
    case AvCreditEvent::receive_return: return "receive_return";
    case AvCreditEvent::count: return "count";
    }
    return "invalid";
}
inline std::string av_delivery_json(const channels::AvDeliveryId& value) {
    return "{\"role\":" + std::to_string(static_cast<unsigned>(value.role)) +
        ",\"generation\":" + std::to_string(value.generation) +
        ",\"delivery_sequence\":" + std::to_string(value.sequence) +
        ",\"session_id\":" + std::to_string(value.session_id) + "}";
}
inline std::string av_observation_json(const AvObservation& value) {
    return "{\"event\":\"" + std::string{av_credit_name(value.event)} + "\",\"at_ns\":" +
        std::to_string(value.at_ns) + ",\"delivery\":" + av_delivery_json(value.delivery) +
        ",\"session_active\":" + (value.session_active ? "true" : "false") +
        ",\"message_sequence\":" + std::to_string(value.message_sequence) +
        ",\"message_id\":" + std::to_string(value.message_id) +
        ",\"bytes\":" + std::to_string(value.bytes) + ",\"ack_for\":" + std::to_string(value.ack_for) +
        ",\"count\":" + std::to_string(value.count) + ",\"error_code\":" +
        (value.error ? std::to_string(static_cast<unsigned>(*value.error)) : "null") +
        ",\"pending\":" + (value.pending ? av_delivery_json(*value.pending) : "null") + "}";
}
inline std::string av_credit_state_json(const AvCreditState& state) {
    std::string line = "{\"pending_available\":" + std::string{state.pending_available ? "true" : "false"} +
        ",\"pending_deferred\":" + (state.pending_deferred ? "true" : "false") + ",\"roles\":[";
    for (std::size_t i = 0; i < state.roles.size(); ++i) {
        const auto& role = state.roles[i];
        if (i != 0) { line += ','; }
        line += "{\"slots\":" + std::to_string(role.slots) + ",\"bytes\":" + std::to_string(role.bytes) +
            ",\"units\":" + std::to_string(role.units) + ",\"credits\":" + std::to_string(role.credits) +
            ",\"deferred\":" + std::to_string(role.deferred) + ",\"completions\":" + std::to_string(role.completions) +
            ",\"slot_capacity\":" + std::to_string(role.slot_capacity) + ",\"byte_capacity\":" + std::to_string(role.byte_capacity) +
            ",\"unit_capacity\":" + std::to_string(role.unit_capacity) + ",\"next_envelope_bytes\":" + std::to_string(role.next_envelope_bytes) + "}";
    }
    return line + "]}";
}
inline std::string av_totals_json(const AvGenerationTotals& value) {
    std::string line = "{\"generation\":" + std::to_string(value.generation) + ",\"events\":{";
    for (std::size_t i = 0; i < value.events.size(); ++i) {
        if (i != 0) { line += ','; }
        line += "\"" + std::string{av_credit_name(static_cast<AvCreditEvent>(i))} + "\":" + std::to_string(value.events[i]);
    }
    line += "},\"arrivals\":[";
    for (std::size_t i = 0; i < value.arrivals.size(); ++i) {
        if (i != 0) { line += ','; }
        line += std::to_string(value.arrivals[i]);
    }
    line += "],\"accepted\":[";
    for (std::size_t i = 0; i < value.accepted.size(); ++i) {
        if (i != 0) { line += ','; }
        line += std::to_string(value.accepted[i]);
    }
    return line + "],\"data_acks\":" + std::to_string(value.data_acks) +
        ",\"config_acks\":" + std::to_string(value.config_acks) + ",\"failed_acks\":" + std::to_string(value.failed_acks) +
        ",\"ready_max_unacked\":" + std::to_string(value.ready_max_unacked) +
        ",\"last_data\":" + av_observation_json(value.last_data) +
        ",\"last_ready\":" + av_observation_json(value.last_ready) + "}";
}
inline std::string av_credit_json(const AvCreditSnapshot& value) {
    std::string line = "{\"capacity\":64,\"total_events\":" + std::to_string(value.total_events) +
        ",\"message_counter_order\":[32768,1,32769,32770,0],\"role_order\":[1,2,3],\"generations\":[";
    for (std::size_t i = 0; i < value.generations.size(); ++i) {
        if (i != 0) { line += ','; }
        line += av_totals_json(value.generations[i]);
    }
    line += "],\"previous_generations\":[";
    for (std::size_t i = 0; i < value.previous_generations.size(); ++i) {
        if (i != 0) { line += ','; }
        line += av_totals_json(value.previous_generations[i]);
    }
    line += "],\"events\":[";
    for (std::size_t i = 0; i < value.size; ++i) {
        if (i != 0) { line += ','; }
        line += "{\"ordinal\":" + std::to_string(value.events[i].ordinal) +
            ",\"sampled_ns\":" + std::to_string(value.events[i].sampled_ns) + ",\"av\":" +
            av_observation_json(value.events[i].observation) + ",\"worker\":" + av_credit_state_json(value.events[i].worker) + "}";
    }
    return line + "]}";
}
} // namespace aa::wired_receiver
