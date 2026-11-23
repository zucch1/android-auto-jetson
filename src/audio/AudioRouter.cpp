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
// The rate itself moves by at most this much per second (0.1% per A26 chunk).
constexpr double kRateSlewPpmPerSecond = 25'000.0;
// Carry of correction that short runs could not take, in frames.
constexpr double kMaxRateCarryFrames = 64.0;
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
    // Lossless steady: trim_frames keeps the backlog under the emergency bound.
    bool overflow{false};
    // Lossless anchor: only the decoder held the role back; it still owes.
    bool decoder_held{false};
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
    struct PlayoutRole final {
        bool anchored{false};
        std::deque<std::pair<std::int64_t, std::int64_t>> depths{};
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
        // Lossless: output of already committed parents the decoder still
        // holds (a resampler split), in output frames.
        std::uint64_t decoder_owed{};
    };
    // Re-anchors, resumes and underruns since the last take_playout_events().
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
    // pump()/flush(): everything pending is written, nothing is held.
    bool draining{};
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
        const double depth_ms = static_cast<double>(config.playout.depth.count) / 1e6;
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
        const double wanted = floor.sustained ? wanted_rate_ppm(floor.low) : 0.0;
        const double slew = kRateSlewPpmPerSecond * static_cast<double>(run.frames)
            / static_cast<double>(config.output_format.sample_rate_hz);
        state.rate_ppm += std::clamp(wanted - state.rate_ppm, -slew, slew);
        state.rate_carry = std::clamp(
            state.rate_carry + static_cast<double>(run.frames) * state.rate_ppm / 1e6,
            -kMaxRateCarryFrames, kMaxRateCarryFrames);
        const auto limit = detail::max_warp_frames(run.frames);
        const auto removed = std::clamp(static_cast<std::int64_t>(state.rate_carry), -limit, limit);
        if (removed == 0) { return; }
        run.pcm = detail::warp_frames(run.pcm, config.output_format.channels, -removed);
        run.frames = static_cast<std::uint64_t>(static_cast<std::int64_t>(run.frames) - removed);
        plan.warped_frames = -removed;
        state.rate_carry -= static_cast<double>(removed);
    }

    // End (output clock) and end offset of the last reported write
    // (WriteCompletion): the reported offset is slewed from it.
    std::optional<std::pair<std::int64_t, std::int64_t>> last_reported{};

    void reset_playout(std::size_t role) noexcept {
        playout[role] = PlayoutRole{};
        arrival_bursts[role] = ArrivalBurst{};
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
        for (const auto& [at, value] : state.depths) {
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
        for (const auto& [at, value] : state.depths) {
            if (at >= arrival - span) { high = std::max(high, value); }
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
        const std::int64_t depth = config.playout.depth.count;
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
            // a backlog beyond the emergency bound is cut.
            plan.start_frame = slot_frame;
            plan.slot_frame = slot_frame;
            const auto bound = config.playout.emergency_backlog.count;
            if (plan.depth_ns > bound) {
                plan.trim_frames = std::min(
                    run.frames, ns_to_frames(core::Nanoseconds{plan.depth_ns - bound}));
                plan.overflow = plan.trim_frames != 0;
            }
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

    // Commits a placed run's playout decision: role state, window and stats.
    void commit_playout(std::size_t role, Run& run, const ScheduledRun& entry, std::int64_t base_ns) {
        if (!playout_enabled() || !run.playout) { return; }
        const auto& plan = *run.playout;
        auto& state = playout[role];
        auto& stats = report.stats;
        std::int64_t start_ns = 0;
        if (!frame_time(entry.start_frame, base_ns, start_ns)) { start_ns = run.timestamp.count; }
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
            for (auto& [at, value] : state.depths) { value += adjust; }
            state.depths.emplace_back(run.timestamp.count, plan.depth_ns + adjust);
            stats.frames_padded += pad;
            if (plan.shed) { stats.frames_shed += entry.head_trim_frames; }
            if (plan.overflow) {
                ++stats.playout_overflows;
                stats.frames_overflowed += entry.head_trim_frames;
            }
            if (plan.warped_frames < 0) {
                stats.frames_compressed += static_cast<std::uint64_t>(-plan.warped_frames);
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
            state.decoder_owed = owed_after(plan.decoder_held ? state.decoder_owed : 0, run);
            const auto depth = start_ns - run.timestamp.count;
            state.depths.emplace_back(run.timestamp.count, depth);
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
            constexpr std::size_t kMaxPlayoutEvents = 64;
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
            for (const auto& [at, value] : state.depths) {
                if (at >= window_from) { best = std::max(best, value); }
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
        while (!state.depths.empty() && state.depths.front().first < from) { state.depths.pop_front(); }
        constexpr std::size_t kMaxPlayoutDepths = 512;
        while (state.depths.size() > kMaxPlayoutDepths) { state.depths.pop_front(); }
        state.anchored = true;
        state.last_arrival = std::max(run.timestamp.count, state.last_arrival.value_or(run.timestamp.count));
        stats.playout_depth = core::Nanoseconds{plan.depth_ns};
        // The receiver's offset: the largest over its playing roles (a write
        // mixing roles reports the largest of its contributors).
        std::int64_t offset = 0;
        for (const auto& other : playout) {
            if (other.anchored) { offset = std::max(offset, other.offset_ns); }
        }
        if (const auto media = media_offset(start_ns)) { offset = *media; }
        stats.playout_offset = core::Nanoseconds{offset};
    }

    // Lossless: the offset of the media stream, the one video is in sync with,
    // while it plays at output time `at_ns`. A prompt placed behind it (a
    // resampler split) must not pull the video hold away from the music.
    [[nodiscard]] std::optional<std::int64_t> media_offset(std::int64_t at_ns) const noexcept {
        const auto& media = playout[role_index(Role::media)];
        if (!lossless() || !media.anchored || !media.last_arrival
            || at_ns - *media.last_arrival - media.offset_ns > kHoldExpectNs) { return std::nullopt; }
        return media.offset_ns;
    }

    // Lossless: true when another role that is playing has not delivered its
    // next chunk yet, so a frame mixed now would leave it behind.
    [[nodiscard]] bool awaiting_role(std::int64_t at_ns) const noexcept {
        for (std::size_t role = 0; role < kRoleCount; ++role) {
            const auto& state = playout[role];
            if (state.anchored && pending[role].empty() && state.last_arrival
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
                                      core::Nanoseconds receiver_input_at) {
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
        auto decoded = impl_->decoders[index]->decode(chunk);
        if (!decoded.has_value()) {
            return decoded.error();
        }
        run.pcm.assign(decoded.value().pcm.begin(), decoded.value().pcm.end());
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
        const std::uint64_t input_frame_bytes =
            static_cast<std::uint64_t>(input.channels) * (input.bits_per_sample / 8U);
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
    if (run.frames == 0) {
        return Error{ErrorCode::audio_format_unsupported};
    }

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
    auto base = impl_->timeline_base;
    if (!base) {
        // A playout stream's timeline starts at its anchor (arrival + depth),
        // so the buffer fills without synthesized silence.
        const auto lead = impl_->playout_enabled() ? impl_->config.playout.depth.count : 0;
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
                // Playout: back to back from the role's stream, re-anchored
                // at arrival + depth on a fresh stream or an underrun.
                const auto slot_frame = std::max(cursor, role_end);
                PlayoutPlan plan;
                if (run.playout) {
                    plan = *run.playout;
                } else if (!impl_->plan_playout(role, run, slot_frame, role_end + decoder_owed,
                                                base->count, anchored, plan)) {
                    diagnostic.time_conversion_frame = slot_frame;
                    fail(RenderFailureReason::placement_time_conversion);
                    valid = false;
                    break;
                } else if (&run == &impl_->pending[role].front()) {
                    if (impl_->lossless() && plan.mode == PlayoutPlan::Mode::steady
                        && !plan.backward && !plan.overflow) {
                        impl_->apply_rate(role, run, plan);
                    }
                    run.playout = plan;
                }
                const bool continues = plan.mode == PlayoutPlan::Mode::steady || plan.decoder_held;
                decoder_owed = Impl::owed_after(continues ? decoder_owed : 0, run);
                if (plan.backward) {
                    diagnostic.start.reset();
                    fail(RenderFailureReason::late);
                    valid = false;
                    break;
                }
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
                        fail(RenderFailureReason::intended_time_conversion);
                        valid = false;
                        break;
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
                entry.intended = static_cast<std::int64_t>(entry.start_frame);
                diagnostic.intended = entry.intended;
                diagnostic.start = entry.start_frame;
                anchored = true;
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
    auto end = cursor + quantum;
    bool any = false;
    for (const auto& head : planned) {
        const auto role = head.role;
        if (heads[role]) { continue; }
        auto& queue = impl_->pending[role];
        if (head.start_frame == head.end_frame) {
            impl_->report.stats.frames_trimmed += head.head_trim_frames;
            impl_->report.stats.frames_caught_up += head.caught_up_frames;
            impl_->commit_playout(role, queue.front(), head, base->count);
            impl_->role_end_frames[role] = head.end_frame;
            if (head.intended) {
                const auto run_source_end = source_end_frame(*head.intended, head.run->content_frames);
                impl_->source_frontier_frames = std::max(impl_->source_frontier_frames, run_source_end);
                impl_->role_source_frontier_frames[role] =
                    std::max(impl_->role_source_frontier_frames[role], run_source_end);
            }
            result.completed.push_back(queue.front().header);
            queue.pop_front();
            continue;
        }
        // Never cross an earlier queued head/end to reach a future role.
        end = std::min(end, head.start_frame > cursor ? head.start_frame : head.end_frame);
        heads[role] = head;
        any = true;
    }
    if (!any) { return result; }
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
        end = std::min(end, cursor + std::max<std::uint64_t>(1, std::min(ahead, quantum)));
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
    const auto frames = end - cursor;
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
    std::array<PcmChunk, kRoleCount> materialized{};
    std::array<const PcmChunk*, kRoleCount> pieces{};
    std::optional<std::int64_t> oldest;
    std::int64_t playout_offset_ns = 0;
    for (std::size_t role = 0; role < kRoleCount; ++role) {
        if (!heads[role] || heads[role]->start_frame > cursor) { continue; }
        const auto& head = *heads[role];
        auto& run = impl_->pending[role].front();
        if (!run.placed) {
            run.placed = head.start_frame;
            run.trimmed = head.head_trim_frames;
            impl_->report.stats.frames_trimmed += run.trimmed;
            impl_->report.stats.frames_caught_up += head.caught_up_frames;
            impl_->commit_playout(role, run, head, base->count);
            if (head.intended) {
                const auto run_source_end = source_end_frame(*head.intended, run.content_frames);
                impl_->source_frontier_frames = std::max(impl_->source_frontier_frames, run_source_end);
                impl_->role_source_frontier_frames[role] =
                    std::max(impl_->role_source_frontier_frames[role], run_source_end);
            }
        }
        materialized[role] = PcmChunk{static_cast<Role>(role), impl_->config.output_format,
            core::Nanoseconds{scheduled_ns}, std::span<const std::byte>{run.pcm}.subspan(
                static_cast<std::size_t>(head.head_trim_frames) * frame_bytes,
                static_cast<std::size_t>(frames) * frame_bytes)};
        pieces[role] = &materialized[role];
        oldest = oldest ? std::min(*oldest, run.submitted_at_ns) : run.submitted_at_ns;
        playout_offset_ns = std::max(playout_offset_ns, run.playout_offset_ns);
        result.contributors.push_back(run.header);
    }
    if (const auto media = impl_->media_offset(scheduled_ns)) { playout_offset_ns = *media; }
    // Materialize before detaching: cancel_pending can now free EVERY parent
    // remainder while this single, bounded mixed piece is inside sink->write.
    std::vector<std::byte> output;
    if (oldest) {
        auto mixed = impl_->mixer.mix(MixFrame{core::Nanoseconds{scheduled_ns}, frames, pieces});
        if (!mixed) { fail(RenderFailureReason::mixer_error, mixed.error().code()); return mixed.error(); }
        output.assign(mixed.value().pcm.begin(), mixed.value().pcm.end());
    } else {
        output.assign(static_cast<std::size_t>(frames) * frame_bytes, std::byte{0});
    }
    for (std::size_t role = 0; role < kRoleCount; ++role) {
        if (!pieces[role]) { continue; }
        impl_->idle.reset();
        auto& run = impl_->pending[role].front();
        run.consumed += frames;
        impl_->role_end_frames[role] = end;
        if (run.trimmed + run.consumed == run.frames) {
            result.completed.push_back(run.header);
            result.delivered.push_back(run.header);
            impl_->pending[role].pop_front();
        }
    }
    impl_->timeline_base = base;
    impl_->playback_cursor_frames = end;
    lock.unlock();
    const auto written = impl_->sink->write(MixedChunk{
        impl_->config.output_format, core::Nanoseconds{scheduled_ns}, output});
    const auto done = steady_now_ns();
    impl_->report.sink_probe = impl_->sink->probe_stats();
    impl_->report.render_barrier = impl_->sink->render_stats();
    if (!written) {
        lock.lock();
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
    stats.frames_retained += frames * result.contributors.size();
    if (oldest) {
        stats.last_path_latency = core::Nanoseconds{done - *oldest};
        stats.max_path_latency = std::max(stats.max_path_latency, stats.last_path_latency);
        stats.last_completion_skew = core::Nanoseconds{done - scheduled_ns};
        stats.max_completion_skew = std::max(stats.max_completion_skew, stats.last_completion_skew);
        std::int64_t playout_end_offset_ns = playout_offset_ns;
        if (impl_->playout_enabled()) {
            // The reported offset moves toward its target by at most half the
            // output time that passes, in both directions (Neon P1-1): content
            // then advances at between half and one and a half times the
            // playout rate, so the reported content interval never starts
            // before the last one ended (the collector never drops a write,
            // Neon P2-4), and the video hold that follows it, t + O(t), only
            // ever increases - frames are never pinned behind an earlier due
            // (no freeze, then burst). Across an output gap (an underrun's
            // skip and refill) it may fall by half the gap at once.
            //
            // last_reported is shared with cancel_pending() on the session
            // thread, so it is read and written under pending_mutex (Neon
            // P1-2), and a write that completes after the last stream was
            // cancelled (no role anchored; the gate is router-wide, as
            // cancel_pending's reset is) no longer seeds the next stream's
            // slew. The lock is
            // released again before on_write_completed: that callback takes
            // the render worker's mutex, which is ordered before this one.
            const auto target = std::max<std::int64_t>(0, playout_offset_ns);
            const auto length = end_ns - scheduled_ns;
            auto start_offset = target;
            lock.lock();
            if (impl_->last_reported) {
                const auto [previous_end, previous_offset] = *impl_->last_reported;
                const auto gap = std::max<std::int64_t>(0, scheduled_ns - previous_end);
                start_offset = std::clamp(target, previous_offset - gap / 2, previous_offset + gap);
            }
            start_offset = std::max<std::int64_t>(0, start_offset);
            const auto step = std::clamp(target - start_offset, -length / 2, length / 2);
            playout_offset_ns = start_offset;
            playout_end_offset_ns = std::max<std::int64_t>(0, start_offset + step);
            if (std::any_of(impl_->playout.begin(), impl_->playout.end(),
                            [](const auto& state) { return state.anchored; })) {
                impl_->last_reported = std::pair{end_ns, playout_end_offset_ns};
                stats.playout_reported_offset = core::Nanoseconds{playout_end_offset_ns};
            }
            lock.unlock();
        }
        if (impl_->config.on_write_completed) {
            impl_->config.on_write_completed(WriteCompletion{core::Nanoseconds{scheduled_ns},
                core::Nanoseconds{end_ns}, core::Nanoseconds{done},
                presented_at(impl_->sink->last_playback_slot(), done),
                impl_->sink->last_playback_slot_absence(),
                core::Nanoseconds{playout_offset_ns}, core::Nanoseconds{playout_end_offset_ns}});
        }
    } else { stats.frames_silence += frames; }
    lock.lock();
    std::erase_if(result.completed, [&](const auto& header) {
        const auto role = role_of(header.channel);
        return role && generations[role_index(*role)] != impl_->generations[role_index(*role)];
    });
    for (const auto& queue : impl_->pending) { result.pending = result.pending || !queue.empty(); }
    return result;
}

core::Result<void> AudioRouter::flush() {
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
    auto drained = impl_->sink->drain(core::Milliseconds{2000});
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

void AudioRouter::close() noexcept {
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
            (void)impl_->sink->drain(core::Milliseconds{2000});
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
