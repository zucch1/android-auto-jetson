// SPDX-License-Identifier: GPL-3.0-or-later
// Task 25 four-channel AAP audio router: channel/session messages in,
// per-role decode, duck/mix, route to the default PipeWire output. Pure
// project logic - the GStreamer elements live in the adapter zone and are
// reached only through the private AudioSinkFactory seam.
//
// Gate fix round 2: mixing runs on a SAMPLE TIMELINE (every run start/end is a
// boundary; each interval mixes exactly the samples present in it, with
// ducking applied per interval), the receiver-input timestamp is recorded at
// submit() BEFORE decoding so decoder waits are inside the path latency, and
// any routing write failure degrades the router typed with a recorded cause.
#include <aa/audio/AudioRouter.hpp>

#include "AudioSinkFactory.hpp"
#include "RateCorrection.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <tuple>
#include <utility>
#include <vector>

namespace aa::audio {
namespace {

constexpr std::size_t kRoleCount = 4;
constexpr std::size_t kMaxAudioPayloadBytes = std::size_t{64} * 1024;
constexpr std::size_t kMaxPendingRunsPerRole = 512;
constexpr std::size_t kMaxOutputPieceBytes = std::size_t{64} * 1024;
constexpr std::size_t kMaxBackpressureIntervals = 64;
// Playout: a role silent this long past its slot resumed rather than underran.
constexpr std::int64_t kPlayoutResumeAfterNs = 1'000'000'000;
// Playout drift: a depth error is sustained once the window holds this much
// of arrivals (2 s, ~47 A26 chunks).
constexpr std::int64_t kPlayoutSustainNs = 2'000'000'000;
// After an underrun the phone's backlog arrives on top of the refill: judge it
// after 0.5 s of arrivals (within 4 s of the re-anchor) so it drains well
// inside the A/V contract's 5 s mean window (Neon P2-3).
constexpr std::int64_t kPlayoutRecoverySustainNs = 250'000'000;
constexpr std::int64_t kPlayoutRecoveryNs = 4'000'000'000;
constexpr std::int64_t kPlayoutRecoveryWindowNs = 500'000'000;
// Shed a backlog whose shallowest chunk is this far above the depth (350 ms
// at the 120 ms default), so the offset stays under the 400 ms video hold cap.
constexpr std::int64_t kPlayoutShedAboveNs = 230'000'000;
// ... but never above 350 ms (a 180 ms depth would put 410 ms above the cap).
constexpr std::int64_t kPlayoutShedCeilingNs = 350'000'000;
// Re-anchor chunks arriving within this of the re-anchor are one co-arriving
// group (live stamps differ by microseconds; Neon P2-3 round 4).
constexpr std::int64_t kPlayoutReanchorGroupNs = 10'000'000;
// Playout fails closed, like the arrival model's late cap, when a role's
// arrival clock runs backwards by more than this (N6).
constexpr std::int64_t kPlayoutBackwardCapNs = 200'000'000;
// Arrivals closer than this belong to one burst (max_arrival_burst).
constexpr std::int64_t kArrivalBurstNs = 10'000'000;

// Lossless playout (PlayoutPolicy::lossless).
// Rate correction: nothing within the dead band above the target depth, then
// proportional (a 20 ms excess reaches 0.5%), then up to 1% while the excess
// is between 100 and 200 ms or more. Below half the depth the buffer refills
// at up to 0.5%.
constexpr double kRateDeadBandMs = 10.0;
constexpr double kRatePpmPerMs = 250.0;
constexpr double kRatePpm = 5'000.0;
constexpr double kDeepExcessMs = 100.0;
constexpr double kDeepRatePpmPerMs = 50.0;
// A measured stopped reader can add much more backlog than an arrival burst.
// Media drains it at up to 2% (about a third of a semitone while resampled):
// 300 ms in about 15 s, 1.6 s in about 80 s. Guidance and system voice keep
// the ordinary 1% ceiling: pitch moves are easier to hear on speech. The
// wireless video hold cap (#232) covers the longer A/V drain.
// A device (Bluetooth) whose own delay alone exceeds the configured depth.
constexpr std::int64_t kMaxReaderDepthNs = 500'000'000;
constexpr unsigned kHoldRecoveryPercent = 2;
constexpr double kHoldRecoveryPpm = kHoldRecoveryPercent * 10'000.0;
constexpr double kHoldRecoveryDeadBandMs = 2.0;
constexpr double kHoldRecoveryPpmPerMs = 400.0;
// The rate itself moves by at most this much per second (0.1% per A26 chunk).
constexpr double kRateSlewPpmPerSecond = 25'000.0;
// Most correction carried from runs too short to take it, in frames.
constexpr double kMaxRateCarryFrames = 4.0;
// Depths of runs shorter than this share a window entry, so the window holds
// its 4 s whatever the packet size.
constexpr std::int64_t kDepthBucketNs = 10'000'000;
// A write that started this long after its slot on the sink's clock played late
// (below it, the slot query's jitter).
constexpr std::int64_t kLatePlaybackNs = 20'000'000;
constexpr std::int64_t kSameInstantNs = 1'000;
// A frame another playing role may still contribute to is mixed no earlier
// than this before its slot (half the depth when that is smaller). Wired
// arrival scheduling wrote a median 15 ms ahead of the slot with the sink on
// time (presented == scheduled at p99 in the 1800 s A26 runs of 2026-11-17), so
// 20 ms is enough.
constexpr std::int64_t kHoldLeadNs = 20'000'000;
// How much is mixed at a time once such a role is late.
constexpr std::int64_t kHoldQuantumNs = 10'000'000;
// A role silent this long no longer holds the others.
constexpr std::int64_t kHoldExpectNs = 1'000'000'000;
// Runs shorter than this share a write with the ones right behind them.
constexpr std::int64_t kJoinedWriteNs = 4'000'000;
// Input a resampler that holds nothing is first given: GStreamer's needs half its
// 64 taps, and one frame, before it gives any output.
constexpr std::size_t kMinDecodeFrames = 40;
constexpr std::uint64_t kMinDecodeNs = 2'500'000U; // a resampler to a lower rate takes more

[[nodiscard]] std::optional<Role> role_of(protocol::ChannelRole channel) {
    switch (channel) {
    case protocol::ChannelRole::media_audio: return Role::media;
    case protocol::ChannelRole::guidance_audio: return Role::guidance;
    case protocol::ChannelRole::system_audio: return Role::system;
    case protocol::ChannelRole::speech_audio: return Role::speech;
    default: return std::nullopt;
    }
}

[[nodiscard]] std::int64_t steady_now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// #60: a chunk starts playing no earlier than its handoff and no earlier than
// its slot on the sink's clock.
[[nodiscard]] std::optional<core::Nanoseconds> presented_at(
    std::optional<core::Nanoseconds> slot, std::int64_t completed_ns) {
    if (!slot) { return std::nullopt; }
    return core::Nanoseconds{std::max(slot->count, completed_ns)};
}

// How the playout policy places one run (see PlayoutPolicy).
struct PlayoutPlan final {
    enum class Mode : std::uint8_t { anchor, steady, underrun };
    Mode mode{Mode::anchor};
    // anchor/underrun: arrival + depth, where the run is placed.
    std::int64_t target_ns{};
    // Buffer depth at arrival: slot minus arrival (anchor/underrun: the depth
    // it is re-anchored with).
    std::int64_t depth_ns{};
    // Re-anchor (underrun or resume): how long after its slot the run arrived.
    std::int64_t late_ns{};
    // anchor: the role was anchored before (a resume, not a stream start).
    bool resume{false};
    // steady: bounded drift trim from the head, or silence inserted before it.
    std::uint64_t trim_frames{};
    std::uint64_t pad_frames{};
    // The role's content timestamps ran backwards: fail closed (N6).
    bool backward{false};
    // steady: the trim sheds a deep post-drought backlog fast.
    bool shed{false};
    // steady: absolute start frame (back-to-back slot plus pad), fixed when
    // planned so a cached pad is applied once, never again to an advancing
    // cursor (Neon P1-1).
    std::uint64_t start_frame{};
    // steady: the back-to-back slot the plan was made against.
    std::uint64_t slot_frame{};
    // Lossless steady: frames the run was shortened (negative) or lengthened
    // by to correct the playback rate; no sample is dropped or inserted.
    std::int64_t warped_frames{};
    // Lossless anchor: only the decoder held the role back; it still owes.
    bool decoder_held{false};
};

// Lossless, a resampling role: `frames` consecutive output frames whose source
// audio was received at `at_ns` (the first of them; each later one is one frame
// of content after it). The decoder returns its input's audio in order, so the
// role's output stream maps onto these spans in order too.
struct SourceSpan final {
    std::uint64_t frames{};
    std::int64_t at_ns{};
};

// One decoded run pending on the mix timeline. submitted_at_ns is stamped at
// submit() BEFORE decode() so the whole decode + mix + write path is inside
// the recorded latency (gate fix F4).
struct Run final {
    protocol::MessageHeader header{};
    core::Nanoseconds timestamp{};
    std::vector<std::byte> pcm{};
    std::uint64_t frames{};
    std::int64_t submitted_at_ns{};
    std::optional<std::uint64_t> placed{};
    std::uint64_t trimmed{};
    std::uint64_t consumed{};
    bool preflighted{};
    // The parent's own PCM duration in output frames, from its input byte
    // count at the role's input format (`frames` for a non-PCM codec). The
    // decoder's resampler may return only part of a parent now and the rest
    // at the head of a later run, so `frames` can be shorter or longer.
    std::uint64_t content_frames{};
    // Output beyond the parent's own duration (#154): the head of `pcm` is
    // carryover from earlier parents, at least this long, so its source
    // samples are at least carried_ns older than `timestamp`.
    std::int64_t carried_ns{};
    // Playout decision (PlayoutPolicy), fixed once the run heads its role:
    // every earlier run of the role is committed by then, so the decision no
    // longer depends on a dry-run plan.
    std::optional<PlayoutPlan> playout{};
    // Playout offset this run was committed with (WriteCompletion).
    std::int64_t playout_offset_ns{};
    // Decoded frames before any rate correction.
    std::uint64_t decoded_frames{};
    // First run of a stream the peer started after a stop (end_stream).
    bool stream_start{};
    // A decoder tail's content begins this long after `timestamp` (the parent
    // it belongs to).
    std::int64_t content_ahead_ns{};
    // Its input waits to be decoded (held_input): no frames yet.
    bool awaiting{};
    // Lossless, a resampling role: where the source audio of each part of `pcm`
    // was received, in order. The decoder can return one parent's audio at the
    // head of a later parent's run, so the run's own timestamp does not say how
    // old its head is (bot review of #229). Empty: `timestamp` plus
    // content_ahead_ns is the receipt of all of it.
    std::vector<SourceSpan> sources{};
};

// One run placed on the sample timeline at an effective (non-overlapping)
// frame range. Same-role runs are sequentialized in arrival order so no
// samples are dropped when consecutive channel chunks overlap.
// head_trim_frames is the live-PCM drift correction offset: that many decoded
// frames were dropped from the run's head and the retained PCM starts there.
struct ScheduledRun final {
    std::size_t role{0};
    std::uint64_t start_frame{0};
    std::uint64_t end_frame{0};
    std::uint64_t head_trim_frames{0};
    const Run* run{nullptr};
    std::optional<std::int64_t> intended{};
    std::uint64_t role_end_before{};
    // Part of head_trim_frames dropped by the catch-up (#154), counted when
    // the run is placed.
    std::uint64_t caught_up_frames{0};
    // The run's phase takes the catch-up trim (#154).
    bool catching_up{false};
    // Playout re-anchor: the already-elapsed frames [skip_first, skip_last)
    // before the run are skipped without a write instead of synthesized.
    std::uint64_t skip_first{0};
    std::uint64_t skip_last{0};
    // Playout re-anchor: the refill silence before the run (at most the
    // depth) is the buffer, not gap silence, and is exempt from the
    // aggregate cap (Neon P2-3).
    std::uint64_t refill_allowance{0};
    // Playout steady: the pad this placement applies (0 when the slot moved
    // past the planned start).
    std::uint64_t applied_pad_frames{0};
    // Lossless: part of head_trim_frames cut by the emergency bound.
    std::uint64_t overflow_frames{0};
};

// Drift trim of one unplaced run. The catch-up (#154) judges the run by the
// source time of its oldest sample when the run is already behind placement or
// overlaps an earlier parent on its role (`backlog_carry`): decoder carryover
// at its head is older than the parent's timestamp by at least carried_ns, so
// a coalesced burst run is trimmed through that stale audio too, and placing
// it can never carry the cursor more than the drift target past the end of its
// own parent's PCM. An on-time non-overlapping run (phase <= 0 and no source
// overlap) never lets carryover alone trigger catch-up. Runs the catch-up does
// not take keep the bounded trim on their raw phase.
void plan_drift_trim(const LivePcmDriftCorrector& drift, core::Nanoseconds phase,
                     const Run& run, bool backlog_carry, ScheduledRun& entry) noexcept {
    const std::int64_t effective_carried = backlog_carry ? run.carried_ns : 0;
    const core::Nanoseconds source_phase{
        phase.count > INT64_MAX - effective_carried ? INT64_MAX : phase.count + effective_carried};
    entry.catching_up = drift.catches_up(source_phase);
    const core::Nanoseconds judged = entry.catching_up ? source_phase : phase;
    entry.head_trim_frames = drift.trim_frames(judged, run.frames);
    entry.caught_up_frames = drift.caught_up_frames(judged, run.frames);
}

[[nodiscard]] std::uint64_t source_end_frame(std::int64_t intended,
                                             std::uint64_t content_frames) noexcept {
    if (intended < 0) {
        const auto neg = static_cast<std::uint64_t>(-(intended + 1)) + 1ULL;
        return content_frames > neg ? content_frames - neg : 0ULL;
    }
    const auto pos = static_cast<std::uint64_t>(intended);
    return content_frames > UINT64_MAX - pos ? UINT64_MAX : pos + content_frames;
}

// Late-arrival discontinuity check (200 ms cap). Raw lateness is how far
// the run's start lies behind the playback cursor. A caught-up run (#154) is
// judged by its parent packet's own PTS range against the committed source-time
// frontier (bounded by cursor): it fails only when the END of the parent's PCM
// (timestamp plus input duration) is more than 200 ms behind recent source
// audio - real lag such as a backward timestamp jump, not a forward burst
// following a non-caught-up 200 ms guidance parent placed near the watermark.
// Neither side moves with how the resampler splits or coalesces output: the
// input duration comes from the payload size, never from decoded length. Runs
// the catch-up does not take keep the raw check unchanged.
[[nodiscard]] bool late_beyond_cap(std::uint64_t cursor, std::uint64_t source_frontier,
                                   std::int64_t intended, const Run& run,
                                   const ScheduledRun& entry, std::uint64_t cap) noexcept {
    std::int64_t judged = intended;
    std::uint64_t reference = cursor;
    if (entry.catching_up) {
        const auto content = static_cast<std::int64_t>(
            std::min<std::uint64_t>(run.content_frames, static_cast<std::uint64_t>(INT64_MAX)));
        if (intended > INT64_MAX - content) { return false; }
        judged = intended + content;
        reference = std::min(cursor, source_frontier);
    }
    if (judged >= static_cast<std::int64_t>(reference)) { return false; }
    return static_cast<std::uint64_t>(static_cast<std::int64_t>(reference) - judged) > cap;
}

} // namespace

LivePcmDriftCorrector::LivePcmDriftCorrector(LivePcmDriftPolicy policy,
                                             std::uint32_t output_rate_hz) noexcept
    : policy_(policy), rate_(output_rate_hz) {}

// Trim = min(max_trim, phase - target) converted to whole frames (floor), then
// capped by the run itself. Only runs above the high watermark are touched;
// the residual lands near `target`, so a slower producer (negative phase) and
// a zero-drift stream never lose a frame.
std::uint64_t LivePcmDriftCorrector::trim_frames(core::Nanoseconds phase,
                                                 std::uint64_t run_frames) const noexcept {
    return trim(phase, run_frames, true);
}

std::uint64_t LivePcmDriftCorrector::caught_up_frames(core::Nanoseconds phase,
                                                      std::uint64_t run_frames) const noexcept {
    return trim(phase, run_frames, true) - trim(phase, run_frames, false);
}

bool LivePcmDriftCorrector::catches_up(core::Nanoseconds phase) const noexcept {
    return policy_.enabled && rate_ != 0 && phase > policy_.high_watermark
        && policy_.catch_up_watermark.count > 0 && phase > policy_.catch_up_watermark
        && phase > policy_.target;
}

std::uint64_t LivePcmDriftCorrector::trim(core::Nanoseconds phase, std::uint64_t run_frames,
                                          bool catch_up) const noexcept {
    if (!policy_.enabled || rate_ == 0 || phase <= policy_.high_watermark) {
        return 0;
    }
    const core::Nanoseconds excess{phase.count - policy_.target.count};
    // #154 catch-up: a run whose initial phase exceeds the catch-up watermark
    // loses the whole excess above target at once instead of max_trim
    // (bounded by the run below); the watermark gates the run, not each sample.
    const bool catching_up = catch_up && policy_.catch_up_watermark.count > 0
        && phase > policy_.catch_up_watermark;
    const core::Nanoseconds budget =
        catching_up || excess < policy_.max_trim ? excess : policy_.max_trim;
    if (budget.count <= 0) {
        return 0;
    }
    const std::uint64_t nanoseconds = static_cast<std::uint64_t>(budget.count);
    const std::uint64_t seconds = nanoseconds / 1'000'000'000ULL;
    if (seconds > UINT64_MAX / rate_) {
        return run_frames;
    }
    const std::uint64_t frames =
        seconds * rate_ + ((nanoseconds % 1'000'000'000ULL) * rate_) / 1'000'000'000ULL;
    return std::min(frames, run_frames);
}

SinkSelection select_sink_backend(const SinkReport& report, SinkBackend request) {
    SinkSelection selection;
    const SinkCandidate* chosen = nullptr;

    // #80: pipewiresink is refused outright. On the target (PipeWire 1.0.5,
    // Ubuntu noble) pipewiresink has no rate control and degrades after
    // 1.5-2.3 h, and no fixed PipeWire is packaged for noble. pulsesink via
    // pipewire-pulse is the default and only supported real-time route.
    constexpr std::string_view native_reason =
        "pipewiresink_unsupported: pipewiresink has no rate control on PipeWire 1.0.5 / "
        "Ubuntu noble and degrades after 1.5-2.3 h (no fixed PipeWire packaged for noble); "
        "pulsesink is the supported real-time route";
    if (request == SinkBackend::pipewire) {
        selection.cause = "disabled: " + std::string{native_reason};
        return selection;
    }
    if (request == SinkBackend::pulse || request == SinkBackend::automatic) {
        for (const auto& candidate : report.candidates) {
            if (candidate.element == "pulsesink" && candidate.viable) {
                chosen = &candidate;
                break;
            }
        }
    }

    if (chosen == nullptr) {
        selection.usable = false;
        selection.cause = request == SinkBackend::automatic
            ? "disabled: " + std::string{native_reason} : "disabled:";
        bool first = true;
        for (const auto& candidate : report.candidates) {
            const bool wanted = request == SinkBackend::automatic
                || (request == SinkBackend::pulse && candidate.element == "pulsesink");
            if (!wanted) {
                continue;
            }
            if (!first) {
                selection.cause += ";";
            }
            first = false;
            selection.cause += " " + candidate.element + " "
                + (candidate.element == "pipewiresink" && candidate.viable
                    ? "refused" : (candidate.available ? "not viable" : "absent"))
                + " (" + candidate.cause + ")";
        }
        return selection;
    }

    selection.element = chosen->element;
    selection.usable = true;
    if (request == SinkBackend::automatic) {
        selection.cause =
            "enabled: falling back to pulsesink; " + std::string{native_reason};
    } else {
        selection.cause = "enabled: " + chosen->element + " runtime-detected viable";
    }
    return selection;
}

struct AudioRouter::Impl final {
    RouterConfig config{};
    AudioMixer mixer{};
    std::array<std::unique_ptr<AudioDecoder>, kRoleCount> decoders{};
    // Test-only scripted decoder (open_router_with_decoder_for_test): when
    // set, it replaces every role's decoder so a test controls exactly how
    // output is split across and coalesced into runs.
    detail::TestDecodeFn test_decode{};
    std::unique_ptr<detail::ObservedAudioSink> sink{};
    RouterReport report{};
    bool closed{false};
    // Terminal route state (gate fix 6): set immediately before flush()
    // attempts the route's single EOS drain - a successful drain leaves the
    // route terminal (submit()/pump() fail invalid_argument before decode or
    // sink access) and a FAILED drain leaves it terminal AND degraded (the
    // typed audio_sink_unavailable refusal is retained). A pump/preflight
    // failure before the EOS drain does NOT set it.
    bool flushed{false};

    // Per-role decoded runs pending on the mix timeline.
    std::array<std::deque<Run>, kRoleCount> pending{};
    std::mutex pending_mutex{};
    std::array<std::uint64_t, kRoleCount> generations{};
    bool incremental{};
    std::vector<std::byte> silence_scratch{};
    LivePcmDriftCorrector drift{};

    // Persistent sample timeline across pumps: base timestamp anchored at the
    // FIRST nonempty pump, the playback cursor (frames already committed to
    // the sink), and each role's committed end. Split and batched pumps of the
    // same schedule therefore produce identical output.
    std::optional<core::Nanoseconds> timeline_base{};
    std::uint64_t playback_cursor_frames{0};
    std::array<std::uint64_t, kRoleCount> role_end_frames{};
    // Committed source-time frontier (max intended + content_frames across
    // placed/committed runs, advanced on elapsed skips) so late_beyond_cap and
    // carryover burst detection are not distorted by placement-phase inflation.
    std::uint64_t source_frontier_frames{0};
    std::array<std::uint64_t, kRoleCount> role_source_frontier_frames{};
    // Disjoint, sorted [start, end) steady-clock intervals during which the
    // owner withheld peer credit (note_backpressure). Empty for every caller
    // that never records one, which keeps the strict gap policy unchanged.
    std::vector<std::pair<std::int64_t, std::int64_t>> backpressure{};
    struct IdleAuthorization final {
        std::pair<std::int64_t, std::int64_t> interval;
        std::optional<std::uint64_t> prefix_end{};
    };
    std::optional<IdleAuthorization> idle{};

    // Playout state per role (PlayoutPolicy): anchored once the role's first
    // run is committed; depths holds (arrival, buffer depth) of the committed
    // runs inside the window, adjusted by every later trim or pad so it stays
    // relative to the current schedule.
    struct DepthEntry final {
        std::int64_t at;
        std::int64_t low;
        std::int64_t high;
    };
    struct PlayoutRole final {
        bool anchored{false};
        std::deque<DepthEntry> depths{};
        // Current playout offset of the role (see commit_playout).
        std::int64_t offset_ns{};
        // Arrival of the role's last underrun re-anchor, if any.
        std::optional<std::int64_t> recovering_since{};
        // Latest committed arrival (backward-jump check).
        std::optional<std::int64_t> last_arrival{};
        // Lossless: current rate correction (ppm; positive drains) and its
        // fraction of a frame not yet applied.
        double rate_ppm{};
        double rate_carry{};
        bool recovering_from_hold{};
        // Lossless: output of already committed parents the decoder still
        // holds (a resampler split), in output frames.
        std::uint64_t decoder_owed{};
    };
    // Re-anchors, resumes, underruns and retimes since the last
    // take_playout_events().
    static constexpr std::size_t kMaxPlayoutEvents = 64;
    std::vector<PlayoutEvent> playout_events{};
    std::array<PlayoutRole, kRoleCount> playout{};
    struct ArrivalBurst final {
        std::optional<std::int64_t> last{};
        std::uint64_t length{};
    };
    std::array<ArrivalBurst, kRoleCount> arrival_bursts{};
    LivePcmDriftCorrector playout_drift{};

    [[nodiscard]] bool playout_enabled() const noexcept { return config.playout.enabled; }
    [[nodiscard]] bool lossless() const noexcept {
        return config.playout.enabled && config.playout.lossless;
    }
    [[nodiscard]] std::int64_t target_depth() const {
        if (!lossless() || !sink) { return config.playout.depth.count; }
        return std::clamp(sink->reader_depth().count, config.playout.depth.count, kMaxReaderDepthNs);
    }
    // Depth a stream is placed at: the steady depth plus room for arrival
    // jitter, which the window floor measures and drains to afterwards.
    [[nodiscard]] std::int64_t anchor_depth() const {
        if (!lossless() || !sink || sink->reader_depth().count == 0) { return config.playout.depth.count; }
        return std::min(std::max(config.playout.depth.count,
                                 sink->reader_depth().count + sink->arrival_margin().count), kMaxReaderDepthNs);
    }
    // pump()/flush(): everything pending is written, nothing is held.
    bool draining{};
    // The render_step() in progress has just retimed the timeline.
    bool retimed{};
    // Used when the sink cannot distinguish a stopped read cursor from a
    // dry late write. The write can begin and block ahead of its own slot.
    struct LastWrite {
        std::int64_t slot_end;
        std::int64_t entered;
        std::int64_t returned;
    };
    std::optional<LastWrite> last_write{};
    // Newest arrival submitted on any role.
    std::int64_t newest_arrival{INT64_MIN};

    [[nodiscard]] std::int64_t now_ns() const {
        return config.clock ? config.clock().count : steady_now_ns();
    }

    // Frames of committed parents the decoder still owes once `run` is
    // committed. A re-anchored role (or a decoder that has caught up) owes
    // nothing older.
    [[nodiscard]] static std::uint64_t owed_after(std::uint64_t owed, const Run& run) noexcept {
        const auto due = owed > UINT64_MAX - run.content_frames ? UINT64_MAX : owed + run.content_frames;
        return due > run.decoded_frames ? due - run.decoded_frames : 0;
    }

    // Wanted playback-rate correction (ppm; positive drains, negative
    // refills) for a role whose window floor is `floor_ns` deep.
    [[nodiscard]] double wanted_rate_ppm(std::int64_t floor_ns) const noexcept {
        const double depth_ms = static_cast<double>(target_depth()) / 1e6;
        const double floor_ms = static_cast<double>(floor_ns) / 1e6;
        const double excess_ms = floor_ms - depth_ms;
        if (excess_ms > kRateDeadBandMs) {
            return std::min(kRatePpm, (excess_ms - kRateDeadBandMs) * kRatePpmPerMs)
                + std::clamp((excess_ms - kDeepExcessMs) * kDeepRatePpmPerMs, 0.0, kRatePpm);
        }
        // Spent down to half the depth a jitter spike costs nothing.
        const double deficit_ms = depth_ms / 2.0 - floor_ms;
        return deficit_ms > 0.0 ? -std::min(kRatePpm, deficit_ms * kRatePpmPerMs) : 0.0;
    }

    // Lossless steady run, about to head its role: warps the decoded PCM by
    // the role's rate correction. Never drops or inserts a sample.
    void apply_rate(std::size_t role, Run& run, PlayoutPlan& plan) {
        auto& state = playout[role];
        const auto floor = window_floor(role, run.timestamp.count, plan.depth_ns);
        double backlog_rate = floor.sustained ? wanted_rate_ppm(floor.low) : 0.0;
        const auto excess_ms = static_cast<double>(floor.low - target_depth()) / 1e6;
        if (state.recovering_from_hold && floor.sustained) {
            if (excess_ms <= kHoldRecoveryDeadBandMs && state.rate_ppm <= 2 * kRatePpm) {
                state.recovering_from_hold = false;
            } else {
                backlog_rate = std::max(backlog_rate, std::clamp(
                    (excess_ms - kHoldRecoveryDeadBandMs) * kHoldRecoveryPpmPerMs, 0.0, kHoldRecoveryPpm));
            }
        }
        const unsigned percent = state.recovering_from_hold ? kHoldRecoveryPercent : 1U;
        const double ceiling = static_cast<double>(percent) * 10'000.0;
        const double wanted = std::clamp(backlog_rate + (sink ? sink->read_clock_skew_ppm() : 0.0),
                                         -ceiling, ceiling);
        const double slew = kRateSlewPpmPerSecond * static_cast<double>(run.frames)
            / static_cast<double>(config.output_format.sample_rate_hz);
        state.rate_ppm += std::clamp(wanted - state.rate_ppm, -slew, slew);
        if (sink && !sink->compression_safe()) {
            state.rate_ppm = std::min(0.0, state.rate_ppm);
            state.rate_carry = std::min(0.0, state.rate_carry);
        }
        state.rate_carry += static_cast<double>(run.frames) * state.rate_ppm / 1e6;
        const auto limit = detail::max_warp_frames(run.frames, percent);
        const auto removed = std::clamp(static_cast<std::int64_t>(state.rate_carry), -limit, limit);
        state.rate_carry = std::clamp(state.rate_carry - static_cast<double>(removed),
                                      -kMaxRateCarryFrames, kMaxRateCarryFrames);
        if (removed == 0) { return; }
        run.pcm = detail::warp_frames(run.pcm, config.output_format.channels, -removed, percent);
        run.frames = static_cast<std::uint64_t>(static_cast<std::int64_t>(run.frames) - removed);
        plan.warped_frames = -removed;
    }

    // End (output clock) and end offset of the last reported write
    // (WriteCompletion): the reported offset is slewed from it.
    std::optional<std::pair<std::int64_t, std::int64_t>> last_reported{};

    void reset_playout(std::size_t role) noexcept {
        playout[role] = PlayoutRole{};
        arrival_bursts[role] = ArrivalBurst{};
        if (lossless()) {
            decoder_stale[role] = decoder_stale[role] || std::exchange(decoder_used[role], false);
            stream_ended[role] = false;
            last_header[role].reset();
            held_input[role].clear();
            held_content[role] = 0;
            clear_sources(role);
        }
    }

    // Smallest depth of the committed runs inside the window ending at
    // `arrival`, folded with this run's own depth: the buffer the latest
    // arrival had. With grouped delivery it is a group's first (late) chunk,
    // so the group's spread never reads as drift (DeepSeek F1, N2).
    // `sustained` is false until the window holds kPlayoutSustainNs of
    // arrivals. While the role recovers from an underrun the window is only
    // the last 0.5 s and 0.25 s of it sustains, so the one late re-anchor chunk cannot
    // hold the backlog's drain back for the whole 4 s window (Neon P2-3).
    struct DepthFloor final {
        std::int64_t low;
        bool sustained;
    };
    [[nodiscard]] DepthFloor window_floor(std::size_t role, std::int64_t arrival,
                                          std::int64_t depth) const noexcept {
        const auto& state = playout[role];
        const bool recovering = state.recovering_since && arrival - *state.recovering_since <= kPlayoutRecoveryNs;
        const auto span = recovering ? kPlayoutRecoveryWindowNs : config.playout.window.count;
        const auto needed = recovering ? kPlayoutRecoverySustainNs : kPlayoutSustainNs;
        std::int64_t low = depth;
        std::int64_t oldest = arrival;
        for (const auto& [at, value, high] : state.depths) {
            (void)high;
            if (at < arrival - span) { continue; }
            // While recovering, the re-anchor chunk itself says nothing: it
            // sits at the depth by construction (P1-2: it held the floor down
            // for a second before a deep backlog could be shed).
            if (recovering && at >= *state.recovering_since
                && at - *state.recovering_since < kPlayoutReanchorGroupNs) { continue; }
            low = std::min(low, value);
            oldest = std::min(oldest, at);
        }
        return {low, arrival - oldest >= needed};
    }

    // Largest committed depth inside the window ending at `arrival` (folded
    // with this run's): the offset estimate (commit_playout).
    [[nodiscard]] std::int64_t window_max(std::size_t role, std::int64_t arrival,
                                          std::int64_t depth) const noexcept {
        const auto& state = playout[role];
        const bool recovering = state.recovering_since && arrival - *state.recovering_since <= kPlayoutRecoveryNs;
        const auto span = recovering ? kPlayoutRecoveryWindowNs : config.playout.window.count;
        std::int64_t high = depth;
        for (const auto& entry : state.depths) {
            if (entry.at >= arrival - span) { high = std::max(high, entry.high); }
        }
        return high;
    }

    [[nodiscard]] std::int64_t shed_threshold() const noexcept {
        return std::min(config.playout.depth.count + kPlayoutShedAboveNs, kPlayoutShedCeilingNs);
    }

    // Playout decision for an unplaced run whose back-to-back slot is
    // `slot_frame`. False when a time cannot be represented (schedule
    // rejection, like every other conversion failure).
    // `own_frame` is the role's own content end (its committed end plus what the
    // decoder still owes); the slot is never before the shared cursor.
    [[nodiscard]] bool plan_playout(std::size_t role, const Run& run, std::uint64_t slot_frame,
                                    std::uint64_t own_frame, std::int64_t base_ns, bool anchored,
                                    PlayoutPlan& plan) const {
        const std::int64_t arrival = run.timestamp.count;
        const std::int64_t depth = anchor_depth();
        std::int64_t slot_ns = 0;
        if (!frame_time(slot_frame, base_ns, slot_ns)) { return false; }
        plan = PlayoutPlan{};
        const auto& state = playout[role];
        if (anchored && state.last_arrival && arrival < *state.last_arrival - kPlayoutBackwardCapNs) {
            plan.backward = true;
            return true;
        }
        // Lossless: a role starves when its own stream ran out, also while
        // another role kept the shared cursor ahead of the arrival. A stream
        // that only the decoder held back (a resampler split) did not: it goes
        // on at its own content slot (at most `depth` after the arrival, so the
        // hole is never gap silence), not as an underrun.
        auto due_ns = slot_ns;
        if (lossless() && anchored) {
            std::int64_t own_ns = 0;
            if (!frame_time(own_frame, base_ns, own_ns)) { return false; }
            if (slot_ns < arrival && own_ns >= arrival) {
                if (arrival > INT64_MAX - depth) { return false; }
                plan.target_ns = std::min(own_ns, arrival + depth);
                plan.depth_ns = plan.target_ns - arrival;
                plan.decoder_held = true;
                return true;
            }
            if (slot_ns >= arrival) { due_ns = std::min(due_ns, own_ns); }
        }
        if (!anchored || due_ns < arrival) {
            // A stream that resumes more than kPlayoutResumeAfter after its
            // slot paused (no stop message) rather than starved: a fresh
            // anchor, not an underrun.
            const bool underrun = anchored && arrival - due_ns <= kPlayoutResumeAfterNs;
            plan.mode = underrun ? PlayoutPlan::Mode::underrun : PlayoutPlan::Mode::anchor;
            plan.resume = anchored && !underrun;
            plan.late_ns = std::max<std::int64_t>(0, arrival - due_ns);
            if (arrival > INT64_MAX - depth) { return false; }
            plan.target_ns = arrival + depth;
            plan.depth_ns = std::max(plan.target_ns, slot_ns) - arrival;
            return true;
        }
        plan.mode = PlayoutPlan::Mode::steady;
        plan.depth_ns = slot_ns - arrival;
        if (lossless()) {
            // The depth is free to grow and is drained by apply_rate(); only
            // a backlog beyond the emergency bound is cut (overflow_beyond).
            plan.start_frame = slot_frame;
            plan.slot_frame = slot_frame;
            return true;
        }
        // Drift acts on sustained error of the buffer the latest arrival had
        // (DeepSeek F1, N2, windows 10/11). A group's chunks share one arrival
        // and are staggered by slot, so single depths spread by the group;
        // judging the window's smallest depth keeps that spread out of both
        // directions. Trim when even the latest arrival is more than the high
        // watermark too deep (phone clock faster, a backlog). Pad only when it
        // is thinner than a quarter of the depth (phone clock slower, a slower
        // path): a jitter spike inside the buffer must cost nothing, so the
        // buffer is spent down to that floor before it is refilled.
        const auto floor = window_floor(role, arrival, plan.depth_ns);
        const auto pad_floor = depth / 4;
        // The shed protects the video hold cap, which bounds the offset - the
        // window's largest depth, a group span above the floor with grouped
        // delivery (DeepSeek N1) - so it is gated on that, and only while the
        // floor itself is over-full (shedding never thins the latest chunk's
        // buffer below the trim threshold).
        const auto over_full = depth + config.live_pcm_drift.high_watermark.count;
        const bool recovering = state.recovering_since
            && arrival - *state.recovering_since <= kPlayoutRecoveryNs;
        // After an underrun the backlog that follows is expected: the chunks
        // arriving deep are the ones to shed, and the shallow ones of the
        // backlog's ramp have already played, so the current chunk's own
        // buffer is the margin to keep. Otherwise the window's floor is.
        const bool shed_margin = recovering ? plan.depth_ns > over_full
                                            : floor.sustained && floor.low > over_full;
        if (shed_margin && window_max(role, arrival, plan.depth_ns) > shed_threshold()) {
            // A backlog this deep (after a drought) is shed fast, 30% of each
            // chunk, so audio comes back under the 400 ms video hold cap
            // within about a second instead of dwelling above it at 5 ms per
            // chunk (Neon P1-2, DeepSeek R1). Shedding f of a chunk lowers
            // the offset by f while the output advances 1 - f, and the
            // reported offset may fall by half of that: f <= 1/3 keeps the
            // offset the video follows equal to the delay audio plays with.
            plan.trim_frames = run.frames * 3 / 10;
            plan.shed = true;
        } else if (floor.sustained && floor.low > depth) {
            plan.trim_frames = playout_drift.trim_frames(core::Nanoseconds{floor.low - depth}, run.frames);
        } else if (floor.sustained && floor.low < pad_floor) {
            // Refill toward half the depth, max_trim at a time (R6).
            const auto deficit = depth / 2 - floor.low;
            plan.pad_frames = ns_to_frames(core::Nanoseconds{
                std::min(deficit, config.live_pcm_drift.max_trim.count)});
        }
        plan.start_frame = slot_frame + plan.pad_frames;
        plan.slot_frame = slot_frame;
        return true;
    }

    // Lossless: how many of `frames` frames of the run, from `content_frame` on,
    // the emergency bound cuts when the first of them is placed at `start_frame`:
    // what would play more than emergency_backlog after its content time (the
    // run's arrival plus the frames before it). Decided for the placement itself,
    // so it covers every way a run gets there: steady, a fresh stream queued
    // behind the one before, a retime after the run was planned. False when a
    // time cannot be represented.
    [[nodiscard]] bool overflow_beyond(const Run& run, std::uint64_t start_frame, std::int64_t base_ns,
                                       std::uint64_t content_frame, std::uint64_t frames,
                                       std::uint64_t& cut) const {
        std::int64_t start_ns = 0;
        if (!frame_time(start_frame, base_ns, start_ns)) { return false; }
        cut = 0;
        const auto bound = config.playout.emergency_backlog.count;
        if (run.sources.empty()) {
            const auto age = start_ns - run.timestamp.count - run.content_ahead_ns
                - frames_to_ns(content_frame).count;
            if (age > bound) { cut = std::min(frames, ns_to_frames(core::Nanoseconds{age - bound})); }
            return true;
        }
        // Each part goes by its own source's receipt: the cut ends at the first
        // frame received no more than the bound before it plays at start_ns
        // (cutting a frame moves every later one a frame earlier).
        if (start_ns < INT64_MIN + bound) { return true; }
        const auto oldest = start_ns - bound;
        std::uint64_t at = 0;
        for (std::size_t i = 0; i < run.sources.size(); ++i) {
            const auto& span = run.sources[i];
            const bool last = i + 1 == run.sources.size();
            const auto end = last ? UINT64_MAX : at + span.frames;
            if (end > content_frame) {
                const auto from = std::max(at, content_frame);
                const auto received = span.at_ns + frames_to_ns(from - at).count;
                const auto skip = received >= oldest ? 0 : ns_to_frames(core::Nanoseconds{oldest - received});
                if (skip < end - from) {
                    cut = std::min(frames, from + skip - content_frame);
                    return true;
                }
            }
            at = end;
        }
        cut = frames;
        return true;
    }

    static void shift_depths(PlayoutRole& state, std::int64_t by) noexcept {
        for (auto& entry : state.depths) {
            entry.low += by;
            entry.high += by;
        }
    }

    // Adds a steady run's depth to its role's window. Lossless: short runs
    // share an entry (their smallest and largest depth).
    void note_depth(PlayoutRole& state, const Run& run, std::int64_t depth) const {
        const auto at = run.timestamp.count;
        if (lossless() && !state.depths.empty()) {
            auto& last = state.depths.back();
            const bool reanchor = state.recovering_since && last.at == *state.recovering_since;
            if (!reanchor && at >= last.at && at - last.at < kDepthBucketNs
                && frames_to_ns(run.decoded_frames).count < kDepthBucketNs) {
                last.low = std::min(last.low, depth);
                last.high = std::max(last.high, depth);
                return;
            }
        }
        state.depths.push_back({at, depth, depth});
    }

    // Apply a clock correction to everything still to come. A discrete
    // retime can age queued PCM past the emergency bound in one step.
    void shift_timeline(std::int64_t by, bool enforce_backlog) {
        timeline_base = core::Nanoseconds{timeline_base->count + by};
        for (std::size_t role = 0; role < kRoleCount; ++role) {
            auto& state = playout[role];
            if (!state.anchored) { continue; }
            shift_depths(state, by);
            state.offset_ns += by;
            for (auto& run : pending[role]) {
                if (run.playout && run.playout->mode == PlayoutPlan::Mode::steady) {
                    run.playout->depth_ns += by;
                }
                if (!run.placed) { continue; }
                run.playout_offset_ns += by;
                if (!enforce_backlog) { continue; }
                // The rest of a run already placed (render_step() decides the
                // others again).
                const auto used = run.trimmed + run.consumed;
                std::uint64_t cut = 0;
                if (!overflow_beyond(run, *run.placed + run.consumed, timeline_base->count, used,
                                     run.frames - used, cut) || cut == 0) { continue; }
                run.trimmed += cut;
                const auto cut_ns = frames_to_ns(cut).count;
                run.playout_offset_ns -= cut_ns;
                shift_depths(state, -cut_ns);
                state.offset_ns -= cut_ns;
                report.stats.frames_trimmed += cut;
                ++report.stats.playout_overflows;
                report.stats.frames_overflowed += cut;
            }
        }
    }

    void retime(std::int64_t by, std::int64_t held = 0) {
        shift_timeline(by, by > 0);
        if (held > 0) {
            if (auto& media = playout[role_index(Role::media)]; media.anchored) {
                media.recovering_from_hold = true;
            }
        }
        auto& stats = report.stats;
        ++stats.playout_retimes;
        stats.playout_retimed.count += by;
        std::int64_t offset = 0;
        for (std::size_t role = 0; role < kRoleCount; ++role) {
            if (playing(role)) { offset = std::max(offset, playout[role].offset_ns); }
        }
        stats.playout_offset = core::Nanoseconds{offset};
        if (playout_events.size() < kMaxPlayoutEvents) {
            playout_events.push_back({PlayoutEvent::Kind::retime, core::Nanoseconds{by},
                                      core::Nanoseconds{offset}, Role::media, core::Nanoseconds{held}});
        }
    }

    [[nodiscard]] std::uint64_t frames_pending() const noexcept {
        std::uint64_t frames = 0;
        for (const auto& queue : pending) {
            for (const auto& run : queue) { frames += run.frames - run.trimmed - run.consumed; }
        }
        return frames;
    }

    // Decoders whose buffered output belongs to a cancelled stream: restarted
    // before their next decode (lossless; see AudioRouter::cancel_pending).
    std::array<bool, kRoleCount> decoder_stale{};
    // Decoders that decoded since they were opened, restarted or drained.
    std::array<bool, kRoleCount> decoder_used{};
    // The peer stopped the role's stream: what is pending plays out, and the
    // next run submitted starts a new one (end_stream).
    std::array<bool, kRoleCount> stream_ended{};
    std::array<std::optional<protocol::MessageHeader>, kRoleCount> last_header{};
    // Content duration of that parent.
    std::array<std::int64_t, kRoleCount> last_content_ns{};

    // Lossless, a resampling role: input that is not decoded yet, and the output
    // frames of the part of it whose runs are done already.
    std::array<std::vector<std::byte>, kRoleCount> held_input{};
    std::array<std::uint64_t, kRoleCount> held_content{};
    // Lossless, a resampling role: where the content of every parent submitted
    // and not yet returned by the decoder was received, oldest first (held
    // input, what the decoder keeps back, and parents retired without output
    // alike), and the end of the last span handed out (for output beyond them).
    std::array<std::deque<SourceSpan>, kRoleCount> held_sources{};
    std::array<std::optional<std::int64_t>, kRoleCount> sources_end{};

    void clear_sources(std::size_t role) noexcept {
        held_sources[role].clear();
        sources_end[role].reset();
    }

    void hold_source(std::size_t role, std::uint64_t frames, std::int64_t at_ns) {
        if (frames != 0) { held_sources[role].push_back({frames, at_ns}); }
    }

    // Undoes hold_source for the newest `frames` (their input was refused).
    void unhold_sources(std::size_t role, std::uint64_t frames) noexcept {
        auto& held = held_sources[role];
        while (frames != 0 && !held.empty()) {
            const auto take = std::min(frames, held.back().frames);
            held.back().frames -= take;
            frames -= take;
            if (held.back().frames == 0) { held.pop_back(); }
        }
    }

    // The source spans of the role's next `frames` output frames. Output beyond
    // what was held (rounding) continues from the last span. Empty for a role
    // that holds no sources.
    [[nodiscard]] std::vector<SourceSpan> take_sources(std::size_t role, std::uint64_t frames) {
        std::vector<SourceSpan> spans;
        auto& held = held_sources[role];
        if (frames == 0 || (held.empty() && !sources_end[role])) { return spans; }
        while (frames != 0 && !held.empty()) {
            auto& front = held.front();
            const auto take = std::min(frames, front.frames);
            spans.push_back({take, front.at_ns});
            const auto taken_ns = frames_to_ns(take).count;
            front.at_ns = front.at_ns > INT64_MAX - taken_ns ? INT64_MAX : front.at_ns + taken_ns;
            sources_end[role] = front.at_ns;
            front.frames -= take;
            frames -= take;
            if (front.frames == 0) { held.pop_front(); }
        }
        if (frames != 0) {
            const auto at = *sources_end[role];
            spans.push_back({frames, at});
            const auto taken_ns = frames_to_ns(frames).count;
            sources_end[role] = at > INT64_MAX - taken_ns ? INT64_MAX : at + taken_ns;
        }
        return spans;
    }

    [[nodiscard]] static std::size_t min_decode_frames(const AudioFormat& input) noexcept {
        return std::max(kMinDecodeFrames,
                        static_cast<std::size_t>(kMinDecodeNs * input.sample_rate_hz / 1'000'000'000U));
    }

    // Gives the runs at the end of the role's queue that wait for their output their
    // share of it. What the decoder kept back is missing from the first of them, and
    // what it had kept back before is at the first one's head, as if each had been
    // decoded alone. A run left without output counts towards the next one's content.
    void share_output(std::size_t role, std::span<const std::byte> pcm) {
        auto& queue = pending[role];
        const auto frame_bytes = static_cast<std::size_t>(config.output_format.channels) * sizeof(std::int16_t);
        const auto frames = static_cast<std::uint64_t>(pcm.size() / frame_bytes);
        auto first = queue.size();
        std::uint64_t content = held_content[role];
        while (first != 0 && queue[first - 1].awaiting) { content += queue[--first].content_frames; }
        std::uint64_t due = std::exchange(held_content[role], 0);
        std::uint64_t given = 0;
        std::uint64_t unplayed = due;
        bool starts = false;
        for (auto i = first; i < queue.size(); ++i) {
            auto& run = queue[i];
            due += run.content_frames;
            const auto upto = due + frames > content ? due + frames - content : 0;
            run.awaiting = false;
            run.frames = upto - given;
            run.decoded_frames = run.frames;
            run.content_frames += std::exchange(unplayed, 0);
            run.stream_start = run.stream_start || std::exchange(starts, false);
            const auto part = pcm.subspan(static_cast<std::size_t>(given) * frame_bytes,
                                          static_cast<std::size_t>(run.frames) * frame_bytes);
            run.pcm.assign(part.begin(), part.end());
            // The output is the held input's audio in order, whichever parent it
            // lands on: its sources go with it.
            run.sources = take_sources(role, run.frames);
            given = upto;
            if (run.frames == 0) {
                unplayed = std::exchange(run.content_frames, 0);
                starts = std::exchange(run.stream_start, false);
            } else if (run.frames > run.content_frames) {
                run.carried_ns = frames_to_ns(run.frames - run.content_frames).count;
            }
        }
        report.stats.frames_decoded += frames;
    }

    // Takes a run without output of its own (the decoder has all of it) off the
    // head of the role's queue. Its receipt stays in held_sources, so the run
    // that gets its audio later is judged by it.
    void finish_silent_run(std::size_t role) {
        auto& queue = pending[role];
        const bool starts = queue.front().stream_start;
        held_content[role] += queue.front().content_frames;
        queue.pop_front();
        if (!starts) { return; }
        if (queue.empty()) { stream_ended[role] = true; } else { queue.front().stream_start = true; }
    }

    // Appends what the role's decoder still holds to the role's pending audio
    // and restarts the decoder. Nothing once the role's own stream has run dry:
    // a resampler tail (about 2 ms) alone after a gap is a click, not sound. Returns
    // what that drops. Input that was never decoded is not such a tail: it is
    // audio of its own that has not played (a stream of packets each too short
    // to resample), placed like any other (bot review of #229).
    AudioRouter::StreamEnd drain_decoder(std::size_t role) {
        auto& state = playout[role];
        state.decoder_owed = 0;
        const auto input = std::exchange(held_input[role], {});
        const bool used = std::exchange(decoder_used[role], false);
        if (!decoders[role] || decoder_stale[role] || (!used && input.empty())) {
            clear_sources(role);
            return {};
        }
        const bool unplayed = !input.empty();
        const auto frame_bytes = static_cast<std::size_t>(config.output_format.channels) * sizeof(std::int16_t);
        auto tail = decoders[role]->drain(input);
        const auto frames = static_cast<std::uint64_t>(tail.size() / frame_bytes);
        // The decoder starts afresh: nothing it held is still to come.
        const auto finish = [&] { clear_sources(role); };
        if (frames == 0 || !last_header[role]) {
            held_content[role] = 0;
            finish();
            return {};
        }
        if (!pending[role].empty() && pending[role].back().awaiting) {
            share_output(role, tail);
            finish();
            return {};
        }
        held_content[role] = 0;
        auto sources = take_sources(role, frames);
        finish();
        if (!pending[role].empty()) {
            auto& run = pending[role].back();
            if (!run.sources.empty()) {
                run.sources.insert(run.sources.end(), sources.begin(), sources.end());
            }
            run.pcm.insert(run.pcm.end(), tail.begin(), tail.end());
            run.frames += frames;
            run.decoded_frames += frames;
        } else {
            std::int64_t end_ns = INT64_MAX;
            // Nothing of the stream is placed: every packet was too short to give output.
            const bool fresh = !state.anchored || stream_ended[role];
            if (fresh) {
                const auto at = last_header[role]->timestamp.count;
                const auto depth = anchor_depth();
                if (at <= INT64_MAX - depth) { end_ns = at + depth; }
            } else if (!timeline_base || !frame_time(role_end_frames[role], timeline_base->count, end_ns)) {
                end_ns = INT64_MIN;
            }
            // Only a residual tail is late here; unplayed audio meets the
            // emergency bound like the rest, by its own receipt.
            if (!unplayed && end_ns < now_ns()) { return {false, frames, core::Nanoseconds{end_ns}}; }
            Run run;
            run.header = *last_header[role];
            run.timestamp = run.header.timestamp;
            run.pcm = std::move(tail);
            run.frames = frames;
            run.decoded_frames = frames;
            run.submitted_at_ns = now_ns();
            run.content_ahead_ns = fresh ? 0 : last_content_ns[role];
            run.sources = std::move(sources);
            run.stream_start = fresh && std::exchange(stream_ended[role], false);
            pending[role].push_back(std::move(run));
        }
        report.stats.frames_decoded += frames;
        return {};
    }

    // Commits a placed run's playout decision: role state, window and stats.
    void commit_playout(std::size_t role, Run& run, const ScheduledRun& entry, std::int64_t base_ns) {
        if (!playout_enabled() || !run.playout) { return; }
        const auto& plan = *run.playout;
        auto& state = playout[role];
        auto& stats = report.stats;
        std::int64_t start_ns = 0;
        if (!frame_time(entry.start_frame, base_ns, start_ns)) { start_ns = run.timestamp.count; }
        // A run cut whole is still noted no deeper than the bound: nothing plays
        // beyond it.
        if (entry.overflow_frames != 0) {
            ++stats.playout_overflows;
            stats.frames_overflowed += entry.overflow_frames;
        }
        if (plan.mode == PlayoutPlan::Mode::steady) {
            // How much later than its planned slot the run is placed (its pad,
            // or a cursor another role moved on) shifts every later depth (N7).
            const auto shifted = entry.applied_pad_frames;
            const auto pad = std::min(shifted, plan.pad_frames);
            const auto warped = plan.warped_frames >= 0
                ? frames_to_ns(static_cast<std::uint64_t>(plan.warped_frames)).count
                : -frames_to_ns(static_cast<std::uint64_t>(-plan.warped_frames)).count;
            const auto adjust = frames_to_ns(shifted).count
                - frames_to_ns(entry.head_trim_frames).count + warped;
            shift_depths(state, adjust);
            note_depth(state, run, entry.overflow_frames != 0
                ? std::min(plan.depth_ns + adjust, config.playout.emergency_backlog.count)
                : plan.depth_ns + adjust);
            stats.frames_padded += pad;
            if (plan.shed) { stats.frames_shed += entry.head_trim_frames; }
            if (plan.warped_frames < 0) {
                stats.frames_compressed += static_cast<std::uint64_t>(-plan.warped_frames);
                if (role != role_index(Role::media)) {
                    stats.frames_compressed_voice += static_cast<std::uint64_t>(-plan.warped_frames);
                }
            } else {
                stats.frames_stretched += static_cast<std::uint64_t>(plan.warped_frames);
            }
            state.decoder_owed = owed_after(state.decoder_owed, run);
            stats.playout_depth_min = stats.playout_depth_min
                ? std::min(*stats.playout_depth_min, core::Nanoseconds{plan.depth_ns})
                : core::Nanoseconds{plan.depth_ns};
            stats.playout_depth_max = stats.playout_depth_max
                ? std::max(*stats.playout_depth_max, core::Nanoseconds{plan.depth_ns})
                : core::Nanoseconds{plan.depth_ns};
        } else {
            state.depths.clear();
            state.rate_ppm = 0.0;
            state.rate_carry = 0.0;
            state.recovering_from_hold = false;
            state.decoder_owed = owed_after(plan.decoder_held ? state.decoder_owed : 0, run);
            auto depth = start_ns - run.timestamp.count - frames_to_ns(entry.overflow_frames).count;
            if (entry.overflow_frames != 0) { depth = std::min(depth, config.playout.emergency_backlog.count); }
            state.depths.push_back({run.timestamp.count, depth, depth});
            PlayoutEvent event{PlayoutEvent::Kind::start, core::Nanoseconds{plan.late_ns},
                               core::Nanoseconds{depth}, static_cast<Role>(role)};
            if (plan.mode == PlayoutPlan::Mode::underrun) {
                ++stats.playout_underruns;
                stats.playout_underrun_late = core::Nanoseconds{plan.late_ns};
                state.recovering_since = run.timestamp.count;
                event.kind = PlayoutEvent::Kind::underrun;
            } else {
                ++stats.playout_anchors;
                if (plan.resume) {
                    ++stats.playout_resumes;
                    event.kind = PlayoutEvent::Kind::resume;
                }
                stats.playout_anchor_skip = core::Nanoseconds{plan.late_ns};
                state.recovering_since.reset();
            }
            if (playout_events.size() < kMaxPlayoutEvents) { playout_events.push_back(event); }
        }
        // Playout offset: the depth of the least-delayed recent arrival, i.e.
        // how long after the content (arrival) clock the role now plays. A
        // phone releasing chunks in groups delivers the group's last chunk
        // on time and the others late, so the earliest arrival is the
        // content clock (A/V contract); after a re-anchor it is the anchor's
        // depth until later arrivals show more.
        {
            std::int64_t best = 0;
            const auto window_from = run.timestamp.count - config.playout.window.count;
            for (const auto& entry : state.depths) {
                if (entry.at >= window_from) { best = std::max(best, entry.high); }
            }
            run.playout_offset_ns = best;
            state.offset_ns = best;
            // A burst can keep the offset above the threshold even when
            // the buffer minimum leaves no room for another shed.
            if (const auto excess = best - shed_threshold(); excess > 0) {
                ++stats.playout_shed_residual_chunks;
                stats.playout_shed_residual_peak =
                    std::max(stats.playout_shed_residual_peak, core::Nanoseconds{excess});
            }
        }
        // While recovering the floor reads only the last 0.5 s; entries it
        // skips are dropped, so trims applied to them meanwhile can never
        // bring them back below the floor when the window widens again
        // (Neon P2-3).
        const bool recovering = state.recovering_since
            && run.timestamp.count - *state.recovering_since <= kPlayoutRecoveryNs;
        const auto from = run.timestamp.count
            - (recovering ? kPlayoutRecoveryWindowNs : config.playout.window.count);
        while (!state.depths.empty() && state.depths.front().at < from) { state.depths.pop_front(); }
        constexpr std::size_t kMaxPlayoutDepths = 512;
        while (state.depths.size() > kMaxPlayoutDepths) { state.depths.pop_front(); }
        state.anchored = true;
        state.last_arrival = std::max(run.timestamp.count, state.last_arrival.value_or(run.timestamp.count));
        stats.playout_depth = core::Nanoseconds{plan.depth_ns};
        // The receiver's offset: the largest over its playing roles (a write
        // mixing roles reports the largest of its contributors).
        std::int64_t offset = 0;
        for (std::size_t other = 0; other < kRoleCount; ++other) {
            if (playing(other)) { offset = std::max(offset, playout[other].offset_ns); }
        }
        if (const auto media = media_offset(start_ns)) { offset = *media; }
        stats.playout_offset = core::Nanoseconds{offset};
    }

    // Anchored, and not a stopped stream (end_stream) that has played out.
    [[nodiscard]] bool playing(std::size_t role) const noexcept {
        return playout[role].anchored && !(stream_ended[role] && pending[role].empty());
    }

    // Lossless: the offset of the media stream, the one video is in sync with,
    // while it plays at output time `at_ns`. A prompt placed behind it (a
    // resampler split) must not pull the video hold away from the music.
    [[nodiscard]] std::optional<std::int64_t> media_offset(std::int64_t at_ns) const noexcept {
        const auto& media = playout[role_index(Role::media)];
        if (!lossless() || !playing(role_index(Role::media)) || !media.last_arrival
            || at_ns - *media.last_arrival - media.offset_ns > kHoldExpectNs) { return std::nullopt; }
        return media.offset_ns;
    }

    // Lossless: true when another role that is playing has not delivered its
    // next chunk yet, so a frame mixed now would leave it behind.
    [[nodiscard]] bool awaiting_role(std::int64_t at_ns) const noexcept {
        for (std::size_t role = 0; role < kRoleCount; ++role) {
            const auto& state = playout[role];
            if (playing(role) && pending[role].empty() && state.last_arrival
                && at_ns - *state.last_arrival < kHoldExpectNs) { return true; }
        }
        return false;
    }

    struct ElapsedRange final {
        std::uint64_t first;
        std::uint64_t last;
        bool idle;
    };

    struct TimelineClock final {
        std::int64_t base_ns;
        std::int64_t now_ns;
    };

    // Frame range [first, last) of one recorded interval on the timeline,
    // clipped to `now` so only already-elapsed time is ever skippable.
    [[nodiscard]] bool backpressure_frames(const std::pair<std::int64_t, std::int64_t>& interval,
                                           TimelineClock clock,
                                           std::uint64_t& first, std::uint64_t& last) const {
        const auto start_ns = std::max(interval.first, clock.base_ns);
        const auto end_ns = std::min(interval.second, clock.now_ns);
        std::int64_t start_frame = 0;
        std::int64_t end_frame = 0;
        if (start_ns >= end_ns
            || !ns_to_frames_signed(core::Nanoseconds{start_ns - clock.base_ns}, start_frame)
            || !ns_to_frames_signed(core::Nanoseconds{end_ns - clock.base_ns}, end_frame)) {
            return false;
        }
        std::int64_t start_frame_ns = 0;
        if (!frame_time(static_cast<std::uint64_t>(start_frame), clock.base_ns, start_frame_ns)) {
            return false;
        }
        if (start_frame_ns < start_ns) { ++start_frame; }
        if (end_frame <= start_frame) { return false; }
        first = static_cast<std::uint64_t>(start_frame);
        last = static_cast<std::uint64_t>(end_frame);
        return true;
    }

    [[nodiscard]] std::vector<ElapsedRange> elapsed_ranges(TimelineClock clock,
                                                          std::uint64_t prefix_end) const {
        std::vector<ElapsedRange> ranges;
        for (const auto& interval : backpressure) {
            std::uint64_t first = 0;
            std::uint64_t last = 0;
            if (backpressure_frames(interval, clock, first, last)) {
                ranges.push_back({first, last, false});
            }
        }
        std::uint64_t first = 0;
        std::uint64_t last = 0;
        if (idle && backpressure_frames(idle->interval, clock, first, last)) {
            last = std::min(last, prefix_end);
            // Backpressure owns overlap. Split the idle range around it so
            // preflight and advancement use the same disjoint, reason-tagged union.
            const auto backpressure_count = ranges.size();
            for (std::size_t i = 0; i < backpressure_count && first < last; ++i) {
                const auto range = ranges[i];
                if (range.last <= first) { continue; }
                if (range.first >= last) { break; }
                if (range.first > first) { ranges.push_back({first, range.first, true}); }
                first = std::max(first, range.last);
            }
            if (first < last) { ranges.push_back({first, last, true}); }
        }
        std::sort(ranges.begin(), ranges.end(), [](const auto& left, const auto& right) {
            return left.first < right.first;
        });
        return ranges;
    }

    void mark_degraded(std::string cause) {
        report.state = RouterState::degraded;
        report.sink.usable = false;
        // The degrade cause always wins the recorded cause: it is why the
        // route is gone (gate fix F3 - the cause must record the loss, not
        // keep the stale startup "enabled" text).
        report.sink.cause = std::move(cause);
    }

    [[nodiscard]] core::Nanoseconds frames_to_ns(std::uint64_t frames) const {
        std::int64_t out = 0;
        if (!frames_to_ns_checked(frames, out)) {
            return core::Nanoseconds{0};
        }
        return core::Nanoseconds{out};
    }

    // floor(frames * 1e9 / rate) via quotient/remainder (frames * 1e9
    // overflows int64 on long timelines). False when the result is not
    // representable. EVERY step is checked BEFORE the overflowing
    // expression is evaluated: the quotient product, the tail product and
    // their final sum (gate round: the unchecked remainder addition wrapped
    // frame 442721857769989 at 48 kHz to a large NEGATIVE delta).
    [[nodiscard]] bool frames_to_ns_checked(std::uint64_t frames, std::int64_t& out) const {
        const auto rate = static_cast<std::uint64_t>(config.output_format.sample_rate_hz);
        const std::uint64_t q = frames / rate;
        const std::uint64_t r = frames % rate;
        if (q > static_cast<std::uint64_t>(INT64_MAX / 1'000'000'000LL)) {
            return false;
        }
        if (r > static_cast<std::uint64_t>(INT64_MAX / 1'000'000'000LL)) {
            return false;
        }
        const std::int64_t seconds_ns = static_cast<std::int64_t>(q) * 1'000'000'000LL;
        const std::int64_t tail_ns =
            static_cast<std::int64_t>((r * 1'000'000'000LL) / rate);
        if (tail_ns > INT64_MAX - seconds_ns) {
            return false;
        }
        out = seconds_ns + tail_ns;
        return true;
    }

    [[nodiscard]] std::uint64_t ns_to_frames(core::Nanoseconds ns) const {
        std::int64_t out = 0;
        if (!ns_to_frames_signed(ns, out) || out < 0) {
            return 0;
        }
        return static_cast<std::uint64_t>(out);
    }

    // floor(ns * rate / 1e9) with floor semantics for negative displacements
    // (quotient/remainder; ns * rate overflows int64 at large timestamps).
    // False when the result is not representable: the quotient product, the
    // tail product and their final sum are each checked BEFORE evaluation
    // (gate round: this conversion always returned true through unchecked
    // signed multiply/add), and callers treat failure as schedule rejection.
    [[nodiscard]] bool ns_to_frames_signed(core::Nanoseconds ns, std::int64_t& out) const {
        const auto rate = static_cast<std::int64_t>(config.output_format.sample_rate_hz);
        if (rate <= 0) {
            return false;
        }
        std::int64_t q = ns.count / 1'000'000'000LL;
        std::int64_t r = ns.count % 1'000'000'000LL;
        if (r < 0) {
            q -= 1;
            r += 1'000'000'000LL;
        }
        if (q > INT64_MAX / rate || q < INT64_MIN / rate) {
            return false;
        }
        if (r > INT64_MAX / rate) {
            return false;
        }
        const std::int64_t head_ns = q * rate;
        const std::int64_t tail_ns = (r * rate) / 1'000'000'000LL;
        if (tail_ns > 0 && head_ns > INT64_MAX - tail_ns) {
            return false;
        }
        out = head_ns + tail_ns;
        return true;
    }

    // Effective scheduled timestamp time(f) = base + floor(f * 1e9 / rate),
    // computed from the base every time (never accumulated rounded adds).
    [[nodiscard]] bool frame_time(std::uint64_t frame, std::int64_t base_ns, // NOLINT(bugprone-easily-swappable-parameters): frame index and signed clock base have distinct units
                                  std::int64_t& out) const {
        std::int64_t delta = 0;
        if (!frames_to_ns_checked(frame, delta)) {
            return false;
        }
        if (base_ns > INT64_MAX - delta) {
            return false;
        }
        out = base_ns + delta;
        return true;
    }
};

namespace {

core::Result<AudioRouter> open_router_with(
    const RouterConfig& config, std::unique_ptr<detail::ObservedAudioSink> injected,
    detail::SinkOpenOutcome (*opener)(const detail::SinkOpenRequest&) = detail::open_default_sink,
    detail::TestDecodeFn test_decode = {}) {
    if (config.output_format.bits_per_sample != 16 || config.output_format.channels == 0
        || config.output_format.sample_rate_hz == 0
        || config.output_format.sample_rate_hz > static_cast<std::uint32_t>(INT32_MAX)) {
        return core::Result<AudioRouter>{Error{ErrorCode::audio_format_unsupported}};
    }

    // Router codec contract: this router accepts PCM and AAC-LC ADTS; raw
    // AAC-LC (no ADTS framing) is rejected at open with
    // audio_format_unsupported. RoleStreamConfig carries no AudioSpecificConfig
    // and the decoder requires EncodedChunk::codec_data for raw AAC-LC, so a
    // raw AAC-LC role cannot be honored here - rejected BEFORE any decoder or
    // sink is opened. The standalone decoder's raw-AAC capability is unchanged.
    for (const RoleStreamConfig& role : config.roles) {
        if (role.codec == Codec::aac_lc) {
            return core::Result<AudioRouter>{Error{ErrorCode::audio_format_unsupported}};
        }
    }

    auto impl = std::make_unique<AudioRouter::Impl>();
    impl->config = config;
    impl->drift = LivePcmDriftCorrector{config.live_pcm_drift,
                                        config.output_format.sample_rate_hz};
    if (config.playout.enabled) {
        // R8: without the drift policy the playout clock would follow the
        // phone's clock without bound; playout requires it.
        if (config.playout.depth.count <= 0 || config.playout.depth > kMaxPlayoutDepth
            || config.playout.window.count <= 0 || !config.live_pcm_drift.enabled
            || (config.playout.lossless && config.playout.emergency_backlog < kMinEmergencyBacklog)) {
            return core::Result<AudioRouter>{Error{ErrorCode::invalid_argument}};
        }
        // Playout keeps its trims bounded: never a catch-up.
        auto bounded = config.live_pcm_drift;
        bounded.catch_up_watermark = core::Nanoseconds{0};
        impl->playout_drift = LivePcmDriftCorrector{bounded, config.output_format.sample_rate_hz};
    }
    impl->mixer = AudioMixer{config.mix};
    impl->report.sink.requested = config.backend;
    impl->test_decode = std::move(test_decode);

    for (std::size_t i = 0; i < kRoleCount; ++i) {
        AudioDecoder::Config decode_config;
        decode_config.codec = config.roles[i].codec;
        decode_config.input_format = config.roles[i].input_format;
        decode_config.output_format = config.output_format;
        auto decoder = AudioDecoder::open(decode_config);
        if (!decoder.has_value()) {
            return core::Result<AudioRouter>{decoder.error()};
        }
        impl->decoders[i] = std::make_unique<AudioDecoder>(std::move(decoder).value());
    }

    if (injected) {
        impl->report.sink.usable = true;
        impl->report.sink.selected_element = "test-recording-sink";
        impl->report.sink.cause = "enabled: test recording sink";
        impl->sink = std::move(injected);
    } else {
        detail::SinkOpenRequest request;
        request.format = config.output_format;
        request.backend = config.backend;
        request.target = config.sink_remote;
        request.start_timeout = config.sink_start_timeout;
        request.write_timeout = config.sink_write_timeout;
        request.drop_nonempty_at_ingress = config.drop_nonempty_at_ingress;
        request.drop_nonempty_after_window = config.drop_nonempty_after_window;
        request.on_write_progress = config.on_sink_write_progress;
        request.contiguous_stream = config.playout.enabled && config.playout.lossless;
        detail::SinkOpenOutcome outcome = opener(request);

        impl->report.sink = outcome.report;
        if (!outcome.sink) {
            // No usable route: the router opens DEGRADED with the recorded
            // cause. Every submit()/pump() then fails typed - never silence
            // pretending success, never a crash.
            impl->mark_degraded(outcome.report.cause.empty()
                                    ? "disabled: no PipeWire-capable sink element usable"
                                    : outcome.report.cause);
        } else {
            impl->sink = std::move(outcome.sink);
        }
    }

    return core::Result<AudioRouter>{AudioRouter{std::move(impl)}};
}

} // namespace

core::Result<AudioRouter> AudioRouter::open(const RouterConfig& config) {
    return open_router_with(config, nullptr);
}

namespace detail {

core::Result<AudioRouter> open_router_with_sink_for_test(const RouterConfig& config,
                                                         std::unique_ptr<ObservedAudioSink> sink) {
    return open_router_with(config, std::move(sink));
}

core::Result<AudioRouter> open_router_with_decoder_for_test(const RouterConfig& config,
                                                            std::unique_ptr<ObservedAudioSink> sink,
                                                            TestDecodeFn decode) {
    return open_router_with(config, std::move(sink), detail::open_default_sink, std::move(decode));
}

[[nodiscard]] SinkOpenOutcome open_native_sink_for_test(const SinkOpenRequest& request);

core::Result<AudioRouter> open_router_with_native_sink_for_test(const RouterConfig& config) {
    return open_router_with(config, nullptr, open_native_sink_for_test);
}

} // namespace detail

AudioRouter::AudioRouter(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

AudioRouter::~AudioRouter() = default;

AudioRouter::AudioRouter(AudioRouter&& other) noexcept = default;

AudioRouter& AudioRouter::operator=(AudioRouter&& other) noexcept = default;

core::Result<void> AudioRouter::submit(const protocol::MessageView& message) {
    return submit(message, core::Nanoseconds{steady_now_ns()});
}

core::Result<void> AudioRouter::submit(const protocol::MessageView& message,
                                      core::Nanoseconds receiver_input_at, bool more) {
    if (impl_) { impl_->report.queue_rejection.reset(); }
    if (!impl_ || impl_->closed) {
        return Error{ErrorCode::invalid_argument};
    }
    if (impl_->report.state == RouterState::degraded) {
        return Error{ErrorCode::audio_sink_unavailable};
    }
    if (impl_->flushed) {
        return Error{ErrorCode::invalid_argument};
    }
    const std::optional<Role> role = role_of(message.header.channel);
    if (!role.has_value()) {
        if (message.header.channel == protocol::ChannelRole::microphone) {
            return Error{ErrorCode::audio_mic_disabled};
        }
        return Error{ErrorCode::invalid_argument};
    }
    if (message.payload.empty() || message.payload.size() > kMaxAudioPayloadBytes) {
        return Error{ErrorCode::invalid_argument};
    }
    if (message.header.timestamp.count < 0) {
        return Error{ErrorCode::invalid_argument};
    }
    const std::size_t index = role_index(*role);
    std::lock_guard lock(impl_->pending_mutex);
    if (impl_->pending[index].size() >= kMaxPendingRunsPerRole) {
        impl_->report.queue_rejection = diagnostics::QueueRejection{
            diagnostics::QueueSource::audio_router, diagnostics::QueueReason::pending_runs,
            message.header.channel, impl_->pending[index].size(), kMaxPendingRunsPerRole,
            0, 0, 0, 0, message.payload.size()};
        return Error{ErrorCode::ipc_queue_full};
    }

    // Includes owner-side copy/queue wait as well as decoder and render waits.
    const std::int64_t submitted_at_ns = receiver_input_at.count;

    EncodedChunk chunk;
    chunk.role = *role;
    chunk.codec = impl_->config.roles[index].codec;
    chunk.input_format = impl_->config.roles[index].input_format;
    chunk.sequence = message.header.sequence;
    chunk.timestamp = message.header.timestamp;
    chunk.payload = message.payload;

    Run run;
    const std::uint64_t input_frame_bytes =
        static_cast<std::uint64_t>(chunk.input_format.channels) * (chunk.input_format.bits_per_sample / 8U);
    // Lossless: a packet does not have to be long enough to be decoded on its own.
    const bool buffered = impl_->lossless() && !impl_->test_decode && chunk.codec == Codec::pcm
        && chunk.input_format != impl_->config.output_format
        && input_frame_bytes != 0 && chunk.input_format.sample_rate_hz != 0;
    if (impl_->test_decode) {
        auto scripted = impl_->test_decode(chunk);
        if (!scripted.has_value()) {
            return scripted.error();
        }
        run.pcm = std::move(scripted).value();
    } else if (impl_->playout_enabled() && chunk.codec == Codec::pcm
               && chunk.input_format == impl_->config.output_format) {
        // Playout (wireless): PCM already in the output format is an
        // identity conversion; skipping the GStreamer push/pull round trip
        // keeps the render worker's lock short while a post-stall burst is
        // queued (window 13: video releases waited ~85-200 ms behind it).
        run.pcm.assign(message.payload.begin(), message.payload.end());
    } else {
        if (impl_->decoder_stale[index]) {
            // Output the resampler still holds of a cancelled stream.
            if (auto restarted = impl_->decoders[index]->restart(); !restarted) {
                return restarted.error();
            }
            impl_->clear_sources(index);
        }
        impl_->decoder_stale[index] = false;
        if (!buffered) {
            impl_->decoder_used[index] = true;
            auto decoded = impl_->decoders[index]->decode(chunk);
            if (!decoded.has_value()) {
                return decoded.error();
            }
            run.pcm.assign(decoded.value().pcm.begin(), decoded.value().pcm.end());
        }
    }
    run.header = message.header;
    run.timestamp = message.header.timestamp;
    const std::uint64_t frame_bytes =
        static_cast<std::uint64_t>(impl_->config.output_format.channels) * sizeof(std::int16_t);
    run.frames = frame_bytes == 0 ? 0 : run.pcm.size() / frame_bytes;
    run.content_frames = run.frames;
    run.decoded_frames = run.frames;
    if (chunk.codec == Codec::pcm) {
        const auto& input = chunk.input_format;
        if (input_frame_bytes != 0 && input.sample_rate_hz != 0) {
            // Bounded by kMaxAudioPayloadBytes: no overflow at any supported rate.
            const std::uint64_t input_frames = message.payload.size() / input_frame_bytes;
            run.content_frames =
                input_frames * impl_->config.output_format.sample_rate_hz / input.sample_rate_hz;
        }
    }
    if (run.frames > run.content_frames) {
        // At most 64 KiB of input per parent: frames * 1e9 cannot overflow.
        run.carried_ns = static_cast<std::int64_t>(
            (run.frames - run.content_frames) * 1'000'000'000ULL
            / impl_->config.output_format.sample_rate_hz);
    }
    run.submitted_at_ns = submitted_at_ns;
    run.awaiting = buffered;
    if (run.frames == 0 && !buffered) {
        return Error{ErrorCode::audio_format_unsupported};
    }
    if (buffered) {
        auto& held = impl_->held_input[index];
        held.insert(held.end(), message.payload.begin(), message.payload.end());
        impl_->hold_source(index, run.content_frames, run.timestamp.count);
    }

    run.stream_start = std::exchange(impl_->stream_ended[index], false);
    impl_->last_header[index] = message.header;
    impl_->last_content_ns[index] = impl_->frames_to_ns(run.content_frames).count;
    impl_->report.stats.frames_decoded += run.frames;
    impl_->newest_arrival = std::max(impl_->newest_arrival, run.timestamp.count);
    {
        // Chunks arriving together on one channel: how many the phone had in
        // flight at once (its credit use; our acks go out on receipt).
        auto& burst = impl_->arrival_bursts[index];
        const auto delta = burst.last ? run.timestamp.count - *burst.last : kArrivalBurstNs;
        burst.length = delta >= 0 && delta < kArrivalBurstNs ? burst.length + 1 : 1;
        burst.last = run.timestamp.count;
        impl_->report.stats.max_arrival_burst =
            std::max(impl_->report.stats.max_arrival_burst, burst.length);
    }
    impl_->pending[index].push_back(std::move(run));
    impl_->report.stats.chunks_submitted += 1;
    if (buffered && !more) {
        const auto& held = impl_->held_input[index];
        const auto held_frames = held.size() / input_frame_bytes;
        // Part of a frame is the decoder's to refuse, like any malformed packet.
        if (impl_->decoder_used[index] || held_frames >= Impl::min_decode_frames(chunk.input_format)
            || held.size() % input_frame_bytes != 0) {
            const auto input = std::exchange(impl_->held_input[index], {});
            chunk.payload = input;
            impl_->decoder_used[index] = true;
            auto decoded = impl_->decoders[index]->decode(chunk);
            if (!decoded.has_value()) {
                auto& queue = impl_->pending[index];
                std::uint64_t refused = 0;
                while (!queue.empty() && queue.back().awaiting) {
                    refused += queue.back().content_frames;
                    queue.pop_back();
                }
                impl_->unhold_sources(index, refused);
                return decoded.error();
            }
            impl_->share_output(index, decoded.value().pcm);
        }
    }
    return {};
}

core::Result<void> AudioRouter::pump() {
    if (impl_ && impl_->incremental) {
        impl_->draining = true;
        for (;;) {
            auto step = render_step();
            if (!step || !step.value().pending) {
                impl_->draining = false;
                if (!step) { return step.error(); }
                return {};
            }
        }
    }
    if (!impl_ || impl_->closed) {
        return Error{ErrorCode::invalid_argument};
    }
    if (impl_->report.state == RouterState::degraded) {
        return Error{ErrorCode::audio_sink_unavailable};
    }
    if (impl_->flushed) {
        return Error{ErrorCode::invalid_argument};
    }
    if (!impl_->sink) {
        impl_->mark_degraded("disabled: routing sink missing");
        return Error{ErrorCode::audio_sink_unavailable};
    }

    std::lock_guard lock(impl_->pending_mutex);

    // Gate fix F2 + persistent-cursor fix: mix on a PERSISTENT SAMPLE
    // TIMELINE. The base anchors at the FIRST nonempty pump and the playback
    // cursor + per-role ends carry across pumps, so split and batched pumps of
    // one schedule produce identical output. Every run occupies a frame
    // range; boundaries split the sweep; uncovered intervals ahead of the
    // cursor become bounded silence; every piece carries its EFFECTIVE
    // scheduled timestamp time(f) = B + floor(f*1e9/R).
    std::optional<core::Nanoseconds> base = impl_->timeline_base;
    if (!base.has_value()) {
        for (const auto& queue : impl_->pending) {
            for (const Run& run : queue) {
                if (!base.has_value() || run.timestamp < *base) {
                    base = run.timestamp;
                }
            }
        }
    }
    if (!base.has_value()) {
        impl_->report.sink_probe = impl_->sink->probe_stats();
        impl_->report.render_barrier = impl_->sink->render_stats();
        return {};
    }
    const std::int64_t base_ns = base->count;
    const std::uint64_t cursor = impl_->playback_cursor_frames;

    // Discontinuity policy: 200 ms safety caps (tied to the receiver latency
    // budget, NOT negotiated limits) on each uncovered gap, the total
    // synthesized silence per pump, and late-arrival displacement.
    std::int64_t cap_signed = 0;
    if (!impl_->ns_to_frames_signed(core::Nanoseconds{200'000'000LL}, cap_signed)) {
        return Error{ErrorCode::invalid_argument};
    }
    const std::uint64_t cap_frames = static_cast<std::uint64_t>(cap_signed);
    const std::size_t frame_bytes = static_cast<std::size_t>(
        impl_->config.output_format.channels * sizeof(std::int16_t));

    std::vector<ScheduledRun> scheduled;
    std::array<std::uint64_t, kRoleCount> new_role_ends = impl_->role_end_frames;
    std::array<std::uint64_t, kRoleCount> new_role_source_frontiers = impl_->role_source_frontier_frames;
    std::uint64_t planned_source_frontier = impl_->source_frontier_frames;
    std::uint64_t planned_retained = 0;
    std::uint64_t planned_trimmed = 0;
    std::uint64_t planned_caught_up = 0;
    bool schedule_ok = true;
    for (std::size_t i = 0; i < kRoleCount; ++i) {
        std::uint64_t role_end = impl_->role_end_frames[i];
        std::uint64_t role_source_frontier = impl_->role_source_frontier_frames[i];
        for (const Run& run : impl_->pending[i]) {
            std::int64_t intended = 0;
            if (!impl_->ns_to_frames_signed(core::Nanoseconds{run.timestamp.count - base_ns},
                                            intended)) {
                schedule_ok = false;
                continue;
            }
            ScheduledRun entry;
            entry.role = i;
            entry.intended = intended;
            const std::int64_t placed = std::max<int64_t>(
                intended, static_cast<std::int64_t>(std::max(cursor, role_end)));
            entry.start_frame = static_cast<std::uint64_t>(placed);
            std::int64_t planned_ns = 0;
            if (!impl_->frame_time(entry.start_frame, base_ns, planned_ns)) {
                schedule_ok = false;
                continue;
            }
            // Live-PCM drift correction (policy-gated; default off): the
            // phase is the planned start minus the run's ORIGINAL scheduled
            // timestamp, measured before the run is committed to the sink.
            // Over-watermark runs lose bounded head frames and the timeline
            // advances by the retained frames only.
            const core::Nanoseconds phase{planned_ns - run.timestamp.count};
            const bool backlog_carry =
                phase.count > 0 || intended < 0
                || static_cast<std::uint64_t>(intended) < role_source_frontier;
            plan_drift_trim(impl_->drift, phase, run, backlog_carry, entry);
            if (late_beyond_cap(cursor, impl_->source_frontier_frames, intended, run, entry,
                                cap_frames)) {
                schedule_ok = false;
            }
            const std::uint64_t retained = run.frames - entry.head_trim_frames;
            if (retained > UINT64_MAX - entry.start_frame) {
                schedule_ok = false;
                continue;
            }
            entry.end_frame = entry.start_frame + retained;
            std::int64_t end_ns = 0;
            if (!impl_->frame_time(entry.end_frame, base_ns, end_ns)) {
                schedule_ok = false;
                continue;
            }
            role_end = entry.end_frame;
            new_role_ends[i] = entry.end_frame;
            const std::uint64_t run_source_end = source_end_frame(intended, run.content_frames);
            role_source_frontier = std::max(role_source_frontier, run_source_end);
            new_role_source_frontiers[i] = role_source_frontier;
            planned_source_frontier = std::max(planned_source_frontier, run_source_end);
            entry.run = &run;
            planned_retained += retained;
            planned_trimmed += entry.head_trim_frames;
            planned_caught_up += entry.caught_up_frames;
            scheduled.push_back(entry);
        }
    }
    if (!schedule_ok) {
        // Rejected discontinuity: the batch is discarded and no timeline
        // state, mixer state, sink state or counters change.
        for (auto& queue : impl_->pending) {
            queue.clear();
        }
        return Error{ErrorCode::invalid_argument};
    }
    if (scheduled.empty()) {
        impl_->report.sink_probe = impl_->sink->probe_stats();
        impl_->report.render_barrier = impl_->sink->render_stats();
        return {};
    }

    std::uint64_t timeline_frames = 0;
    for (const ScheduledRun& entry : scheduled) {
        timeline_frames = std::max(timeline_frames, entry.end_frame);
    }

    // Boundaries: every run start and end (frame-accurate, deduplicated).
    std::vector<std::uint64_t> boundaries;
    boundaries.reserve(scheduled.size() * 2 + 2);
    boundaries.push_back(cursor);
    for (const ScheduledRun& entry : scheduled) {
        boundaries.push_back(entry.start_frame);
        boundaries.push_back(entry.end_frame);
    }
    boundaries.push_back(timeline_frames);
    std::sort(boundaries.begin(), boundaries.end());
    boundaries.erase(std::unique(boundaries.begin(), boundaries.end()), boundaries.end());

    // Preflight the ENTIRE schedule before any mix or write: uncovered
    // intervals are gaps (another role may cover an apparent gap), each gap
    // and the pump's total synthesized silence must fit the 200 ms cap.
    std::uint64_t total_gap = 0;
    for (std::size_t b = 0; b + 1 < boundaries.size(); ++b) {
        const std::uint64_t start = boundaries[b];
        const std::uint64_t end = boundaries[b + 1];
        if (end <= start) {
            continue;
        }
        bool covered = false;
        for (const ScheduledRun& entry : scheduled) {
            if (entry.start_frame < end && entry.end_frame > start) {
                covered = true;
                break;
            }
        }
        if (!covered) {
            const std::uint64_t gap = end - start;
            if (gap > cap_frames || total_gap > cap_frames - gap) {
                schedule_ok = false;
                break;
            }
            total_gap += gap;
        }
    }
    if (!schedule_ok) {
        // Rejected discontinuity: the batch is discarded and no timeline
        // state, mixer state, sink state or counters change.
        for (auto& queue : impl_->pending) {
            queue.clear();
        }
        return Error{ErrorCode::invalid_argument};
    }

    for (std::size_t b = 0; b + 1 < boundaries.size(); ++b) {
        const std::uint64_t start = boundaries[b];
        const std::uint64_t end = boundaries[b + 1];
        if (end <= start) {
            continue;
        }
        const std::uint64_t frames = end - start;
        std::int64_t interval_ns = 0;
        if (!impl_->frame_time(start, base_ns, interval_ns)) {
            return Error{ErrorCode::invalid_argument};
        }

        std::array<const PcmChunk*, kRoleCount> pieces{};
        std::array<PcmChunk, kRoleCount> materialized{};
        std::int64_t oldest_submit_ns = 0;
        bool first = true;

        for (const ScheduledRun& entry : scheduled) {
            if (entry.start_frame >= end || entry.end_frame <= start) {
                continue;
            }
            // A boundary-split interval always lies inside one run's range
            // for this role slot (same-role runs do not overlap), so the
            // slice covers the whole interval. The retained PCM starts at the
            // run's head-trim offset inside the decoded buffer.
            const std::uint64_t offset_frames =
                entry.head_trim_frames + (start - entry.start_frame);
            const std::size_t byte_offset =
                static_cast<std::size_t>(offset_frames) * frame_bytes;
            const std::size_t byte_count = static_cast<std::size_t>(frames) * frame_bytes;
            materialized[entry.role].role = static_cast<Role>(entry.role);
            materialized[entry.role].format = impl_->config.output_format;
            materialized[entry.role].timestamp = core::Nanoseconds{interval_ns};
            materialized[entry.role].pcm =
                std::span<const std::byte>{entry.run->pcm}.subspan(byte_offset, byte_count);
            pieces[entry.role] = &materialized[entry.role];
            if (first || entry.run->submitted_at_ns < oldest_submit_ns) {
                oldest_submit_ns = entry.run->submitted_at_ns;
                first = false;
            }
        }

        const core::Nanoseconds interval_timestamp{interval_ns};
        bool any_input = false;
        for (const PcmChunk* piece : pieces) {
            if (piece != nullptr) {
                any_input = true;
                break;
            }
        }

        MixedChunk mixed_interval;
        if (any_input) {
            const MixFrame frame{interval_timestamp, frames, pieces};
            auto mixed = impl_->mixer.mix(frame);
            if (!mixed.has_value()) {
                return mixed.error();
            }
            mixed_interval = mixed.value();
        }

        // Every write piece is bounded (64 KiB, whole-frame aligned); silence
        // comes from one reusable bounded zero buffer - never a
        // gap-proportional allocation.
        const std::uint64_t max_piece_frames = std::max<std::uint64_t>(
            1, kMaxOutputPieceBytes / frame_bytes);
        std::int64_t write_done_ns = 0;
        for (std::uint64_t piece_start = start; piece_start < end;) {
            const std::uint64_t piece_frames = std::min(end - piece_start, max_piece_frames);
            const std::size_t piece_bytes = static_cast<std::size_t>(piece_frames) * frame_bytes;
            std::int64_t piece_ns = 0;
            if (!impl_->frame_time(piece_start, base_ns, piece_ns)) {
                return Error{ErrorCode::invalid_argument};
            }
            MixedChunk to_write;
            to_write.format = impl_->config.output_format;
            to_write.timestamp = core::Nanoseconds{piece_ns};
            if (any_input) {
                const std::size_t slice_offset =
                    static_cast<std::size_t>(piece_start - start) * frame_bytes;
                to_write.pcm = std::span<const std::byte>{mixed_interval.pcm}.subspan(
                    slice_offset, piece_bytes);
            } else {
                if (impl_->silence_scratch.size() < piece_bytes) {
                    impl_->silence_scratch.assign(piece_bytes, std::byte{0});
                }
                to_write.pcm =
                    std::span<const std::byte>{impl_->silence_scratch}.first(piece_bytes);
            }

            auto written = impl_->sink->write(to_write);
            write_done_ns = steady_now_ns();
            if (!written.has_value()) {
                impl_->report.sink_probe = impl_->sink->probe_stats();
                impl_->report.render_barrier = impl_->sink->render_stats();
                // Gate fix F3: a routing write failure is an output loss -
                // the router degrades typed with the recorded cause, never a
                // silent "active" state with dropped audio.
                impl_->mark_degraded("disabled: routing write failed ("
                                     + std::string{to_string(written.error().code())}
                                     + "); output no longer accepting audio");
                return Error{ErrorCode::audio_sink_unavailable};
            }
            if (any_input && impl_->config.on_write_completed) {
                std::int64_t end_ns = 0;
                if (!impl_->frame_time(piece_start + piece_frames, base_ns, end_ns)) {
                    return Error{ErrorCode::invalid_argument};
                }
                impl_->config.on_write_completed(WriteCompletion{
                    to_write.timestamp, core::Nanoseconds{end_ns},
                    core::Nanoseconds{write_done_ns},
                    presented_at(impl_->sink->last_playback_slot(), write_done_ns),
                    impl_->sink->last_playback_slot_absence()});
            }
            impl_->report.stats.frames_output += piece_frames;
            if (!any_input) {
                impl_->report.stats.frames_silence += piece_frames;
            }
            piece_start += piece_frames;
        }

        RouterStats& stats = impl_->report.stats;
        stats.frames_mixed += 1;
        stats.frames_written += 1;
        stats.samples_written += frames * impl_->config.output_format.channels;

        // Path latency: receiver input (submit() BEFORE decode, gate fix F4)
        // to DATA-HANDOFF-CONFIRMED WRITE COMPLETION - the clock is sampled
        // here, when write() returned after its handoff credit and marker
        // settlement (gate r3 F3: the handoff event itself is not separately
        // timestamped). Gap intervals carry no receiver input, so they record
        // no latency.
        if (any_input) {
            const core::Nanoseconds path_latency{write_done_ns - oldest_submit_ns};
            stats.last_path_latency = path_latency;
            if (path_latency > stats.max_path_latency) {
                stats.max_path_latency = path_latency;
            }
            // Input-stamp to render-completion skew on the common monotonic
            // clock. Not a device-delivery measurement.
            const core::Nanoseconds completion_skew{write_done_ns - interval_timestamp.count};
            stats.last_completion_skew = completion_skew;
            if (completion_skew > stats.max_completion_skew) {
                stats.max_completion_skew = completion_skew;
            }
        }
    }

    // The whole timeline is consumed in one sweep (every run's range is
    // partitioned by the boundary set). The advanced cursor and role ends are
    // committed only now - after every write succeeded.
    if (!impl_->timeline_base.has_value()) {
        impl_->timeline_base = core::Nanoseconds{base_ns};
    }
    impl_->playback_cursor_frames = timeline_frames;
    impl_->role_end_frames = new_role_ends;
    impl_->source_frontier_frames = planned_source_frontier;
    impl_->role_source_frontier_frames = new_role_source_frontiers;
    impl_->report.stats.frames_retained += planned_retained;
    impl_->report.stats.frames_trimmed += planned_trimmed;
    impl_->report.stats.frames_caught_up += planned_caught_up;
    for (auto& queue : impl_->pending) {
        queue.clear();
    }

    impl_->report.sink_probe = impl_->sink->probe_stats();
    impl_->report.render_barrier = impl_->sink->render_stats();
    return {};
}

void AudioRouter::cancel_pending(protocol::ChannelRole role) {
    if (!impl_) { return; }
    const auto index = role_of(role);
    if (!index) { return; }
    std::lock_guard lock(impl_->pending_mutex);
    impl_->pending[role_index(*index)].clear();
    impl_->reset_playout(role_index(*index));
    if (std::none_of(impl_->playout.begin(), impl_->playout.end(),
                     [](const auto& state) { return state.anchored; })) {
        // The next stream re-anchors; the stat keeps the last reported value
        // (a stop no longer reads as a 0 ms offset in the final media_timing).
        impl_->last_reported.reset();
        impl_->last_write.reset();
    }
    impl_->role_end_frames[role_index(*index)] = impl_->playback_cursor_frames;
    impl_->role_source_frontier_frames[role_index(*index)] = 0;
    const auto remaining_frontier = *std::max_element(
        impl_->role_source_frontier_frames.begin(),
        impl_->role_source_frontier_frames.end());
    impl_->source_frontier_frames = std::min(
        impl_->source_frontier_frames,
        std::max(impl_->playback_cursor_frames, remaining_frontier));
    ++impl_->generations[role_index(*index)];
}

AudioRouter::StreamEnd AudioRouter::end_stream(protocol::ChannelRole role) {
    if (!impl_) { return {}; }
    const auto index = role_of(role);
    if (!index || !impl_->lossless()) { return {}; }
    std::lock_guard lock(impl_->pending_mutex);
    if (impl_->closed || impl_->flushed) { return {}; }
    const auto slot = role_index(*index);
    auto end = impl_->drain_decoder(slot);
    impl_->stream_ended[slot] = true;
    end.pending = !impl_->pending[slot].empty();
    return end;
}

std::uint64_t AudioRouter::pending_frames() const {
    if (!impl_) { return 0; }
    std::lock_guard lock(impl_->pending_mutex);
    return impl_->frames_pending();
}

bool AudioRouter::playout_anchored() const {
    if (!impl_) { return false; }
    std::lock_guard lock(impl_->pending_mutex);
    return std::any_of(impl_->playout.begin(), impl_->playout.end(),
                       [](const auto& state) { return state.anchored; });
}

std::vector<PlayoutEvent> AudioRouter::take_playout_events() {
    if (!impl_) { return {}; }
    std::lock_guard lock(impl_->pending_mutex);
    return std::exchange(impl_->playout_events, {});
}

void AudioRouter::note_backpressure(core::Nanoseconds start, core::Nanoseconds end) {
    if (!impl_ || end <= start) { return; }
    std::lock_guard lock(impl_->pending_mutex);
    auto& intervals = impl_->backpressure;
    std::pair<std::int64_t, std::int64_t> merged{start.count, end.count};
    std::erase_if(intervals, [&](const auto& interval) {
        if (interval.second < merged.first || interval.first > merged.second) { return false; }
        merged = {std::min(merged.first, interval.first), std::max(merged.second, interval.second)};
        return true;
    });
    intervals.insert(std::upper_bound(intervals.begin(), intervals.end(), merged), merged);
    if (intervals.size() > kMaxBackpressureIntervals) { intervals.erase(intervals.begin()); }
}

void AudioRouter::note_all_role_idle(core::Nanoseconds start, core::Nanoseconds end) {
    if (!impl_ || start.count < 0) { return; }
    end.count = std::min(end.count, steady_now_ns());
    if (end <= start || end.count - start.count <= 200'000'000) { return; }
    std::lock_guard lock(impl_->pending_mutex);
    if (!impl_->closed && !impl_->flushed && !impl_->idle) {
        impl_->idle = Impl::IdleAuthorization{{start.count, end.count}, {}};
    }
}

void AudioRouter::retire_all_role_idle() {
    if (!impl_) { return; }
    std::lock_guard lock(impl_->pending_mutex);
    impl_->idle.reset();
}

void AudioRouter::reset_session_timeline() {
    if (!impl_) { return; }
    std::lock_guard lock(impl_->pending_mutex);
    if (impl_->closed) { return; }
    for (std::size_t i = 0; i < kRoleCount; ++i) {
        impl_->pending[i].clear();
        impl_->reset_playout(i);
        ++impl_->generations[i];
    }
    impl_->timeline_base.reset();
    impl_->newest_arrival = INT64_MIN;
    impl_->last_reported.reset();
    impl_->last_write.reset();
    impl_->report.stats.playout_reported_offset = {};
    impl_->playback_cursor_frames = 0;
    impl_->role_end_frames.fill(0);
    impl_->source_frontier_frames = 0;
    impl_->role_source_frontier_frames.fill(0);
    impl_->backpressure.clear();
    impl_->idle.reset();
    impl_->report.render_failure.reset();
}

core::Result<RenderStep> AudioRouter::render_step() {
    if (!impl_) { return Error{ErrorCode::invalid_argument}; }
    impl_->report.render_failure.reset();
    std::unique_lock lock(impl_->pending_mutex);
    RenderFailureSnapshot diagnostic;
    diagnostic.output_sample_rate = impl_->config.output_format.sample_rate_hz;
    diagnostic.base_committed = impl_->timeline_base.has_value();
    if (impl_->timeline_base) { diagnostic.base_ns = impl_->timeline_base->count; }
    diagnostic.entry_cursor = impl_->playback_cursor_frames;
    const auto fail = [&](RenderFailureReason reason, std::optional<ErrorCode> code = {}) {
        if (impl_->report.render_failure) { return; }
        diagnostic.reason = reason;
        diagnostic.underlying_code = code;
        const auto store = [](RenderRangeSummary& summary, const auto& interval) {
            ++summary.count;
            summary.start_ns = summary.start_ns ? std::min(*summary.start_ns, interval.first) : interval.first;
            summary.end_ns = summary.end_ns ? std::max(*summary.end_ns, interval.second) : interval.second;
        };
        for (const auto& interval : impl_->backpressure) { store(diagnostic.stored_backpressure, interval); }
        if (impl_->idle) {
            store(diagnostic.stored_idle, impl_->idle->interval);
            diagnostic.stored_idle_prefix = impl_->idle->prefix_end;
        }
        impl_->report.render_failure = diagnostic;
    };
    if (impl_->closed) { fail(RenderFailureReason::closed); return Error{ErrorCode::invalid_argument}; }
    if (impl_->report.state == RouterState::degraded) {
        fail(RenderFailureReason::degraded);
        return Error{ErrorCode::audio_sink_unavailable};
    }
    if (impl_->flushed) { fail(RenderFailureReason::flushed); return Error{ErrorCode::invalid_argument}; }
    if (!impl_->sink) {
        fail(RenderFailureReason::missing_sink);
        impl_->mark_degraded("disabled: routing sink missing");
        return Error{ErrorCode::audio_sink_unavailable};
    }
    impl_->incremental = true;
    const auto generations = impl_->generations;
    RenderStep result;
    bool retimed = std::exchange(impl_->retimed, false);
    if (impl_->lossless()) {
        const auto timing = impl_->sink->take_timing_adjustment();
        if (timing.starvations || timing.moved.count > 0) {
            impl_->playout[role_index(Role::media)].recovering_from_hold = false;
        }
        impl_->report.stats.playout_sink_starvations += timing.starvations;
        impl_->report.stats.playout_sink_starved.count += timing.starved.count;
        impl_->report.stats.playout_sink_suspends += timing.suspends;
        impl_->report.stats.playout_sink_suspend_loss_frames += timing.suspend_loss_frames;
        impl_->report.stats.playout_sink_disruptions += timing.disruptions;
        impl_->report.stats.playout_sink_late_frames += timing.late_frames;
        const auto held = timing.held.count != 0 && impl_->sink->hold_stream(timing.held)
            ? timing.held.count : 0;
        const bool have_timeline = impl_->timeline_base
            && std::any_of(impl_->playout.begin(), impl_->playout.end(),
                           [](const auto& state) { return state.anchored; });
        if (timing.drift.count != 0 && impl_->sink->hold_stream(timing.drift) && have_timeline) {
            // A later schedule ages placed PCM like a retime does: the same
            // emergency bound applies (#229 P2-4).
            impl_->shift_timeline(timing.drift.count, timing.drift.count > 0);
        }
        const auto shift = held + timing.moved.count;
        if (shift != 0 && have_timeline) {
            // A change in the sink's time mapping applies even when the next
            // wall-clock slot is still ahead. Preserve queued PCM's stamps
            // across a phase step. An ingress reschedule has already moved
            // the current buffer's stamp, so only its successors need to move.
            impl_->retime(shift, held);
            retimed = true;
        }
        for (std::size_t role = 0; role < kRoleCount; ++role) {
            const auto& queue = impl_->pending[role];
            while (!queue.empty() && queue.front().frames == 0) {
                result.completed.push_back(queue.front().header);
                impl_->finish_silent_run(role);
            }
        }
    }
    auto base = impl_->timeline_base;
    if (!base) {
        // A playout stream's timeline starts at its anchor (arrival + depth),
        // so the buffer fills without synthesized silence.
        const auto lead = impl_->playout_enabled() ? impl_->anchor_depth() : 0;
        for (const auto& queue : impl_->pending) {
            for (const auto& run : queue) {
                const core::Nanoseconds start{
                    run.timestamp.count > INT64_MAX - lead ? INT64_MAX : run.timestamp.count + lead};
                if (!base || start < *base) { base = start; }
            }
        }
    }
    if (!base) { return result; }
    diagnostic.base_ns = base->count;
    const auto cursor = impl_->playback_cursor_frames;
    const auto cap = impl_->ns_to_frames(core::Nanoseconds{200'000'000});
    const auto frame_bytes = static_cast<std::size_t>(impl_->config.output_format.channels) * 2;
    const auto quantum = std::min<std::uint64_t>(
        kMaxOutputPieceBytes / frame_bytes,
        impl_->config.render_quantum.count > 0 ? impl_->ns_to_frames(impl_->config.render_quantum) : 0);
    diagnostic.cap = cap;
    diagnostic.quantum = quantum;
    if (quantum == 0) { fail(RenderFailureReason::zero_quantum); return Error{ErrorCode::audio_format_unsupported}; }

    // Playout: where an unplaced run goes, back to back from the role's stream
    // (`at`: the cursor when it is its turn), or re-anchored at arrival + depth on a
    // fresh stream or an underrun. The decision is kept once the run heads its role.
    const auto place_playout = [&](std::size_t role, Run& run, bool is_head, std::uint64_t at,
                                   std::uint64_t role_end, bool& anchored, std::uint64_t& decoder_owed,
                                   ScheduledRun& entry) -> std::optional<RenderFailureReason> {
        if (run.stream_start) {
            anchored = false;
            decoder_owed = 0;
        }
        const auto slot_frame = std::max(at, role_end);
        PlayoutPlan plan;
        if (run.playout) {
            plan = *run.playout;
        } else if (!impl_->plan_playout(role, run, slot_frame, role_end + decoder_owed,
                                        base->count, anchored, plan)) {
            diagnostic.time_conversion_frame = slot_frame;
            return RenderFailureReason::placement_time_conversion;
        } else if (is_head) {
            if (impl_->lossless() && plan.mode == PlayoutPlan::Mode::steady
                && !plan.backward && plan.depth_ns <= impl_->config.playout.emergency_backlog.count) {
                impl_->apply_rate(role, run, plan);
            }
            run.playout = plan;
        }
        const bool continues = plan.mode == PlayoutPlan::Mode::steady || plan.decoder_held;
        decoder_owed = Impl::owed_after(continues ? decoder_owed : 0, run);
        if (plan.backward) { return RenderFailureReason::late; }
        if (plan.mode == PlayoutPlan::Mode::steady) {
            entry.start_frame = std::max(plan.start_frame, slot_frame);
            // Measured against the planned slot: the pad's own silence
            // advances the cursor before the run is placed (N7).
            entry.applied_pad_frames = entry.start_frame - std::min(entry.start_frame, plan.slot_frame);
            entry.head_trim_frames = std::min(plan.trim_frames, run.frames);
        } else {
            std::int64_t target = 0;
            std::int64_t arrived = 0;
            if (!impl_->ns_to_frames_signed(core::Nanoseconds{plan.target_ns - base->count}, target)
                || !impl_->ns_to_frames_signed(
                       core::Nanoseconds{run.timestamp.count - base->count}, arrived)) {
                return RenderFailureReason::intended_time_conversion;
            }
            entry.start_frame = static_cast<std::uint64_t>(
                std::max<std::int64_t>(target, static_cast<std::int64_t>(slot_frame)));
            // The hole up to the arrival has already played out.
            if (arrived > static_cast<std::int64_t>(slot_frame)) {
                entry.skip_first = slot_frame;
                entry.skip_last = std::min(entry.start_frame, static_cast<std::uint64_t>(arrived));
            }
            entry.refill_allowance = impl_->ns_to_frames(impl_->config.playout.depth) + 1;
        }
        if (impl_->lossless()) {
            if (!impl_->overflow_beyond(run, entry.start_frame, base->count, 0, run.frames,
                                        entry.overflow_frames)) {
                diagnostic.time_conversion_frame = entry.start_frame;
                return RenderFailureReason::placement_time_conversion;
            }
            entry.head_trim_frames = entry.overflow_frames;
        }
        entry.intended = static_cast<std::int64_t>(entry.start_frame);
        anchored = true;
        return {};
    };
    // Lossless: the run at the head of the role's queue, when its turn comes within
    // this step. False: that is the next step's.
    const auto place_next = [&](std::size_t role, std::uint64_t at, ScheduledRun& entry) {
        auto& queue = impl_->pending[role];
        if (queue.empty() || queue.front().placed || queue.front().frames == 0) { return false; }
        auto& run = queue.front();
        entry = ScheduledRun{};
        entry.role = role;
        entry.run = &run;
        entry.role_end_before = impl_->role_end_frames[role];
        bool anchored = impl_->playout[role].anchored;
        auto decoder_owed = impl_->playout[role].decoder_owed;
        if (place_playout(role, run, true, at, entry.role_end_before, anchored, decoder_owed, entry)) {
            diagnostic.time_conversion_frame.reset();
            return false;
        }
        const auto remaining = run.frames - entry.head_trim_frames;
        std::int64_t end_ns = 0;
        if (remaining > UINT64_MAX - entry.start_frame
            || !impl_->frame_time(entry.start_frame + remaining, base->count, end_ns)) { return false; }
        entry.end_frame = entry.start_frame + remaining;
        return true;
    };

    // Preflight all parents, not just the visible heads: splitting a schedule
    // into steps must not evade the existing atomic discontinuity policy.
    // This is a dry run; placement/trim become persistent only on consumption.
    std::vector<ScheduledRun> planned;
    bool valid = cursor <= static_cast<std::uint64_t>(INT64_MAX)
        && quantum <= UINT64_MAX - cursor;
    if (!valid) {
        fail(cursor > static_cast<std::uint64_t>(INT64_MAX)
            ? RenderFailureReason::cursor_range : RenderFailureReason::quantum_end_overflow);
    }
    for (std::size_t role = 0; role < kRoleCount && valid; ++role) {
        auto role_end = impl_->role_end_frames[role];
        auto role_source_frontier = impl_->role_source_frontier_frames[role];
        bool anchored = impl_->playout[role].anchored;
        auto decoder_owed = impl_->playout[role].decoder_owed;
        for (auto& run : impl_->pending[role]) {
            diagnostic.offending = run.header;
            diagnostic.preflighted = run.preflighted;
            diagnostic.role_end = role_end;
            diagnostic.intended.reset();
            diagnostic.start.reset();
            diagnostic.remaining.reset();
            diagnostic.time_conversion_frame.reset();
            ScheduledRun entry;
            entry.role = role;
            entry.run = &run;
            entry.role_end_before = role_end;
            if (run.placed) {
                entry.start_frame = *run.placed + run.consumed;
                entry.head_trim_frames = run.trimmed + run.consumed;
            } else if (impl_->playout_enabled()) {
                if (run.frames == 0) { continue; } // nothing to place (finish_silent_run)
                if (const auto failed = place_playout(role, run, &run == &impl_->pending[role].front(),
                                                      cursor, role_end, anchored, decoder_owed, entry)) {
                    fail(*failed);
                    valid = false;
                    break;
                }
                diagnostic.intended = entry.intended;
                diagnostic.start = entry.start_frame;
                role_source_frontier =
                    std::max(role_source_frontier, source_end_frame(*entry.intended, run.content_frames));
            } else {
                std::int64_t intended = 0;
                if (!impl_->ns_to_frames_signed(
                        core::Nanoseconds{run.timestamp.count - base->count}, intended)) {
                    fail(RenderFailureReason::intended_time_conversion);
                    valid = false;
                    break;
                }
                diagnostic.intended = intended;
                entry.intended = intended;
                entry.start_frame = static_cast<std::uint64_t>(std::max<std::int64_t>(
                    intended, static_cast<std::int64_t>(std::max(cursor, role_end))));
                diagnostic.start = entry.start_frame;
                std::int64_t placed_ns = 0;
                if (!impl_->frame_time(entry.start_frame, base->count, placed_ns)) {
                    diagnostic.time_conversion_frame = entry.start_frame;
                    fail(RenderFailureReason::placement_time_conversion);
                    valid = false;
                    break;
                }
                const core::Nanoseconds phase{placed_ns - run.timestamp.count};
                const bool backlog_carry =
                    phase.count > 0 || intended < 0
                    || static_cast<std::uint64_t>(intended) < role_source_frontier;
                plan_drift_trim(impl_->drift, phase, run, backlog_carry, entry);
                if (!run.preflighted &&
                    late_beyond_cap(cursor, impl_->source_frontier_frames, intended, run, entry,
                                    cap)) {
                    diagnostic.start.reset();
                    fail(RenderFailureReason::late);
                    valid = false;
                    break;
                }
                role_source_frontier =
                    std::max(role_source_frontier, source_end_frame(intended, run.content_frames));
            }
            const auto remaining = run.frames - entry.head_trim_frames;
            diagnostic.start = entry.start_frame;
            diagnostic.remaining = remaining;
            std::int64_t end_ns = 0;
            if (remaining > UINT64_MAX - entry.start_frame) {
                fail(RenderFailureReason::parent_end_overflow);
                valid = false;
                break;
            }
            if (!impl_->frame_time(entry.start_frame + remaining, base->count, end_ns)) {
                diagnostic.time_conversion_frame = entry.start_frame + remaining;
                fail(RenderFailureReason::parent_end_time_conversion);
                valid = false;
                break;
            }
            entry.end_frame = entry.start_frame + remaining;
            role_end = entry.end_frame;
            planned.push_back(entry);
        }
    }
    std::stable_sort(planned.begin(), planned.end(), [](const auto& left, const auto& right) {
        return left.start_frame < right.start_frame;
    });
    const auto now_ns = impl_->now_ns();
    diagnostic.now_ns = now_ns;
    std::int64_t cursor_ns = 0;
    if (valid && impl_->frame_time(cursor, base->count, cursor_ns)) {
        std::erase_if(impl_->backpressure, [&](const auto& interval) { return interval.second <= cursor_ns; });
    }
    auto prefix_end = cursor;
    if (!planned.empty() && impl_->idle) {
        prefix_end = impl_->idle->prefix_end.value_or(planned.front().start_frame);
        prefix_end = std::min(prefix_end, planned.front().start_frame);
    }
    const auto elapsed = impl_->elapsed_ranges({base->count, now_ns}, prefix_end);
    diagnostic.prefix_end = prefix_end;
    auto covered_end = cursor;
    std::uint64_t total_gap = 0;
    for (const auto& entry : planned) {
        // As in pump(), even a completely trimmed parent can end a gap. Only
        // silence still requiring synthesis counts against the caps; time
        // that already elapsed while credit was withheld is never rendered.
        if (entry.start_frame > covered_end) {
            auto gap = entry.start_frame - covered_end;
            for (const auto& range : elapsed) {
                const auto lo = std::max(covered_end, range.first);
                const auto hi = std::min(entry.start_frame, range.last);
                if (hi > lo) { gap -= hi - lo; }
            }
            if (entry.skip_last > entry.skip_first) {
                // Playout underrun hole: elapsed, disjoint from the ranges above
                // only where they do not already cover it.
                std::uint64_t covered = 0;
                const auto lo = std::max(covered_end, entry.skip_first);
                const auto hi = std::min(entry.start_frame, entry.skip_last);
                for (const auto& range : elapsed) {
                    const auto a = std::max(lo, range.first);
                    const auto b = std::min(hi, range.last);
                    if (b > a) { covered += b - a; }
                }
                if (hi > lo && hi - lo > covered) { gap -= std::min(gap, hi - lo - covered); }
            }
            // A re-anchor's refill is the playout buffer (<= depth < cap),
            // not gap silence: it never counts against the caps. Only
            // re-anchor entries carry the allowance, and it is bounded by the
            // depth, so an unexplained gap beyond the refill still fails (N8).
            gap -= std::min(gap, entry.refill_allowance);
            if (gap > cap || total_gap > cap - gap) {
                diagnostic.offending = entry.run->header;
                diagnostic.preflighted = entry.run->preflighted;
                diagnostic.intended = entry.intended;
                diagnostic.role_end = entry.role_end_before;
                diagnostic.start = entry.start_frame;
                diagnostic.remaining = entry.end_frame - entry.start_frame;
                diagnostic.time_conversion_frame.reset();
                diagnostic.covered_end = covered_end;
                diagnostic.raw_gap = entry.start_frame - covered_end;
                diagnostic.uncovered_gap = gap;
                diagnostic.total_gap_before = total_gap;
                diagnostic.effective_backpressure.emplace();
                diagnostic.effective_idle.emplace();
                for (const auto& range : elapsed) {
                    const auto lo = std::max(covered_end, range.first);
                    const auto hi = std::min(entry.start_frame, range.last);
                    if (hi <= lo) { continue; }
                    auto& summary = range.idle ? *diagnostic.effective_idle : *diagnostic.effective_backpressure;
                    ++summary.count;
                    summary.first_frame = summary.first_frame ? std::min(*summary.first_frame, lo) : lo;
                    summary.last_frame = summary.last_frame ? std::max(*summary.last_frame, hi) : hi;
                    summary.excluded_frames += hi - lo;
                }
                fail(gap > cap ? RenderFailureReason::single_gap : RenderFailureReason::aggregate_gap);
                valid = false;
                break;
            }
            total_gap += gap;
        }
        covered_end = std::max(covered_end, entry.end_frame);
    }
    if (!valid) {
        for (auto& queue : impl_->pending) { queue.clear(); }
        impl_->idle.reset();
        return Error{ErrorCode::invalid_argument};
    }
    if (!planned.empty() && impl_->idle) { impl_->idle->prefix_end = prefix_end; }
    for (auto& queue : impl_->pending) {
        for (auto& run : queue) { run.preflighted = true; }
    }
    std::array<std::optional<ScheduledRun>, kRoleCount> heads{};
    auto limit = cursor + quantum;
    auto end = limit;
    bool any = false;
    const auto reach_source_end = [&](const ScheduledRun& head) {
        if (!head.intended) { return; }
        const auto run_source_end = source_end_frame(*head.intended, head.run->content_frames);
        impl_->source_frontier_frames = std::max(impl_->source_frontier_frames, run_source_end);
        impl_->role_source_frontier_frames[head.role] =
            std::max(impl_->role_source_frontier_frames[head.role], run_source_end);
    };
    // The head of its role's queue, of which nothing is left to play.
    const auto finish_cut = [&](const ScheduledRun& head) {
        auto& queue = impl_->pending[head.role];
        // A placed run ends here when a retime cut its rest: already counted.
        if (!queue.front().placed) {
            impl_->report.stats.frames_trimmed += head.head_trim_frames;
            impl_->report.stats.frames_caught_up += head.caught_up_frames;
            impl_->commit_playout(head.role, queue.front(), head, base->count);
        }
        impl_->role_end_frames[head.role] = head.end_frame;
        reach_source_end(head);
        result.completed.push_back(queue.front().header);
        queue.pop_front();
    };
    // Lossless: roles whose next run waits to be planned as a head of its own.
    std::array<bool, kRoleCount> replan{};
    // Lossless: roles whose cut heads were finished here. `planned` holds every
    // run of the role, and those after the cut one are popped or replanned
    // already: a cut that took the whole queue left them pointing at freed runs
    // (d0c5b41 crashed there behind a retime cascade).
    std::array<bool, kRoleCount> cut_through{};
    for (const auto& head : planned) {
        const auto role = head.role;
        if (heads[role] || replan[role] || cut_through[role]) { continue; }
        auto& queue = impl_->pending[role];
        if (head.start_frame == head.end_frame) {
            finish_cut(head);
            if (impl_->lossless()) {
                ScheduledRun next;
                while (place_next(role, cursor, next) && next.start_frame == next.end_frame) { finish_cut(next); }
                replan[role] = !queue.empty();
                cut_through[role] = true;
            }
            continue;
        }
        // Never cross an earlier queued head/end to reach a future role.
        if (head.start_frame > cursor) { limit = std::min(limit, head.start_frame); }
        end = std::min(end, head.start_frame > cursor ? head.start_frame : head.end_frame);
        heads[role] = head;
        any = true;
    }
    if (std::find(replan.begin(), replan.end(), true) != replan.end()) {
        // What follows a wholly cut run had no playout decision yet (only a head
        // gets one): the next step makes it, so nothing is placed, cut or
        // reported without one.
        result.pending = true;
        return result;
    }
    if (!any) { return result; }
    if (impl_->lossless() && !retimed && !impl_->sink->handles_late_slots()
        && std::any_of(heads.begin(), heads.end(),
                       [&](const auto& head) { return head && head->start_frame <= cursor; })) {
        // This piece's slot has passed: the sink ran dry while a write blocked
        // (or this thread was held up), and samples written behind its read
        // position would never be heard. Play on from here instead. A slot
        // further back than the emergency bound is not a lateness: the stamps
        // come from another clock.
        std::int64_t slot_ns = 0;
        const auto lead = std::min(kHoldLeadNs, impl_->config.playout.depth.count / 2);
        if (impl_->timeline_base && impl_->frame_time(cursor, base->count, slot_ns) && slot_ns < now_ns
            && now_ns - slot_ns <= impl_->config.playout.emergency_backlog.count) {
            const auto late = now_ns - slot_ns;
            // While the preceding write was blocked past its slot, the ring
            // had no space for its successor. Keep that part of the shift
            // (and its share of the refill lead) out of the sink timestamps.
            const auto shift = late + lead;
            std::int64_t held = 0;
            std::int64_t extra_wait = 0;
            if (const auto& write = impl_->last_write;
                write && std::abs(write->slot_end - slot_ns) <= kSameInstantNs) {
                const auto blocked = std::clamp(write->returned - slot_ns, std::int64_t{0}, late);
                held = blocked + lead * blocked / late;
                if (held > 0) {
                    // A long write can start ahead of its slot and already
                    // be blocked there. Count that wait too, so draining the
                    // queue doesn't run into the same pause a second time.
                    const auto waited = std::min(write->returned - write->entered,
                                                 impl_->config.playout.emergency_backlog.count);
                    extra_wait = std::max<std::int64_t>(0, waited - held);
                }
            }
            const bool kept = held > 0 && impl_->sink->hold_stream(core::Nanoseconds{held + extra_wait});
            impl_->retime(shift + (kept ? extra_wait : 0), kept ? held + extra_wait : 0);
            impl_->retimed = true;
            lock.unlock();
            auto again = render_step();
            impl_->retimed = false;
            if (again) {
                auto& completed = again.value().completed;
                completed.insert(completed.begin(), result.completed.begin(), result.completed.end());
            }
            return again;
        }
    }
    // Stamps are receipt times on this clock, so the newest one has passed.
    const auto known_ns = std::max(now_ns, impl_->newest_arrival);
    if (impl_->lossless() && !impl_->draining && impl_->awaiting_role(known_ns)) {
        // A frame written now is gone for the role that still owes its next
        // chunk: rendering ahead without it used to push that role's whole
        // stream behind its arrival (a prompt over media, #228). Wait for it
        // until shortly before the frame is due.
        std::int64_t due_ns = 0;
        const auto lead = std::min(kHoldLeadNs, impl_->config.playout.depth.count / 2);
        if (!impl_->frame_time(cursor, base->count, due_ns)) {
            diagnostic.time_conversion_frame = cursor;
            fail(RenderFailureReason::output_start_time_conversion);
            return Error{ErrorCode::invalid_argument};
        }
        if (due_ns - lead > known_ns) {
            result.wait_until = core::Nanoseconds{due_ns - lead};
            return result;
        }
        const auto ahead = impl_->ns_to_frames(
            core::Nanoseconds{known_ns - (due_ns - lead) + kHoldQuantumNs});
        limit = std::min(limit, cursor + std::max<std::uint64_t>(1, std::min(ahead, quantum)));
        end = std::min(end, limit);
    }
    const bool silence_only = std::none_of(heads.begin(), heads.end(), [&](const auto& head) {
        return head && head->start_frame <= cursor;
    });
    if (silence_only) {
        auto next_start = UINT64_MAX;
        for (const auto& head : heads) {
            if (head) { next_start = std::min(next_start, head->start_frame); }
        }
        for (const auto& range : elapsed) {
            if (range.first <= cursor && cursor < range.last) {
                const auto skipped = std::min(range.last, next_start) - cursor;
                impl_->timeline_base = base;
                impl_->playback_cursor_frames = cursor + skipped;
                impl_->source_frontier_frames =
                    std::max(impl_->source_frontier_frames, impl_->playback_cursor_frames);
                if (range.idle) { impl_->report.stats.frames_idle_skipped += skipped; }
                else { impl_->report.stats.frames_skipped += skipped; }
                result.pending = true;
                return result;
            }
            if (range.first > cursor) { end = std::min(end, range.first); }
        }
        // A playout re-anchor's elapsed hole is skipped like a recorded one;
        // recorded idle/backpressure ranges (checked above) own their frames.
        for (const auto& head : heads) {
            if (head && head->skip_first <= cursor && cursor < head->skip_last) {
                auto limit = std::min(head->skip_last, next_start);
                for (const auto& range : elapsed) {
                    if (range.first > cursor) { limit = std::min(limit, range.first); }
                }
                const auto skipped = limit - cursor;
                impl_->timeline_base = base;
                impl_->playback_cursor_frames = cursor + skipped;
                impl_->source_frontier_frames =
                    std::max(impl_->source_frontier_frames, impl_->playback_cursor_frames);
                impl_->report.stats.frames_skipped += skipped;
                result.pending = true;
                return result;
            }
        }
    }
    std::int64_t scheduled_ns = 0;
    std::int64_t end_ns = 0;
    // Interval operands are copied before any parents can be popped/unlocked.
    diagnostic.offending.reset();
    diagnostic.preflighted.reset();
    diagnostic.intended.reset();
    diagnostic.role_end.reset();
    diagnostic.start.reset();
    diagnostic.remaining.reset();
    diagnostic.output_start = cursor;
    diagnostic.output_end = end;
    if (!impl_->frame_time(cursor, base->count, scheduled_ns)) {
        diagnostic.time_conversion_frame = cursor;
        fail(RenderFailureReason::output_start_time_conversion);
        return Error{ErrorCode::invalid_argument};
    }
    if (!impl_->frame_time(end, base->count, end_ns)) {
        diagnostic.time_conversion_frame = end;
        fail(RenderFailureReason::output_end_time_conversion);
        return Error{ErrorCode::invalid_argument};
    }
    // One role's share of the write: a run's PCM, or (lossless) that of the short
    // runs that follow one another, held here once their parents are done.
    struct RolePiece final {
        std::span<const std::byte> pcm{};
        std::vector<std::byte> joined{};
        std::vector<std::byte> first{}; // keeps `pcm` alive once its run is gone
        std::int64_t offset_ns{};
    };
    std::array<RolePiece, kRoleCount> shares{};
    std::array<bool, kRoleCount> active{};
    std::optional<std::int64_t> oldest;
    for (std::size_t role = 0; role < kRoleCount; ++role) {
        active[role] = heads[role] && heads[role]->start_frame <= cursor;
    }
    const auto joined = impl_->ns_to_frames(core::Nanoseconds{kJoinedWriteNs});
    if (impl_->lossless()) { limit = std::min(limit, cursor + joined); }
    for (auto at = cursor;;) {
        const auto frames = end - at;
        for (std::size_t role = 0; role < kRoleCount; ++role) {
            if (!active[role]) { continue; }
            const auto& head = *heads[role];
            auto& share = shares[role];
            auto& run = impl_->pending[role].front();
            if (!run.placed) {
                run.placed = head.start_frame;
                run.trimmed = head.head_trim_frames;
                impl_->report.stats.frames_trimmed += run.trimmed;
                impl_->report.stats.frames_caught_up += head.caught_up_frames;
                impl_->commit_playout(role, run, head, base->count);
                reach_source_end(head);
            }
            const auto pcm = std::span<const std::byte>{run.pcm}.subspan(
                static_cast<std::size_t>(run.trimmed + run.consumed) * frame_bytes,
                static_cast<std::size_t>(frames) * frame_bytes);
            if (at == cursor) {
                share.pcm = pcm;
            } else {
                if (share.joined.empty()) { share.joined.assign(share.pcm.begin(), share.pcm.end()); }
                share.joined.insert(share.joined.end(), pcm.begin(), pcm.end());
            }
            oldest = oldest ? std::min(*oldest, run.submitted_at_ns) : run.submitted_at_ns;
            share.offset_ns = run.playout_offset_ns;
            // Another role's end can split what a run gives this write.
            if (at == cursor || run.consumed == 0) { result.contributors.push_back(run.header); }
            impl_->idle.reset();
            run.consumed += frames;
            impl_->role_end_frames[role] = end;
            if (run.trimmed + run.consumed == run.frames) {
                result.completed.push_back(run.header);
                result.delivered.push_back(run.header);
                if (at == cursor) { share.first = std::move(run.pcm); }
                impl_->pending[role].pop_front();
            }
        }
        at = end;
        // Lossless: on into the short runs that go on without a gap.
        if (!impl_->lossless() || !oldest || at >= limit) { break; }
        auto next_end = limit;
        bool more = true;
        for (std::size_t role = 0; role < kRoleCount && more; ++role) {
            if (!active[role]) { continue; }
            auto& queue = impl_->pending[role];
            ScheduledRun next;
            if (!queue.empty() && queue.front().placed) {
                next = *heads[role]; // not done yet
            } else {
                do {
                    more = place_next(role, at, next) && !queue.front().stream_start
                        && queue.front().frames < joined && next.start_frame == at
                        && next.skip_first == next.skip_last;
                    if (more && next.end_frame == at) { finish_cut(next); } else { break; }
                } while (true);
                heads[role] = next;
            }
            next_end = std::min(next_end, next.end_frame);
        }
        if (!more || !impl_->frame_time(next_end, base->count, end_ns)) { break; }
        end = next_end;
    }
    if (!impl_->frame_time(end, base->count, end_ns)) { end_ns = scheduled_ns; } // checked above
    diagnostic.output_end = end;
    const auto frames = end - cursor;
    std::array<PcmChunk, kRoleCount> materialized{};
    std::array<const PcmChunk*, kRoleCount> pieces{};
    std::uint64_t role_frames = 0;
    std::int64_t playout_offset_ns = 0;
    for (std::size_t role = 0; role < kRoleCount; ++role) {
        if (!active[role]) { continue; }
        const auto& share = shares[role];
        materialized[role] = PcmChunk{static_cast<Role>(role), impl_->config.output_format,
            core::Nanoseconds{scheduled_ns},
            share.joined.empty() ? share.pcm : std::span<const std::byte>{share.joined}};
        pieces[role] = &materialized[role];
        role_frames += frames;
        playout_offset_ns = std::max(playout_offset_ns, share.offset_ns);
    }
    if (const auto media = impl_->media_offset(scheduled_ns)) { playout_offset_ns = *media; }
    // Detached: cancel_pending can now free EVERY parent remainder while this
    // single, bounded mixed piece is inside sink->write.
    std::vector<std::byte> output;
    if (oldest) {
        auto mixed = impl_->mixer.mix(MixFrame{core::Nanoseconds{scheduled_ns}, frames, pieces});
        if (!mixed) { fail(RenderFailureReason::mixer_error, mixed.error().code()); return mixed.error(); }
        output.assign(mixed.value().pcm.begin(), mixed.value().pcm.end());
    } else {
        output.assign(static_cast<std::size_t>(frames) * frame_bytes, std::byte{0});
    }
    impl_->timeline_base = base;
    impl_->playback_cursor_frames = end;
    // The reported offset moves toward its target by at most half the output
    // time that passes, in both directions (Neon P1-1): content then advances
    // at between half and one and a half times the playout rate, so the
    // reported content interval never starts before the last one ended (the
    // collector never drops a write, Neon P2-4), and the video hold that
    // follows it, t + O(t), only ever increases - frames are never pinned
    // behind an earlier due (no freeze, then burst). Across an output gap (an
    // underrun's skip and refill) it may fall by half the gap at once, and rise
    // by all of it.
    const auto slewed_offsets = [&](std::int64_t wanted) {
        const auto target = std::max<std::int64_t>(0, wanted);
        const auto length = end_ns - scheduled_ns;
        auto from = target;
        if (impl_->last_reported) {
            const auto [previous_end, previous_offset] = *impl_->last_reported;
            const auto gap = std::max<std::int64_t>(0, scheduled_ns - previous_end);
            from = std::clamp(target, previous_offset - gap / 2, previous_offset + gap);
        }
        from = std::max<std::int64_t>(0, from);
        const auto step = std::clamp(target - from, -length / 2, length / 2);
        return std::pair{from, std::max<std::int64_t>(0, from + step)};
    };
    if (impl_->lossless() && impl_->config.on_write_started) {
        // Silence precedes an anchor, which plays at the configured depth.
        const auto [from, to] = oldest
            ? slewed_offsets(playout_offset_ns)
            : std::pair{impl_->config.playout.depth.count, impl_->config.playout.depth.count};
        const WriteStart started{core::Nanoseconds{scheduled_ns}, core::Nanoseconds{end_ns},
                                 core::Nanoseconds{from}, core::Nanoseconds{to}};
        // As on_write_completed: the worker's mutex is ordered before this one.
        lock.unlock();
        impl_->config.on_write_started(started);
    } else {
        lock.unlock();
    }
    const auto write_entered = impl_->lossless() ? impl_->now_ns() : 0;
    const auto written = impl_->sink->write(MixedChunk{
        impl_->config.output_format, core::Nanoseconds{scheduled_ns}, output});
    const auto done = steady_now_ns();
    // When the write returned: the lateness fallback measures a blocked write
    // by it, so nothing after it (the completion callback) may add to it.
    const auto returned = impl_->config.clock ? impl_->now_ns() : done;
    impl_->report.sink_probe = impl_->sink->probe_stats();
    impl_->report.render_barrier = impl_->sink->render_stats();
    if (!written) {
        lock.lock();
        diagnostic.discarded_frames = role_frames + impl_->frames_pending();
        fail(RenderFailureReason::sink_write_error, written.error().code());
        for (auto& queue : impl_->pending) { queue.clear(); }
        impl_->mark_degraded("disabled: routing write failed ("
            + std::string{to_string(written.error().code())} + "); output no longer accepting audio");
        return Error{ErrorCode::audio_sink_unavailable};
    }
    auto& stats = impl_->report.stats;
    ++stats.frames_mixed;
    ++stats.frames_written;
    stats.samples_written += frames * impl_->config.output_format.channels;
    stats.frames_output += frames;
    stats.frames_retained += role_frames;
    if (oldest) {
        stats.last_path_latency = core::Nanoseconds{done - *oldest};
        stats.max_path_latency = std::max(stats.max_path_latency, stats.last_path_latency);
        stats.last_completion_skew = core::Nanoseconds{done - scheduled_ns};
        stats.max_completion_skew = std::max(stats.max_completion_skew, stats.last_completion_skew);
        std::int64_t playout_end_offset_ns = playout_offset_ns;
        if (impl_->playout_enabled()) {
            // last_reported is shared with cancel_pending() on the session
            // thread, so it is read and written under pending_mutex (Neon
            // P1-2), and a write that completes after the last stream was
            // cancelled (no role anchored; the gate is router-wide, as
            // cancel_pending's reset is) no longer seeds the next stream's
            // slew. The lock is released again before on_write_completed: that
            // callback takes the render worker's mutex, which is ordered
            // before this one.
            lock.lock();
            std::tie(playout_offset_ns, playout_end_offset_ns) = slewed_offsets(playout_offset_ns);
            if (std::any_of(impl_->playout.begin(), impl_->playout.end(),
                            [](const auto& state) { return state.anchored; })) {
                impl_->last_reported = std::pair{end_ns, playout_end_offset_ns};
                stats.playout_reported_offset = core::Nanoseconds{playout_end_offset_ns};
            }
            lock.unlock();
        }
        if (impl_->config.on_write_completed) {
            const auto slot = impl_->sink->last_playback_slot();
            const bool clocked = impl_->lossless() && impl_->sink->last_slot_from_read_clock();
            const auto presented = clocked ? slot : presented_at(slot, done);
            auto plays_from = playout_offset_ns;
            auto plays_to = playout_end_offset_ns;
            if (clocked && slot) {
                const auto shift = slot->count - scheduled_ns;
                plays_from = std::max<std::int64_t>(0, plays_from + shift);
                plays_to = std::max<std::int64_t>(0, plays_to + shift);
            } else if (impl_->lossless()) {
                // Where the sink really plays it: not before it was handed over
                // and not before its slot on the sink's clock. What follows a
                // piece that came back after its own end is retimed.
                const auto finished = impl_->config.clock ? impl_->now_ns() : done;
                const auto bound = impl_->config.playout.emergency_backlog.count;
                const auto late = std::max(finished, presented ? presented->count : finished) - scheduled_ns;
                if (late > kLatePlaybackNs && late <= bound) { plays_from += late; }
                if (const auto over = finished - end_ns; over > 0 && over <= bound) {
                    plays_to += over + std::min(kHoldLeadNs, impl_->config.playout.depth.count / 2);
                }
            }
            impl_->config.on_write_completed(WriteCompletion{core::Nanoseconds{scheduled_ns},
                core::Nanoseconds{end_ns}, core::Nanoseconds{done}, presented,
                impl_->sink->last_playback_slot_absence(),
                core::Nanoseconds{playout_offset_ns}, core::Nanoseconds{playout_end_offset_ns},
                core::Nanoseconds{plays_from}, core::Nanoseconds{plays_to}});
        }
    } else { stats.frames_silence += frames; }
    lock.lock();
    if (impl_->lossless() && std::any_of(impl_->playout.begin(), impl_->playout.end(),
            [](const auto& state) { return state.anchored; })) {
        impl_->last_write = Impl::LastWrite{end_ns, write_entered, returned};
    }
    std::erase_if(result.completed, [&](const auto& header) {
        const auto role = role_of(header.channel);
        return role && generations[role_index(*role)] != impl_->generations[role_index(*role)];
    });
    for (const auto& queue : impl_->pending) { result.pending = result.pending || !queue.empty(); }
    return result;
}

core::Result<void> AudioRouter::flush(core::Milliseconds drain_timeout) {
    if (!impl_ || impl_->closed) {
        return Error{ErrorCode::invalid_argument};
    }
    if (impl_->report.state == RouterState::degraded) {
        return Error{ErrorCode::audio_sink_unavailable};
    }
    if (impl_->flushed) {
        // Idempotent terminal flush: the route already committed its single
        // EOS drain - a repeated flush() never sends a second EOS.
        return {};
    }
    if (impl_->lossless()) {
        std::lock_guard lock(impl_->pending_mutex);
        for (std::size_t role = 0; role < kRoleCount; ++role) { (void)impl_->drain_decoder(role); }
    }
    auto pumped = pump();
    if (!pumped.has_value()) {
        // Preflight failure BEFORE the EOS drain: the route is not marked
        // terminal and stays usable.
        return pumped;
    }
    if (!impl_->sink) {
        impl_->mark_degraded("disabled: routing sink missing");
        return Error{ErrorCode::audio_sink_unavailable};
    }
    // Terminal transition happens immediately BEFORE the EOS drain: the
    // drain is the route's single end-of-stream round-trip (gate fix F1) and
    // the route is terminal from this point on - even when the drain fails.
    impl_->flushed = true;
    auto drained = impl_->sink->drain(drain_timeout);
    impl_->report.sink_probe = impl_->sink->probe_stats();
    impl_->report.render_barrier = impl_->sink->render_stats();
    if (!drained.has_value()) {
        impl_->mark_degraded("disabled: routing drain failed ("
                             + std::string{to_string(drained.error().code())}
                             + "); output no longer completing writes");
        return Error{ErrorCode::audio_sink_unavailable};
    }
    return {};
}

void AudioRouter::set_muted(bool muted) noexcept {
    if (impl_) {
        impl_->mixer.set_muted(muted);
    }
}

[[nodiscard]] bool AudioRouter::muted() const noexcept {
    return impl_ ? impl_->mixer.muted() : false;
}

void AudioRouter::set_write_deadline(std::chrono::steady_clock::time_point until) {
    if (impl_ && impl_->sink) { impl_->sink->set_write_deadline(until); }
}

void AudioRouter::close(core::Milliseconds drain_timeout) noexcept {
    if (!impl_ || impl_->closed) {
        return;
    }
    impl_->closed = true;
    for (std::size_t i = 0; i < kRoleCount; ++i) {
        if (impl_->decoders[i]) {
            impl_->decoders[i]->close();
        }
    }
    if (impl_->sink) {
        // Drain BEFORE teardown (gate fix F1): acknowledged audio is
        // confirmed rendered before the pipeline goes away; the final barrier
        // stats are captured after the drain so drains_completed is visible.
        // A flushed route already spent its single terminal EOS drain - a
        // second one is never sent (and never retried after a failed one).
        if (!impl_->flushed) {
            impl_->flushed = true;
            (void)impl_->sink->drain(drain_timeout);
        }
        impl_->report.sink_probe = impl_->sink->probe_stats();
        impl_->report.render_barrier = impl_->sink->render_stats();
    }
    impl_->sink.reset();
    std::lock_guard lock(impl_->pending_mutex);
    for (auto& queue : impl_->pending) {
        queue.clear();
    }
}

[[nodiscard]] RouterState AudioRouter::state() const noexcept {
    return impl_ ? impl_->report.state : RouterState::degraded;
}

[[nodiscard]] const RouterReport& AudioRouter::report() const noexcept {
    return impl_->report;
}

} // namespace aa::audio
