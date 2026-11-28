// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Bounded wireless audio-startup wait loop and supervisor-owned retiring-worker
// lifecycle. Keeps the session control bus and the supervisor-owned
// ManagedVideoBroker responsive while AudioRenderWorker opens its audio route
// on its background thread — AND when startup is cancelled (wired preemption,
// StopProjection, or bus/process exit), moves the still-opening worker out of
// the dispatch path instead of joining its synchronous router open on the
// supervisor thread.

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
#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace aa::wireless_receiver {

class WirelessAudioLifecycle final {
public:
    WirelessAudioLifecycle() = default;
    ~WirelessAudioLifecycle() { drain_all(); }
    WirelessAudioLifecycle(const WirelessAudioLifecycle&) = delete;
    WirelessAudioLifecycle& operator=(const WirelessAudioLifecycle&) = delete;

    // Stop an opening worker and park it off the supervisor dispatch path so
    // cancellation never blocks in thread::join() while AudioRouter::open runs.
    void retire(std::unique_ptr<audio::AudioRenderWorker> worker) {
        if (!worker) return;
        worker->stop();
        if (worker->thread_exited()) {
            worker.reset();
            return;
        }
        retiring_.push_back(std::move(worker));
    }

    // Reap any retired workers whose background threads have already exited.
    void reap() {
        retiring_.erase(
            std::remove_if(retiring_.begin(), retiring_.end(),
                           [](const std::unique_ptr<audio::AudioRenderWorker>& worker) {
                               return !worker || worker->thread_exited();
                           }),
            retiring_.end());
    }

    // Bounded wait for the oldest retiring worker to exit, then reap.
    bool wait_one_for(std::chrono::milliseconds timeout) {
        reap();
        if (retiring_.empty()) return true;
        const bool exited = retiring_.front()->wait_exited_for(timeout);
        if (exited) reap();
        return retiring_.empty();
    }

    [[nodiscard]] bool has_retiring() const noexcept { return !retiring_.empty(); }
    [[nodiscard]] std::size_t retiring_count() const noexcept { return retiring_.size(); }

    void drain_all() {
        for (auto& worker : retiring_) {
            if (worker) worker->stop();
        }
        retiring_.clear();
    }

private:
    std::vector<std::unique_ptr<audio::AudioRenderWorker>> retiring_{};
};

struct WirelessAudioStartupResult final {
    std::unique_ptr<audio::AudioRenderWorker> worker{};
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
            return {nullptr, std::move(ready), std::nullopt, false};
        }
        if (bus_cycle) {
            if (auto step = bus_cycle(core::Milliseconds{0}); !step) {
                return {nullptr, std::nullopt, step.error(), true};
            }
        }
        if (!running_flag.load() || std::chrono::steady_clock::now() >= deadline) {
            return {nullptr, std::nullopt, std::nullopt, true};
        }
        if (keep_running && bus_state) {
            if (!keep_running(bus_state(), std::nullopt)) {
                return {nullptr, std::nullopt, std::nullopt, true};
            }
        }
        if (managed_video != nullptr) {
            (void)managed_video->poll(video);
        }
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            return {nullptr, std::nullopt, std::nullopt, true};
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
            return {nullptr, std::move(ready), std::nullopt, false};
        }
    }
}

inline WirelessAudioStartupResult start_wireless_audio_ready(
    WirelessAudioLifecycle& lifecycle,
    const std::function<std::unique_ptr<audio::AudioRenderWorker>()>& make_worker,
    const std::function<core::Result<void>(core::Milliseconds)>& bus_cycle,
    const std::function<ipc::StateSnapshot()>& bus_state,
    ipc::VideoServer& video,
    ManagedVideoBroker* managed_video,
    const std::function<bool(ipc::StateSnapshot, const std::optional<trust::TransportIdentity>&)>&
        keep_running,
    std::chrono::steady_clock::time_point deadline,
    const std::atomic<bool>& running_flag) {
    const auto compute_slice = [&](std::chrono::steady_clock::time_point now) {
        const auto slice_cap =
            (managed_video != nullptr) ? std::chrono::milliseconds{5}
                                       : std::chrono::milliseconds{10};
        auto slice = slice_cap;
        if (deadline != std::chrono::steady_clock::time_point::max()) {
            const auto remaining =
                std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
            slice = std::clamp(remaining, std::chrono::milliseconds{1}, slice_cap);
        }
        return slice;
    };

    const auto pump_control_and_video = [&]() -> std::optional<WirelessAudioStartupResult> {
        if (bus_cycle) {
            if (auto step = bus_cycle(core::Milliseconds{0}); !step) {
                return WirelessAudioStartupResult{nullptr, std::nullopt, step.error(), true};
            }
        }
        if (!running_flag.load() || std::chrono::steady_clock::now() >= deadline) {
            return WirelessAudioStartupResult{nullptr, std::nullopt, std::nullopt, true};
        }
        if (keep_running && bus_state) {
            if (!keep_running(bus_state(), std::nullopt)) {
                return WirelessAudioStartupResult{nullptr, std::nullopt, std::nullopt, true};
            }
        }
        if (managed_video != nullptr) {
            (void)managed_video->poll(video);
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            return WirelessAudioStartupResult{nullptr, std::nullopt, std::nullopt, true};
        }
        return std::nullopt;
    };

    // Wait non-blockingly for any previously retired opening worker to exit
    // before opening another audio route so two opens never overlap on the
    // audio device, while keeping bus and video broker dispatch responsive.
    while (lifecycle.has_retiring()) {
        lifecycle.reap();
        if (!lifecycle.has_retiring()) break;
        if (auto stop = pump_control_and_video()) {
            return std::move(*stop);
        }
        (void)lifecycle.wait_one_for(compute_slice(std::chrono::steady_clock::now()));
    }

    auto worker = make_worker();
    if (!worker) {
        return {nullptr, Error{ErrorCode::internal}, std::nullopt, false};
    }

    for (;;) {
        if (auto ready = worker->wait_ready_for(std::chrono::milliseconds{0})) {
            return {std::move(worker), std::move(ready), std::nullopt, false};
        }
        if (auto stop = pump_control_and_video()) {
            lifecycle.retire(std::move(worker));
            return std::move(*stop);
        }
        if (auto ready = worker->wait_ready_for(compute_slice(std::chrono::steady_clock::now()))) {
            return {std::move(worker), std::move(ready), std::nullopt, false};
        }
    }
}

} // namespace aa::wireless_receiver
