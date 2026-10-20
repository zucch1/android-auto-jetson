// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <string>

namespace aa::ipc {

// Side-band diagnostics only. None of these fields changes the video wire.
// timestamp_ns is an opaque correlation key, NEVER the IPC latency start.
enum class VideoEndpointKind { send_started, receive_completed };

struct VideoEndpoint final {
    VideoEndpointKind kind{};
    std::uint32_t producer_pid{};
    std::uint32_t consumer_pid{};
    std::uint64_t connection_ns{}; // local connection attempt/accept epoch
    std::uint64_t frame{};
    std::uint64_t sequence{}; // FIRST fragment, on both sides
    std::uint64_t timestamp_ns{};
    std::uint64_t observed_ns{}; // steady_clock; actual endpoint, not AV schedule
    std::uint64_t payload_bytes{};
};

using VideoEndpointObserver = std::function<void(const VideoEndpoint&)>;

inline std::uint64_t video_monotonic_ns() noexcept {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

// Numeric-only schema, shared by the two executable log adapters.
inline std::string video_endpoint_json(const VideoEndpoint& event) {
    const char* name = nullptr;
    switch (event.kind) {
    case VideoEndpointKind::send_started: name = "video_ipc_send"; break;
    case VideoEndpointKind::receive_completed: name = "video_ipc_receive"; break;
    }
    return std::string{"{\"event\":\""} + name + "\",\"producer_pid\":" +
        std::to_string(event.producer_pid) + ",\"consumer_pid\":" +
        std::to_string(event.consumer_pid) + ",\"connection_ns\":" +
        std::to_string(event.connection_ns) + ",\"frame\":" + std::to_string(event.frame) +
        ",\"sequence\":" + std::to_string(event.sequence) + ",\"timestamp_ns\":" +
        std::to_string(event.timestamp_ns) + ",\"observed_ns\":" +
        std::to_string(event.observed_ns) + ",\"payload_bytes\":" +
        std::to_string(event.payload_bytes) + "}";
}

} // namespace aa::ipc
