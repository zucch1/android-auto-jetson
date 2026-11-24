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
#include <chrono>
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
// host). pulsesink is the default and only supported real-time route: on the
// target (PipeWire 1.0.5, Ubuntu noble) pipewiresink has no rate control and
// degrades after 1.5-2.3 h, and no fixed PipeWire is packaged for noble.
// Explicit pipewire requests are refused outright at startup (#80).
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
// downgrade silently: pipewire is refused outright naming pulsesink as the
// supported route, and automatic selects only pulsesink without falling back
// to pipewiresink.
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

// Diagnostic only: absence never falls back to a handoff A/V endpoint.
enum class PlaybackSlotAbsence : std::uint8_t {
    none, unavailable, worker_busy, worker_stopped, deadline_missed,
    query_failed, negative_position, invalid_pts
};

[[nodiscard]] constexpr std::string_view to_string(PlaybackSlotAbsence reason) noexcept {
    switch (reason) {
    case PlaybackSlotAbsence::none: return "none";
    case PlaybackSlotAbsence::unavailable: return "unavailable";
    case PlaybackSlotAbsence::worker_busy: return "worker_busy";
    case PlaybackSlotAbsence::worker_stopped: return "worker_stopped";
    case PlaybackSlotAbsence::deadline_missed: return "deadline_missed";
    case PlaybackSlotAbsence::query_failed: return "query_failed";
    case PlaybackSlotAbsence::negative_position: return "negative_position";
    case PlaybackSlotAbsence::invalid_pts: return "invalid_pts";
    }
    return "unavailable";
}

// Common monotonic scheduled interval and consumer-side stream-write completion.
// Completion follows the handoff-confirmed RenderBarrier; not speaker output.
struct WriteCompletion final {
    core::Nanoseconds scheduled{};
    core::Nanoseconds scheduled_end{};
    core::Nanoseconds completed{};
    // #60: when the chunk starts playing, max(completed, playback slot on the
    // sink's clock). A sample cannot play before its handoff, and with
    // sync=true it does not play before its slot; pulsesink hands off early
    // (ring-buffer commit), pipewiresink near the slot. Absent when the sink
    // reported no position.
    std::optional<core::Nanoseconds> presented{};
    PlaybackSlotAbsence playback_slot_absence{PlaybackSlotAbsence::unavailable};
    // Wireless playout (PlayoutPolicy): how far after its content time this
    // piece is scheduled - the playout buffer depth its parents were placed
    // with (the largest among contributors). Zero on the arrival model
    // (wired): scheduled is then the arrival-stamped content time itself.
    core::Nanoseconds playout_offset{};
    // The same at scheduled_end. It differs from playout_offset only while
    // an offset increase is absorbed (the reported content clock never runs
    // backwards, so it advances at half rate for a while).
    core::Nanoseconds playout_end_offset{};
    // Lossless playout: the offsets the piece really plays with, which is what
    // the video hold follows. Above playout_offset when the write blocked past
    // its slot or the sink's clock lags (and above playout_end_offset when what
    // follows is retimed); equal to them otherwise.
    core::Nanoseconds endpoint_offset{};
    core::Nanoseconds endpoint_end_offset{};
};

// Lossless playout: one piece about to be handed to the sink, with the offsets
// on_write_completed will report unless the last stream is cancelled meanwhile.
// Content from scheduled - playout_offset on is not heard before it returns.
struct WriteStart final {
    core::Nanoseconds scheduled{};
    core::Nanoseconds scheduled_end{};
    core::Nanoseconds playout_offset{};
    core::Nanoseconds playout_end_offset{};
};

