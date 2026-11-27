// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Bounded wireless audio-startup wait loop. Keeps the session control bus and
// the supervisor-owned ManagedVideoBroker responsive while AudioRenderWorker
// opens its audio route on its background thread, preventing slow GStreamer
// route readiness from consuming the consumer's 2 s AAL1 handshake budget.

#include "ManagedVideo.hpp"

#include <aa/audio/AudioRenderWorker.hpp>
#include <aa/core/Result.hpp>
#include <aa/core/Time.hpp>
#include <aa/ipc/ControlService.hpp>
#include <aa/ipc/VideoChannel.hpp>
#include <aa/trust/Store.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <functional>
#include <optional>

namespace aa::wireless_receiver {

struct WirelessAudioStartupResult final {
    std::optional<core::Result<void>> opened{};
    std::optional<Error> bus_error{};
    bool stopped{false};
};

inline WirelessAudioStartupResult wait_for_wireless_audio_ready(
    audio::AudioRenderWorker& audio_sink,
    const std::function<core::Result<void>(core::Milliseconds)>& bus_cycle,
    const std::function<ipc::StateSnapshot()>& bus_state,
    ipc::VideoServer& video,
    ManagedVideoBroker* managed_video,
    const std::function<bool(ipc::StateSnapshot, const std::optional<trust::TransportIdentity>&)>&
        keep_running,
    std::chrono::steady_clock::time_point deadline,
    const std::atomic<bool>& running_flag) {
    for (;;) {
        if (auto ready = audio_sink.wait_ready_for(std::chrono::milliseconds{0})) {
            return {std::move(ready), std::nullopt, false};
        }
        if (bus_cycle) {
            if (auto step = bus_cycle(core::Milliseconds{0}); !step) {
                return {std::nullopt, step.error(), true};
            }
        }
        if (!running_flag.load() || std::chrono::steady_clock::now() >= deadline) {
            return {std::nullopt, std::nullopt, true};
        }
        if (keep_running && bus_state) {
            if (!keep_running(bus_state(), std::nullopt)) {
                return {std::nullopt, std::nullopt, true};
            }
        }
        if (managed_video != nullptr) {
            (void)managed_video->poll(video);
        }
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            return {std::nullopt, std::nullopt, true};
        }
        const auto slice_cap =
            (managed_video != nullptr) ? std::chrono::milliseconds{5}
                                       : std::chrono::milliseconds{10};
        auto slice = slice_cap;
        if (deadline != std::chrono::steady_clock::time_point::max()) {
            const auto remaining =
                std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
            slice = std::clamp(remaining, std::chrono::milliseconds{1}, slice_cap);
        }
        if (auto ready = audio_sink.wait_ready_for(slice)) {
            return {std::move(ready), std::nullopt, false};
        }
    }
}

} // namespace aa::wireless_receiver
