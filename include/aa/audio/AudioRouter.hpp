// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <aa/audio/Audio.hpp>
#include <aa/audio/AudioDecode.hpp>
#include <aa/audio/AudioMix.hpp>
#include <aa/core/Result.hpp>
#include <aa/core/Time.hpp>
#include <aa/protocol/Protocol.hpp>

#include <array>
#include <cstdint>
#include <memory>
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

struct RouterReport final {
    RouterState state{RouterState::active};
    SinkReport sink{};
    RouterStats stats{};
    SinkProbeStats sink_probe{};
    RenderBarrierStats render_barrier{};
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

    // Mixes and writes every pending frame to the routing element. A write
    // failure (bus error, render-barrier stall, drained route) transitions the
    // router to degraded with the cause recorded and fails typed
    // audio_sink_unavailable. Runs are scheduled on one persistent sample
    // timeline across pumps (first nonempty pump anchors the base; split and
    // batched pumps of one schedule produce identical output); a gap or
    // late-arrival discontinuity above the 200 ms safety caps is rejected
    // invalid_argument before any write (the batch is discarded, cursor and
    // counters unchanged). Output pieces are bounded to 64 KiB whole-frame.
    // Terminal flush state: after flush() has committed the route's EOS
    // drain, pump() fails invalid_argument before any sink access; while
    // degraded it fails audio_sink_unavailable.
    [[nodiscard]] core::Result<void> pump();
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