struct RenderStep final {
    // Headers of parents present in the detached output, even if invalidated
    // during its write. Silence has no contributors.
    std::vector<protocol::MessageHeader> contributors{};
    // Fully consumed parents, once, after successful write completion (or a
    // whole-parent drift trim). Cancellation suppresses stale completion credits.
    std::vector<protocol::MessageHeader> completed{};
    // Parents whose final frames were in this successful write, even if their
    // role was cancelled during it. Delivery accounting only, never a credit;
    // whole-parent drift trims are not deliveries.
    std::vector<protocol::MessageHeader> delivered{};
    // More can be rendered now.
    bool pending{};
    // Lossless playout: nothing was rendered because a playing role has not
    // delivered its next chunk yet; call again at this time (RouterConfig::
    // clock), or earlier once a chunk was submitted or a role cancelled.
    std::optional<core::Nanoseconds> wait_until{};
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
// timestamps are never rebased to "now". The shared pcm_router_config() enables
// the policy for both receivers (#163) with catch_up_watermark off (0); a
// nonzero watermark applies to the arrival model only (--audio-playout-ms 0).
// Wired's lossless playout (PlayoutPolicy::lossless) corrects the rate instead
// and takes none of these trims.
//
// Catch-up (#154, opt-in): a sink stall withholds credit, and the phone then
// delivers its backlog in a burst stamped at arrival. Sequentialized after
// the cursor, that burst places audio far behind its arrival - past the
// 200 ms late-arrival cap, which ends the session - and the bounded max_trim
// drains it at only ~117 ms/s. When catch_up_watermark > 0, the trigger is
// run-level: a run whose initial phase (how far behind its arrival its first
// sample would be placed) strictly exceeds the watermark has the whole excess
// above `target` trimmed from its head at once, bounded by the run's length.
// The watermark is therefore not a per-sample drop bound: the trim runs down
// to `target`, so later dropped samples were less late than the watermark
// (with a 90 ms watermark, a 4800-frame run at 100 ms phase loses 85 ms, of
// which only the first 10 ms was over 90 ms late; its last dropped sample was
// ~15 ms late). A phase at or below the watermark gets the ordinary bounded
// trim. Every trimmed frame counts in frames_trimmed; frames_caught_up counts
// only the part beyond what the bounded max_trim policy would have removed.
// A run whose decoded output exceeds its parent's own PCM duration (input
// bytes at the role's input format) starts with resampler carryover from
// earlier parents; when the run is already behind placement or overlaps an
// earlier parent on its role, the catch-up judges its phase by that older
// source time so stale carryover is trimmed first and a caught-up run never
// leaves the cursor more than `target` past the end of its parent's PCM. An
// on-time non-overlapping run never lets carryover alone trigger catch-up. The
// router's 200 ms late-arrival cap judges a caught-up run by its parent's own
// PTS range against the committed source-time frontier (bounded by cursor): it
// fails only when the parent's end (timestamp plus input duration) is more than
// 200 ms behind recent source audio (so a backward timestamp jump still fails
// late, while a forward-timestamped short chunk after a non-caught-up 200 ms
// guidance packet survives). Neither the decoded length nor how the resampler
// splits output across runs enters that check.
struct LivePcmDriftPolicy final {
    bool enabled{false};
    core::Nanoseconds high_watermark{core::Nanoseconds{20'000'000LL}};
    core::Nanoseconds target{core::Nanoseconds{15'000'000LL}};
    core::Nanoseconds max_trim{core::Nanoseconds{5'000'000LL}};
    core::Nanoseconds catch_up_watermark{core::Nanoseconds{0}}; // 0 disables catch-up

    [[nodiscard]] friend constexpr bool operator==(const LivePcmDriftPolicy&,
                                                   const LivePcmDriftPolicy&) noexcept = default;
};

// Pure trim decision for one decoded run about to be committed: `phase` is
// planned start minus the run's original scheduled timestamp. Returns the
// frames to drop from the HEAD of the run (0 = lossless), never more than the
// run holds. caught_up_frames() is the part of trim_frames() beyond the
// bounded max_trim, i.e. what the catch-up alone dropped.
// Extraction point for the long-run drift simulations.
class LivePcmDriftCorrector final {
public:
    LivePcmDriftCorrector() noexcept = default;
    LivePcmDriftCorrector(LivePcmDriftPolicy policy, std::uint32_t output_rate_hz) noexcept;

