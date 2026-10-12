// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Private seam between the GStreamer-free router (src/audio/AudioRouter.cpp)
// and the routing adapter (src/audio/gstreamer_adapter/). Not an installed
// header; no GStreamer types cross it (boundary_scan adapter zone stays
// confined to the adapter directory).

#include <aa/audio/AudioMix.hpp>
#include <aa/audio/AudioRouter.hpp>
#include <aa/core/Result.hpp>
#include <aa/core/Time.hpp>

#include <memory>
#include <string>

namespace aa::audio::detail {

// The routed output sink (gate r2 F1 semantics).
//
// write() returns only once the mixed chunk has reached the RENDERER'S
// HANDOFF: the element's render call actually handed the audio to PipeWire/
// Pulse (the `gst_pipewire_sink_render: push buffer` /
// `gst_pulseringbuffer_commit` event, observed via a GStreamer debug
// observer). Earlier stages are kept as separate observations and never
// acknowledge a write: element-ingress (pad arrival) and chain-return (the
// zero-length marker after the data's chain). This is still NOT the device
// write endpoint (samples reaching the hardware), which is unmeasured.
//
// drain() is an end-of-stream round-trip: it lets the element's render path
// finish queued work before teardown. It makes NO device-consumption or
// delivery guarantee and is never used to claim one.
class ObservedAudioSink {
public:
    virtual ~ObservedAudioSink() = default;
    ObservedAudioSink() = default;
    ObservedAudioSink(const ObservedAudioSink&) = delete;
    ObservedAudioSink& operator=(const ObservedAudioSink&) = delete;
    ObservedAudioSink(ObservedAudioSink&&) = delete;
    ObservedAudioSink& operator=(ObservedAudioSink&&) = delete;

    [[nodiscard]] virtual core::Result<void> write(const MixedChunk& chunk) = 0;
    // Bounded render drain: pushes end-of-stream and waits for the sink to
    // confirm it rendered everything acknowledged (bus EOS round-trip).
    // Terminal for the route (further writes fail typed). Returns the drain
    // outcome; render_stats().drains_completed advances only on completion.
    [[nodiscard]] virtual core::Result<void> drain(core::Milliseconds timeout) = 0;
    [[nodiscard]] virtual SinkProbeStats probe_stats() const = 0;
    [[nodiscard]] virtual RenderBarrierStats render_stats() const = 0;
};

struct SinkOpenRequest final {
    AudioFormat format{};
    SinkBackend backend{SinkBackend::automatic};
    std::string target{}; // empty = current default output
    core::Milliseconds start_timeout{core::Milliseconds{2000}};
    core::Milliseconds write_timeout{core::Milliseconds{2000}};
    // Test-only fault injection (gate r2 F1 regression): drops the nonempty
    // data buffer at the routing element's ingress while the chain-return
    // marker proceeds, proving the handoff barrier can never acknowledge
    // audio that was never rendered. Never set by production callers.
    bool drop_nonempty_at_ingress{false};
    bool drop_nonempty_after_window{false};
};

struct SinkOpenOutcome final {
    std::unique_ptr<ObservedAudioSink> sink{}; // null when the route is unusable
    SinkReport report{};                       // probe + selection with recorded causes
};

// Probes, selects, builds and starts the routing pipeline with the task-23
// honesty rule, then CONFIRMS ROUTE READINESS within start_timeout: a linked
// default output must exist (the routing backend's device registry must show a
// matching Audio/Sink output). An unusable selection, a full path that cannot
// be constructed or started, an unresolvable explicit target, or an
// unconfirmed route yields sink == nullptr with the cause recorded in the
// report - never a silent misroute.
[[nodiscard]] SinkOpenOutcome open_default_sink(const SinkOpenRequest& request);

// Test-only opener: builds the router around a caller-supplied sink (e.g. a
// recording fake) instead of probing/starting the GStreamer routing element.
// Same private seam - no GStreamer types cross it, no public API.
[[nodiscard]] core::Result<AudioRouter> open_router_with_sink_for_test(
    const RouterConfig& config, std::unique_ptr<ObservedAudioSink> sink);

// Test-only (gate r3 F1 regression): runs an UNRELATED in-process
// `audiotestsrc ! audioconvert ! pipewiresink` pipeline in this process. Its
// render pushes are real handoff-signature trace lines on their own streaming
// thread and must never acknowledge the tracked route's writes. Returns false
// when the traffic pipeline cannot be started.
[[nodiscard]] bool start_foreign_sink_traffic(std::uint32_t buffers);
void stop_foreign_sink_traffic() noexcept;
[[nodiscard]] std::uint64_t handoff_events_seen() noexcept;

} // namespace aa::audio::detail
