// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <aa/audio/Audio.hpp>
#include <aa/audio/AudioDecode.hpp>
#include <aa/audio/AudioMix.hpp>
#include <aa/core/Result.hpp>
#include <aa/core/Time.hpp>
#include <aa/protocol/Protocol.hpp>
#include <aa/diagnostics/QueueRejection.hpp>

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace aa::audio {

// Routing backend policy (task 25 honesty pattern, mirrors the task-23
// consumer decode helper): the available GStreamer routing elements are
// probed at runtime (factory find + instantiate + READY) and every known
// element is reported with availability/viability and a recorded cause. The
// selected route targets the current default PipeWire output; when no
// PipeWire-capable element is usable the router degrades typed and never
// silently misroutes. pipewiresink routes to PipeWire natively; pulsesink
// routes via the PulseAudio-compatible layer (pipewire-pulse on a PipeWire
// host) and is the recorded fallback.
enum class SinkBackend : std::uint8_t { automatic, pipewire, pulse };

[[nodiscard]] std::string_view to_string(SinkBackend backend) noexcept;

struct SinkCandidate final {
    std::string element{};
    bool pipewire_capable{};
    bool available{};
    bool viable{};
    std::string cause{};
};

struct SinkReport final {
    SinkBackend requested{SinkBackend::automatic};
    std::vector<SinkCandidate> candidates{};
    std::string selected_element{};
    bool usable{};
    std::string cause{};
};

struct SinkSelection final {
    std::string element{};
    bool usable{};
    std::string cause{};
};

// Probes the routing elements (pipewiresink, pulsesink) with the task-23
// honesty rule: absence is recorded with its cause, never assumed.
[[nodiscard]] SinkReport probe_sinks();

// Pure selection policy over a probe report. Explicit requests never
// downgrade silently: an unsatisfiable request is unusable with a recorded
// "disabled:" cause.
[[nodiscard]] SinkSelection select_sink_backend(const SinkReport& report,
                                                SinkBackend request);

// Per-role stream shape as negotiated on the channel/session audio path.
// Router codec contract: this router accepts PCM and AAC-LC ADTS; raw AAC-LC
// (no ADTS framing) is rejected at open with audio_format_unsupported.
// RoleStreamConfig carries no AudioSpecificConfig and the decoder requires
// EncodedChunk::codec_data for raw AAC-LC - the standalone decoder's raw-AAC
// capability is unchanged.
struct RoleStreamConfig final {
    Codec codec{Codec::pcm};
    AudioFormat input_format{};
};

// Common monotonic scheduled interval and consumer-side stream-write completion.
// Completion follows the handoff-confirmed RenderBarrier; not speaker output.
struct WriteCompletion final {
    core::Nanoseconds scheduled{};
    core::Nanoseconds scheduled_end{};
    core::Nanoseconds completed{};
};

struct RenderStep final {
    // Headers of parents present in the detached output, even if invalidated
    // during its write. Silence has no contributors.
    std::vector<protocol::MessageHeader> contributors{};
    // Fully consumed parents, once, after successful write completion (or a
    // whole-parent drift trim). Cancellation suppresses stale completion credits.
    std::vector<protocol::MessageHeader> completed{};
    bool pending{};
};

// One backend write, not device delivery. Optional stamps mean unobserved,
// never inferred. Callbacks can run on streaming threads: copy scalars only,
// without logging, waiting, or reentering the router. Sequence includes startup
// readiness attempts and is independent of the render-credit sequence.
struct SinkWriteProgress final {
    std::uint64_t sequence{};
    bool readiness_probe{};
    core::Nanoseconds effective_scheduled{};
    core::Nanoseconds entered_at{};
    core::Nanoseconds deadline{};
    core::Nanoseconds levels_at{};
    bool levels_valid{};
    std::uint64_t queued_bytes{};
    std::uint64_t queued_buffers{};
    std::uint64_t max_bytes{};
    std::uint64_t max_buffers{};
    std::optional<core::Nanoseconds> admitted_at{};
    std::optional<core::Nanoseconds> pair_pushed_at{};
    std::optional<core::Nanoseconds> ingress_at{};
    std::optional<core::Nanoseconds> marker_at{};
    std::optional<core::Nanoseconds> handoff_at{};
    std::optional<core::Nanoseconds> completed_at{};
    std::optional<ErrorCode> outcome{}; // absent on success or before completion
    std::optional<std::uint64_t> pts_ns{};
    std::optional<std::int64_t> pts_origin_ns{};
    // Entry-time clock readings, not a segment-adjusted scheduling verdict.
    std::optional<std::uint64_t> clock_ns{};
    std::optional<std::uint64_t> base_time_ns{};
    std::optional<std::uint64_t> running_time_ns{};
    [[nodiscard]] friend bool operator==(const SinkWriteProgress&, const SinkWriteProgress&) = default;
};

// Live-PCM drift correction policy (opt-in). The default RouterConfig stays
// lossless: every decoded frame is preserved and the timeline never re-times
// or drops audio. Live PCM cannot stay lossless across clock domains - the
// producer's media clock disagrees with the output clock by ~100-200 ppm, so
// preserving every frame accumulates an unbounded backlog. When enabled the
// router measures each run's placement phase (planned start minus the run's
// original scheduled timestamp) before the run is committed to the sink and
// trims bounded head frames from over-watermark runs, advancing the timeline
// by the RETAINED frames only. Trimmed audio is dropped, never resynthesized;
// timestamps are never rebased to "now". Only the wired receiver's
// pcm_router_config() enables this.
struct LivePcmDriftPolicy final {
    bool enabled{false};
    core::Nanoseconds high_watermark{core::Nanoseconds{20'000'000LL}};
    core::Nanoseconds target{core::Nanoseconds{15'000'000LL}};
    core::Nanoseconds max_trim{core::Nanoseconds{5'000'000LL}};

    [[nodiscard]] friend constexpr bool operator==(const LivePcmDriftPolicy&,
                                                   const LivePcmDriftPolicy&) noexcept = default;
};

// Pure trim decision for one decoded run about to be committed: `phase` is
// planned start minus the run's original scheduled timestamp. Returns the
// frames to drop from the HEAD of the run (0 = lossless), never more than the
// run holds. Extraction point for the long-run drift simulations.
class LivePcmDriftCorrector final {
public:
    LivePcmDriftCorrector() noexcept = default;
    LivePcmDriftCorrector(LivePcmDriftPolicy policy, std::uint32_t output_rate_hz) noexcept;

    [[nodiscard]] std::uint64_t trim_frames(core::Nanoseconds phase,
                                            std::uint64_t run_frames) const noexcept;

private:
    LivePcmDriftPolicy policy_{};
    std::uint64_t rate_{};
};

struct RouterConfig final {
    AudioFormat output_format{48000, 2, 16};
    std::array<RoleStreamConfig, 4> roles{};
    SinkBackend backend{SinkBackend::automatic};
    MixPolicy mix{};
    core::Milliseconds sink_start_timeout{core::Milliseconds{2000}};
    // Bounded render/write-completion wait per mixed chunk (the marker
    // barrier). A write that cannot be confirmed rendered within this bound is
    // an output loss: the router degrades typed with the cause recorded.
    core::Milliseconds sink_write_timeout{core::Milliseconds{2000}};
    // Test-only fault injection (gate r2 F1 regression): drops nonempty data
    // buffers at the routing element's ingress (the chain-return marker
    // proceeds) so a test can prove an un-rendered write is never
    // acknowledged. Production leaves this false.
    bool drop_nonempty_at_ingress{false};
    // Test-only fault injection (gate r3 F1 regression): opens the write's
    // render window and THEN drops the nonempty buffer - the GDB scenario in
    // which foreign pipelines' pushes tried to acknowledge the write.
    bool drop_nonempty_after_window{false};
    // Optional PipeWire remote name override for the routing element (empty =
    // current default). Used by tests to point the stream at an unreachable
    // remote and prove the degraded typed path against the real element.
    std::string sink_remote{};
    std::function<void(const WriteCompletion&)> on_write_completed{};
    // Live-PCM drift correction (opt-in; default lossless/unchanged). Enabled
    // only by the wired receiver's pcm_router_config().
    LivePcmDriftPolicy live_pcm_drift{};
    std::function<void(const SinkWriteProgress&)> on_sink_write_progress{};
    // render_step() interval bound. Each sink write costs PipeWire about one
    // graph cycle (1024 frames) while catching up after a stall; 20 ms pieces
    // (3 per 42.67 ms media parent) never recovered. 50 ms renders one write per
    // observed media parent.
    core::Nanoseconds render_quantum{core::Nanoseconds{50'000'000}};
};

enum class RouterState : std::uint8_t { active, degraded };

// Observed values (measurements) recorded on the common monotonic clock at
// receiver input (submit() BEFORE decoding) and at the routing element's
// render-completion barrier (see RenderBarrierStats). Budget verdicts
// (<=200 ms latency, <=50 ms A/V-sync envelope) are asserted separately in
// tests; these fields carry the raw observations only. The device/sink-write
// endpoint (when samples reach the output hardware) is NOT measured here -
// these are input-to-element-completion observations only.
struct RouterStats final {
    std::uint64_t chunks_submitted{};
    std::uint64_t frames_mixed{};
    std::uint64_t frames_written{};
    std::uint64_t samples_written{};
    core::Nanoseconds last_path_latency{};
    core::Nanoseconds max_path_latency{};
    // Receiver-input stamp (chunk timestamp) to the render-completion barrier.
    // Not a device-delivery measurement.
    core::Nanoseconds last_completion_skew{};
    core::Nanoseconds max_completion_skew{};
    // Live-PCM drift accounting (additive: existing telemetry fields and
    // semantics are unchanged). frames_decoded counts every decoded input
    // frame at submit; frames_retained/frames_trimmed account the frames
    // committed to the sink (input = retained + trimmed + pending absent
    // cancellation/loss). Retained input counts each contributing role; a
    // successfully written detached piece remains retained after cancellation.
    // frames_silence counts synthesized gap frames; frames_output counts mixed
    // timeline frames written to the sink, including silence. frames_skipped
    // counts elapsed backpressure gap frames advanced without any write.
    std::uint64_t frames_decoded{};
    std::uint64_t frames_retained{};
    std::uint64_t frames_trimmed{};
    std::uint64_t frames_silence{};
    std::uint64_t frames_output{};
    std::uint64_t frames_skipped{};
    // Elapsed all-role-idle prefix frames advanced without a write. Overlap
    // belongs to frames_skipped (backpressure), never to both counters.
    std::uint64_t frames_idle_skipped{};
};

// ELEMENT-INGRESS observations (task-25 gate fix round 2): what entered the
// routing element's sink pad, counted at pad arrival BEFORE the element's
// chain/render runs. This is an observation of element ingress only - it is
// NOT a sink-write, delivery, or render-completion confirmation (those live in
// RenderBarrierStats). Constant fixtures make min==max==the exact mixed value.
struct SinkProbeStats final {
    std::uint64_t buffers{};
    std::uint64_t samples{};
    std::int32_t min_sample{};
    std::int32_t max_sample{};

    [[nodiscard]] bool non_silent() const noexcept {
        return min_sample != 0 || max_sample != 0;
    }
};

// Three distinct observation levels (gate r2 F1), never conflated:
// element-ingress (SinkProbeStats: what entered the sink pad), chain-return
// (the zero-length marker reaching the pad after the data's chain returned),
// and HANDOFF (the renderer's actual PipeWire/Pulse handoff event - the
// `gst_pipewire_sink_render: push buffer` / `gst_pulseringbuffer_commit`
// render call, observed via a GStreamer debug observer). Only HANDOFF
// acknowledges a write: the element silently drops buffers while its stream
// is not streaming, and that drop looks like success at every earlier level.
// drains_completed counts bounded end-of-stream round-trips: the element's
// render path finished the queued work before teardown - NOT a device-
// consumption or delivery guarantee.
struct RenderBarrierStats final {
    std::uint64_t chain_return_buffers{};
    std::uint64_t chain_return_samples{};
    std::uint64_t handoff_events{};
    // Process-wide count of every handoff-signature event seen (credited or
    // not). handoff_events counts only events bound to THIS route's writes;
    // the gap between the two is foreign/unattributed traffic that failed
    // closed (gate r3 F1).
    std::uint64_t handoff_events_seen{};
    std::int32_t stream_state{-1};
    std::uint64_t drains_completed{};
};

enum class RenderFailureReason : std::uint8_t {
    closed, degraded, flushed, missing_sink, zero_quantum,
    cursor_range, quantum_end_overflow, intended_time_conversion, late,
    placement_time_conversion, parent_end_overflow, parent_end_time_conversion,
    single_gap, aggregate_gap, output_start_time_conversion, output_end_time_conversion,
    mixer_error, sink_write_error
};

// Enclosing bounds are NOT continuous coverage. Effective ranges are clipped
// to the failing gap; backpressure owns overlap with idle authorization.
struct RenderRangeSummary final {
    std::uint64_t count{};
    std::optional<std::int64_t> start_ns{}, end_ns{};
    std::optional<std::uint64_t> first_frame{}, last_frame{};
    std::uint64_t excluded_frames{};
};

// Bounded, owning values only. Uncomputed operands remain absent, not zero.
struct RenderFailureSnapshot final {
    RenderFailureReason reason{};
    std::optional<ErrorCode> underlying_code{};
    std::uint32_t output_sample_rate{};
    std::optional<std::int64_t> base_ns{};
    bool base_committed{};
    std::uint64_t entry_cursor{};
    std::optional<std::uint64_t> quantum{}, cap{};
    std::optional<protocol::MessageHeader> offending{};
    std::optional<bool> preflighted{};
    std::optional<std::int64_t> intended{};
    std::optional<std::uint64_t> role_end{}, start{}, remaining{}, time_conversion_frame{};
    std::optional<std::uint64_t> covered_end{}, raw_gap{}, uncovered_gap{}, total_gap_before{};
    std::optional<std::int64_t> now_ns{};
    std::optional<std::uint64_t> prefix_end{}, stored_idle_prefix{}, output_start{}, output_end{};
    RenderRangeSummary stored_backpressure{}, stored_idle{};
    std::optional<RenderRangeSummary> effective_backpressure{}, effective_idle{};
};

struct RouterReport final {
    RouterState state{RouterState::active};
    SinkReport sink{};
    RouterStats stats{};
    SinkProbeStats sink_probe{};
    RenderBarrierStats render_barrier{};
    // Last submit rejection, cleared at the next submit; retained by close().
    std::optional<diagnostics::QueueRejection> queue_rejection{};
    // Cleared only at the next render_step entry; cleanup preserves evidence.
    std::optional<RenderFailureSnapshot> render_failure{};
};

// Four-channel AAP audio router (task 25): channel/session messages in,
// decode per role, duck/mix, route to the default PipeWire output. Caller-
// driven and thread-free (GStreamer streaming threads stay inside the
// pipelines). While degraded every submit()/pump() fails typed
// audio_sink_unavailable - never silence pretending success.
class AudioRouter final {
public:
    // Opens the router for the configured roles. Router codec contract: this
    // router accepts PCM and AAC-LC ADTS; raw AAC-LC (no ADTS framing) is
    // rejected at open with audio_format_unsupported (see RoleStreamConfig).
    [[nodiscard]] static core::Result<AudioRouter> open(const RouterConfig& config);

    AudioRouter(AudioRouter&&) noexcept;
    AudioRouter& operator=(AudioRouter&&) noexcept;
    ~AudioRouter();

    // Channel/session audio path entry (task-18 dispatch shape). Non-audio
    // role -> invalid_argument; over the payload bound -> invalid_argument;
    // decode failure -> audio_decode_failed / audio_format_unsupported;
    // negative timestamp -> invalid_argument (rejected before decode).
    // Terminal flush state: once flush() has committed the route's EOS drain
    // the route is terminal and submit() fails invalid_argument BEFORE
    // decoding or sink access. While degraded it fails audio_sink_unavailable
    // (a failed flush keeps that degraded refusal and its recorded cause); a
    // closed route fails invalid_argument (closed-state precedence).
    [[nodiscard]] core::Result<void> submit(const protocol::MessageView& message);
    // Owner-captured monotonic receipt, before copying/enqueueing. Only the
    // path-latency origin changes; the message's scheduled timestamp is intact.
    [[nodiscard]] core::Result<void> submit(const protocol::MessageView& message,
                                           core::Nanoseconds receiver_input_at);

    // Mixes and writes every pending frame to the routing element. A write
    // failure (bus error, render-barrier stall, drained route) transitions the
    // router to degraded with the cause recorded and fails typed
    // audio_sink_unavailable. Runs are scheduled on one persistent sample
    // timeline across pumps (first nonempty pump anchors the base; split and
    // batched pumps of one schedule produce identical output); a gap or
    // late-arrival discontinuity above the 200 ms safety caps is rejected
    // invalid_argument before any write (the batch is discarded, cursor and
    // counters unchanged). render_step() alone permits the explicitly observed
    // elapsed gaps documented below. Output pieces are bounded to 64 KiB whole-frame.
    // Terminal flush state: after flush() has committed the route's EOS
    // drain, pump() fails invalid_argument before any sink access; while
    // degraded it fails audio_sink_unavailable.
    [[nodiscard]] core::Result<void> pump();
    // Worker-only incremental seam: one shared-timeline interval, at most
    // RouterConfig::render_quantum including silence. Decode and drift decisions remain parent-scoped.
    // submit/step are owner-thread operations; cancel_pending may run during
    // the sink write. The detached mixed piece alone survives cancellation.
    // Other operations (including close/move/destruction) require the owner
    // thread; the router must outlive every concurrent cancel_pending call.
    [[nodiscard]] core::Result<RenderStep> render_step();
    void cancel_pending(protocol::ChannelRole role);
    // Records a steady-clock interval during which the owner withheld peer
    // credit, or a completed sink write exceeded one PCM piece duration and
    // its scheduled end (excluding normal per-piece pacing). render_step()
    // advances its cursor without writing through the
    // already-elapsed part of an all-role gap inside such an interval; that
    // part is exempt from the 200 ms silence caps. Unexplained gaps, future
    // gaps and late arrivals keep the strict policy. Non-incremental pump()
    // ignores these observations.
    void note_backpressure(core::Nanoseconds start, core::Nanoseconds end);
    // Worker-only observation of settled all-role idle lasting >200 ms.
    // Authorizes only whole, already-elapsed frames within [start, end) in
    // the NEXT resumed schedule's leading uncovered prefix. All pending
    // parents are preflighted before any advance; internal gaps and stale or
    // future remainders keep the 200 ms caps. Repeated observations cannot
    // extend an outstanding authorization. Rejection discards it; first
    // resumed PCM consumption retires it. No rebase, write or parent credit
    // is produced by skipping. pump() ignores this unless already incremental.
    void note_all_role_idle(core::Nanoseconds start, core::Nanoseconds end);
    // Worker-only: discard outstanding idle authorization when the worker
    // confirms a settled-idle transition, never merely because queues are empty.
    // Leaves timeline, counters, pending parents and backpressure unchanged;
    // a later note_all_role_idle() may authorize a new resumed schedule.
    void retire_all_role_idle();
    // Bounded render drain: mixes and writes pending frames, then drains the
    // routed stream to end-of-stream (render completion of everything
    // acknowledged). Terminal for the route: flush() marks the route
    // terminal immediately BEFORE its single EOS drain - afterwards
    // submit()/pump() fail invalid_argument before decode or sink access,
    // and a drain that cannot complete leaves the route degraded with the
    // cause recorded and the typed audio_sink_unavailable refusal retained.
    // Repeated flush() is idempotent - it never sends a second EOS - and
    // close() never drains again after flush(). A pump/preflight failure
    // before the EOS drain does NOT mark the route flushed.
    [[nodiscard]] core::Result<void> flush();

    void set_muted(bool muted) noexcept;
    [[nodiscard]] bool muted() const noexcept;
    void close() noexcept;

    [[nodiscard]] RouterState state() const noexcept;
    [[nodiscard]] const RouterReport& report() const noexcept;

    struct Impl;
    explicit AudioRouter(std::unique_ptr<Impl> impl) noexcept;

private:
    std::unique_ptr<Impl> impl_;
};

} // namespace aa::audio
