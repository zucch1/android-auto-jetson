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

#include <cstdint>
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
// Timeout semantics (gate fix 3): the write timeout is ONE absolute deadline
// for the whole write. Queue admission (the appsrc must hold the complete
// data+marker pair), chain settlement and the render handoff all share that
// single budget - a write never waits twice the timeout. A write that times
// out or loses the route fails typed (timeout / audio_sink_unavailable) and
// LATCHES: every later write fails immediately, so a late render callback can
// never resurrect a failed write. Format errors and oversized writes (over
// 64 KiB, the router splits larger intervals) are audio_format_unsupported
// and do not latch.
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

// ---- Test-only queue admission hooks (gate fix 3) ----
// Private test seams only: they let the audio_pipeline tests block the route's
// downstream AFTER readiness and observe the appsrc queue while the admission
// contract is exercised. No production caller uses them; no GStreamer type
// crosses this seam and no production API changes with them.

// The appsrc queue state of the routing sink: what is queued right now and the
// configured admission limits (bytes / buffers).
struct AppsrcQueueLevel final {
    std::uint64_t queued_bytes{};
    std::uint64_t queued_buffers{};
    std::uint64_t max_bytes{};
    std::uint64_t max_buffers{};
};

// Blocks the routing element's sink pad (a blocking pad probe) so downstream
// render stalls and pushed buffers accumulate in the appsrc queue. False when
// the sink has no attachable pad or the block cannot be installed.
[[nodiscard]] bool block_downstream_for_test(ObservedAudioSink& sink);
// True once the block has actually HELD a buffer - the stall is engaged and
// later pushes can only accumulate in the queue.
[[nodiscard]] bool wait_downstream_block_entered_for_test(ObservedAudioSink& sink,
                                                          core::Milliseconds timeout);
// Releases the block (idempotent). Tests must call this on EVERY exit path,
// including assertion failures, before teardown.
void unblock_downstream_for_test(ObservedAudioSink& sink) noexcept;
// Pushes raw zero-filled buffers straight into appsrc (no render barrier and
// no handoff acknowledgement) until the queue's byte level reaches
// target_bytes; returns the level reached. Fills the queue to its admission
// threshold while the downstream block holds it.
[[nodiscard]] std::uint64_t prefill_appsrc_for_test(ObservedAudioSink& sink,
                                                    std::uint64_t target_bytes);
// Reads the appsrc queue levels and the configured limits.
[[nodiscard]] AppsrcQueueLevel appsrc_queue_level_for_test(ObservedAudioSink& sink);

} // namespace aa::audio::detail
