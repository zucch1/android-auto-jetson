// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <functional>
#include <string>

namespace aa::consumer {

struct DecodeCompletion final {
    std::uint64_t session_ns{};
    std::uint64_t frame{};
    std::uint64_t sequence{};
    std::uint64_t timestamp_ns{};
    std::uint64_t submitted_ns{};
    std::uint64_t completed_ns{};
};

using DecodeCompletionObserver = std::function<void(const DecodeCompletion&)>;

inline std::string decode_completion_json(const DecodeCompletion& event) {
    return "{\"event\":\"video_decode_completed\",\"session_ns\":" +
        std::to_string(event.session_ns) + ",\"frame\":" + std::to_string(event.frame) +
        ",\"sequence\":" + std::to_string(event.sequence) + ",\"timestamp_ns\":" +
        std::to_string(event.timestamp_ns) + ",\"submitted_ns\":" +
        std::to_string(event.submitted_ns) + ",\"completed_ns\":" +
        std::to_string(event.completed_ns) + "}";
}

} // namespace aa::consumer
