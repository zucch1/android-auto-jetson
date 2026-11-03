// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>

namespace aa::consumer {

struct DecodeCompletion final {
    std::uint64_t session_ns{};
    std::uint64_t frame{};
    std::uint64_t sequence{};
    std::uint64_t timestamp_ns{};
    std::uint64_t submitted_ns{};
    std::uint64_t completed_ns{};
    // Diagnostic steady_clock ns. Arrival and successful push return are parallel branches:
    // neither is required to precede the other. Missing evidence stays null.
    std::optional<std::uint64_t> push_returned_ns{};
    std::optional<std::uint64_t> sink_pad_observed_arrival_ns{};
    std::optional<std::uint64_t> pull_returned_ns{};
    std::optional<std::uint64_t> map_copy_started_ns{};
};

using DecodeCompletionObserver = std::function<void(const DecodeCompletion&)>;

inline std::string decode_completion_json(const DecodeCompletion& event) {
    const auto optional_ns = [](const std::optional<std::uint64_t>& value) {
        return value ? std::to_string(*value) : std::string{"null"};
    };
    return "{\"event\":\"video_decode_completed\",\"session_ns\":" +
        std::to_string(event.session_ns) + ",\"frame\":" + std::to_string(event.frame) +
        ",\"sequence\":" + std::to_string(event.sequence) + ",\"timestamp_ns\":" +
        std::to_string(event.timestamp_ns) + ",\"submitted_ns\":" +
        std::to_string(event.submitted_ns) + ",\"completed_ns\":" +
        std::to_string(event.completed_ns) + ",\"push_returned_ns\":" +
        optional_ns(event.push_returned_ns) + ",\"sink_pad_observed_arrival_ns\":" +
        optional_ns(event.sink_pad_observed_arrival_ns) + ",\"pull_returned_ns\":" +
        optional_ns(event.pull_returned_ns) + ",\"map_copy_started_ns\":" +
        optional_ns(event.map_copy_started_ns) + "}";
}

} // namespace aa::consumer