    [[nodiscard]] std::uint64_t trim_frames(core::Nanoseconds phase,
                                            std::uint64_t run_frames) const noexcept;
    [[nodiscard]] std::uint64_t caught_up_frames(core::Nanoseconds phase,
                                                 std::uint64_t run_frames) const noexcept;
    // True when a run at this phase takes the catch-up trim (#154).
    [[nodiscard]] bool catches_up(core::Nanoseconds phase) const noexcept;

private:
    [[nodiscard]] std::uint64_t trim(core::Nanoseconds phase, std::uint64_t run_frames,
                                     bool catch_up) const noexcept;

    LivePcmDriftPolicy policy_{};
    std::uint64_t rate_{};
};

// Playout clock (owner decision 2026-11-20, "credit window + playout"; wired
// since #228, with the lossless rules at the end). The
// arrival model places each chunk at its receipt stamp, so on Wi-Fi every
// round-trip spike becomes a gap (silence) and the burst after it is trimmed or
// judged late. With playout enabled, render_step() schedules each role's
// chunks back to back from its stream on the output clock, `depth` after the
// arrival that anchored the stream:
// - anchor: a role's first chunk (and the first after cancel_pending or a
//   session reset) is placed at arrival + depth;
// - steady: every later chunk follows the role's previous one exactly, so a
//   burst or a short drought inside the buffer costs no trim and no silence;
// - underrun: a chunk arriving after its back-to-back slot (by at most 1 s;
//   later is a resume and counts as an anchor) re-anchors the role at
//   arrival + depth. The elapsed part of the hole is skipped without a
//   write (frames_skipped), the rest is `depth` of silence (frames_silence),
//   and the session continues: playout never fails `late`;
// - drift: the buffer depth of a chunk is its slot minus its arrival. A phone
//   delivering in groups (windows 10-12: up to 5 chunks; it captures in real
//   time, so a group's chunks are late and its last on time) spreads single
//   depths by the group, and a late chunk lowers one; both drain by
//   themselves. Correction judges the window's smallest depth (the group's
//   first, longest-in-transit chunk) once the window holds 2 s of arrivals:
//   the bounded LivePcmDriftPolicy trim (at most max_trim) only when it is
//   more than the high watermark above `depth` (phone clock faster, a
//   backlog), and up to max_trim of silence (refilling toward half the
//   depth) only when it is below a quarter of `depth` (phone clock slower, a
//   slower path), so a jitter spike inside the buffer costs nothing. Within
//   4 s of an underrun the window is the last 0.5 s (older entries are dropped,
//   the re-anchor group itself ignored) and 0.25 s sustains, so the backlog
//   that follows starts draining at once; while that floor is over-full and
//   the offset (the window's largest depth) is above min(depth + 230 ms,
//   350 ms), the backlog is shed at 30% of each chunk - audible as a ~23 Hz
//   gating for the fraction of a second it runs - keeping the offset under
//   the wireless video hold cap (400 ms).
// - offset: the depth of the least-delayed recent arrival, i.e. how long
//   after on-time content audio plays (depth + (group - 1) chunk periods with
//   grouped delivery). WriteCompletion reports it slewed: it moves toward
//   that target by at most half the output time that passes, both ways, so
//   the reported content clock never runs backwards and the video hold that
//   follows it (--video-delay-ms match) never pins a frame behind another.
// - a role's receiver arrival stamps (header.timestamp, the session's
//   monotonic clock) running backwards by more than 200 ms fail closed
//   `late`, as on the arrival model.
// - playout requires LivePcmDriftPolicy::enabled (open fails otherwise).
//
// `lossless` (wired default, #228, owner decision 2026-11-23: wired audio must
// not drop sound, latency is the accepted cost) replaces the drift rules above:
// - no drift trim, no shed and no drift pad: every decoded sample is played.
//   A burst simply deepens the buffer; the depth and the offset grow with it;
// - rate correction: while the window's floor is more than 10 ms above `depth`
//   the role plays slightly fast, below half of `depth` slightly slow
//   (frames_compressed / frames_stretched: frames the resampled run became
//   shorter or longer by). Proportional, at most 0.5% (under 9 cents), up to
//   1% only while the excess is over 100 ms; the rate moves by at most 0.1%
//   per A26 chunk. A 300 ms excess is back under 15 ms in about 45 s;
// - a short run (any packet size down to a third of a millisecond) takes the
//   whole frame its predecessors' fractions add up to;
// - retime: a piece whose slot has already passed when it is about to be written
//   (the write before it blocked, or the render thread was held up) moves the
//   whole timeline to 20 ms from now first: the depth and the offset grow by
//   that and drain by the rate correction (audio_playout_retime). Writing on
//   at slots that had passed would be lossless only on paper: the ring buffer
//   throws samples behind its read position away. The piece inside the blocked
//   write is the sink's: how late it plays is endpoint_offset, and up to all of
//   it (50 ms) can still be lost there, unseen here (#232);
// - stop: what the peer sent before stopping a stream plays out (end_stream);
// - silence is written only on an anchor, a resume or an underrun. A role
//   underruns when its own stream (its end plus what a resampler split still
//   owes) ran out before the arrival, even while another role plays; a stream
//   the split alone held back goes on at its own content slot (an anchor);
// - the roles share one output cursor, so a frame is not mixed before its slot
//   is 20 ms away while a role that plays (heard from in the last second) has
//   not delivered its next chunk: a 200 ms prompt no longer runs the cursor
//   ahead of 42.67 ms media chunks (RenderStep::wait_until);
// - the offset the writes report is media's while media plays;
// - emergency bound: no frame is placed more than `emergency_backlog` after its
//   content time (its chunk's arrival plus its position in the chunk); the part
//   of a chunk beyond it is cut (playout_overflows, frames_overflowed, logged as
//   audio_playout_overflow). It holds however the chunk got there: in a deep
//   stream, as a new stream queued behind the one before (end_stream), or moved
//   by a retime. It bounds memory and delay, and is the line between latency and
//   loss: what follows a stalled write the sink recovers from costs latency as
//   long as the stall and the depth before it stay within it; a sink that fails
//   its write (RouterConfig::sink_write_timeout, 2 s) drops and reopens (#60). A
//   phone bursting its 341 ms capture buffer does not reach it.
struct PlayoutPolicy final {
    bool enabled{false};
    core::Nanoseconds depth{core::Nanoseconds{120'000'000}};
    core::Nanoseconds window{core::Nanoseconds{4'000'000'000LL}};
    bool lossless{false};
    core::Nanoseconds emergency_backlog{core::Nanoseconds{2'000'000'000LL}};

    [[nodiscard]] friend constexpr bool operator==(const PlayoutPolicy&,
                                                   const PlayoutPolicy&) noexcept = default;
};

// Playout defaults, with the reasoning in docs/qualification/wireless-recovery-qa.md
// and docs/qualification/wired-perf-runbook.md.
// Wireless: 120 ms of playout buffer rides out a one-way delay excursion of
// 120 ms (a round trip spike to ~250 ms) and keeps the pulsesink ring (200 ms)
// able to take a whole 42.67 ms chunk ahead of it without blocking the worker.
inline constexpr core::Nanoseconds kWirelessPlayoutDepth{120'000'000};
// Wired (#228): measured on 47 wired A26 runs (5.8 h, 488,000 media chunks,
// 2026-11-15..17). Lateness accumulated within 2 s while the receiver's own sink
// kept up: p99 43 ms over 397 episodes, above 60 ms in 3 (60.6, 61.3 and
// 69.5 ms, all on the 2026-11-15 builds; at most 43.3 ms after). 60 ms covers
// that and leaves 60 + 42.67 = 102.67 ms inside the 200 ms pulsesink ring. The
// larger gaps (up to 204.7 ms) followed the receiver's own sink stalls
// withholding the credit; the lossless queue, not the depth, covers those.
// Table in docs/qualification/wired-perf-runbook.md.
inline constexpr core::Nanoseconds kWiredPlayoutDepth{60'000'000};
// Smallest PlayoutPolicy::emergency_backlog the router accepts.
inline constexpr core::Nanoseconds kMinEmergencyBacklog{1'000'000'000};
// Wireless audio credit window (AudioCreditPolicy, max_unacked): 4 x 42.67 ms
// A26 media chunks = 170 ms of credit, more than the 150 ms round-trip spike
// the replay tests cover and twice window 9's 83 ms spike second.
inline constexpr std::uint32_t kWirelessCreditWindow = 4;
// The largest depth the router accepts: the silence refilling the buffer after
// an underrun must stay under the 200 ms synthesized-silence cap.
inline constexpr core::Nanoseconds kMaxPlayoutDepth{180'000'000};

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
    // Lossless playout only, on the render_step() thread, before every write
    // (synthesized silence included).
    std::function<void(const WriteStart&)> on_write_started{};
    // Live-PCM drift correction. Off in a bare RouterConfig; the receivers'
    // pcm_router_config() enables it with catch-up off (a catch-up watermark
    // is an arrival-model opt-in, --audio-playout-ms 0). Lossless playout
    // requires it enabled but never trims with it.
    LivePcmDriftPolicy live_pcm_drift{};
    std::function<void(const SinkWriteProgress&)> on_sink_write_progress{};
    // render_step() interval bound. Keep the qualified 50 ms batching value.
    // Production uses pulsesink; native pipewiresink is refused on PipeWire
    // 1.0.5 / Ubuntu noble due to missing rate control and long-run degradation (#80).
    core::Nanoseconds render_quantum{core::Nanoseconds{50'000'000}};
    // Playout clock. Off (the arrival model) in a bare RouterConfig;
    // pcm_router_config() turns it on for both receivers: wired 60 ms and
    // lossless, wireless 120 ms with the drift rules.
    PlayoutPolicy playout{};
    // Clock of the arrival stamps (empty: the steady clock). Tests inject it.
    std::function<core::Nanoseconds()> clock{};
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
    // Subset of frames_trimmed: in runs whose initial phase exceeded the
    // catch-up watermark (#154), the frames removed beyond the bounded
    // max_trim drift trim the run would otherwise have had. That ordinary
    // allowance stays in frames_trimmed only.
    std::uint64_t frames_caught_up{};
    std::uint64_t frames_silence{};
    std::uint64_t frames_output{};
    std::uint64_t frames_skipped{};
    // Elapsed all-role-idle prefix frames advanced without a write. Overlap
    // belongs to frames_skipped (backpressure), never to both counters.
    std::uint64_t frames_idle_skipped{};
    // Playout (PlayoutPolicy) observations; all zero on the arrival model.
    // playout_underruns counts re-anchors of a playing role whose chunk
    // arrived after its slot (frames_skipped holds the elapsed hole,
    // frames_silence the refill). playout_depth is the last chunk's buffer
    // depth at arrival (slot minus arrival, before any drift trim or pad),
    // playout_depth_min/max its range over the route excluding anchors, and
    // playout_offset the current offset of the least-delayed arrivals (the
    // target audio_write_completed reports as playout_offset_ns; an increase
    // reaches the record at half rate so the content clock never runs back).
    std::uint64_t playout_underruns{};
    std::uint64_t playout_anchors{};
    std::uint64_t frames_padded{};
    core::Nanoseconds playout_depth{};
    std::optional<core::Nanoseconds> playout_depth_min{};
    std::optional<core::Nanoseconds> playout_depth_max{};
    core::Nanoseconds playout_offset{};
    // How long after its slot the chunk of the latest underrun arrived.
    core::Nanoseconds playout_underrun_late{};
    // Anchors that resumed a paused stream (subset of playout_anchors), and
    // the elapsed time the latest anchor skipped (DeepSeek F3: logged as
    // audio_playout_anchor so a large forward jump is visible).
    std::uint64_t playout_resumes{};
    core::Nanoseconds playout_anchor_skip{};
    // Most chunks of one channel that arrived within 10 ms of each other: the
    // phone's credit use (our acks go out on receipt).
    std::uint64_t max_arrival_burst{};
    // Offset the latest audio_write_completed reported at its end: the
    // target playout_offset slewed by at most half the output time, both
    // ways. The wireless video hold follows the reported offsets.
    core::Nanoseconds playout_reported_offset{};
    // Subset of frames_trimmed: a deep post-drought backlog shed at 30% of
    // each chunk to stay under the video hold cap.
    std::uint64_t frames_shed{};
    // Lossless playout: frames a rate correction shortened or lengthened the
    // decoded runs by (input + stretched = retained + trimmed + compressed +
    // pending), and the emergency-bound losses (subset of frames_trimmed).
    std::uint64_t frames_compressed{};
    std::uint64_t frames_stretched{};
    std::uint64_t playout_overflows{};
    std::uint64_t frames_overflowed{};
    // Lossless playout: times the sink fell behind the schedule (a write blocked
    // past its slot) and everything after it was moved later instead of being
    // written at slots that had passed, and by how much in total.
    std::uint64_t playout_retimes{};
    core::Nanoseconds playout_retimed{};
    // Committed offset above the shedding threshold, even when the buffer
    // minimum prevents additional shedding.
    std::uint64_t playout_shed_residual_chunks{};
    core::Nanoseconds playout_shed_residual_peak{};
};

// One playout re-anchor (stream start, resume, or underrun) of one role, or one
// retime of the whole timeline (lossless), collected for the render worker's
// log (AudioRouter::take_playout_events).
struct PlayoutEvent final {
    enum class Kind : std::uint8_t { start, resume, underrun, retime };
    Kind kind{Kind::start};
    // How long after its slot the run arrived: the elapsed time skipped.
    // retime: how much later everything after it plays.
    core::Nanoseconds late{};
    // Depth the role was re-anchored with; retime: the offset it left.
    core::Nanoseconds depth{};
    Role role{Role::media};
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
    // sink_write_error: decoded frames lost with the write (per contributing role)
    // and dropped behind it.
    std::uint64_t discarded_frames{};
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
    // Drops the role's pending audio. Lossless: what its decoder still holds of
    // that stream goes with it (the decoder restarts before its next chunk), as
    // it does on reset_session_timeline().
    void cancel_pending(protocol::ChannelRole role);
    // Lossless playout, owner thread: the peer stopped the role's stream after
    // everything submitted so far. That audio plays out (including what the
    // decoder still holds), nothing waits for the role any more, and its next
    // chunk starts a new stream. True while the role has audio pending. Without
    // lossless playout it does nothing and returns false.
    bool end_stream(protocol::ChannelRole role);
    // Decoded frames, over all roles, that are still to be written.
    [[nodiscard]] std::uint64_t pending_frames() const;
    // Records a steady-clock interval during which the owner withheld peer
    // credit, or a completed sink write exceeded one PCM piece duration and
    // its scheduled end (excluding normal per-piece pacing). render_step()
    // advances its cursor without writing through the
    // already-elapsed part of an all-role gap inside such an interval; that
    // part is exempt from the 200 ms silence caps. Unexplained gaps, future
    // gaps and late arrivals keep the strict policy. Non-incremental pump()
    // ignores these observations.
    void note_backpressure(core::Nanoseconds start, core::Nanoseconds end);
    // Owner-thread: the playout re-anchors committed since the last call, in
    // order (at most 64 between calls).
    [[nodiscard]] std::vector<PlayoutEvent> take_playout_events();
    // True while any role plays on the playout clock (any thread).
    [[nodiscard]] bool playout_anchored() const;
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
    // Worker-only: clears pending runs and resets sample timeline placement across
    // session reconnects while keeping the opened sink and cumulative stats intact.
    void reset_session_timeline();
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
    [[nodiscard]] core::Result<void> flush(core::Milliseconds drain_timeout = kDrainTimeout);

    // Longest an EOS drain (flush(), close()) waits by default.
    static constexpr core::Milliseconds kDrainTimeout{2000};
    // Owner thread: later sink writes time out at `until` when that is before
    // RouterConfig::sink_write_timeout (a shutdown that has to end by then).
    void set_write_deadline(std::chrono::steady_clock::time_point until);

    void set_muted(bool muted) noexcept;
    [[nodiscard]] bool muted() const noexcept;
    void close(core::Milliseconds drain_timeout = kDrainTimeout) noexcept;

    [[nodiscard]] RouterState state() const noexcept;
    [[nodiscard]] const RouterReport& report() const noexcept;

    struct Impl;
    explicit AudioRouter(std::unique_ptr<Impl> impl) noexcept;

private:
    std::unique_ptr<Impl> impl_;
};

} // namespace aa::audio
