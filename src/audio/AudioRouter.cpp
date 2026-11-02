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
};

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
    if (!policy_.enabled || rate_ == 0 || phase <= policy_.high_watermark) {
        return 0;
    }
    const core::Nanoseconds excess{phase.count - policy_.target.count};
    const core::Nanoseconds budget = excess < policy_.max_trim ? excess : policy_.max_trim;
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

    if (request == SinkBackend::pipewire || request == SinkBackend::automatic) {
        for (const auto& candidate : report.candidates) {
            if (candidate.element == "pipewiresink" && candidate.viable) {
                chosen = &candidate;
                break;
            }
        }
    }
    if (chosen == nullptr && (request == SinkBackend::pulse || request == SinkBackend::automatic)) {
        for (const auto& candidate : report.candidates) {
            if (candidate.element == "pulsesink" && candidate.viable) {
                chosen = &candidate;
                break;
            }
        }
    }

    if (chosen == nullptr) {
        selection.usable = false;
        selection.cause = "disabled:";
        bool first = true;
        for (const auto& candidate : report.candidates) {
            const bool wanted = request == SinkBackend::automatic
                || (request == SinkBackend::pipewire && candidate.element == "pipewiresink")
                || (request == SinkBackend::pulse && candidate.element == "pulsesink");
            if (!wanted) {
                continue;
            }
            if (!first) {
                selection.cause += ";";
            }
            first = false;
            selection.cause += " " + candidate.element + " "
                + (candidate.available ? "not viable" : "absent") + " (" + candidate.cause + ")";
        }
        return selection;
    }

    selection.element = chosen->element;
    selection.usable = true;
    if (request == SinkBackend::automatic && chosen->element != "pipewiresink") {
        selection.cause =
            "disabled: pipewiresink not usable, falling back to pulsesink (recorded fallback)";
    } else {
        selection.cause = "enabled: " + chosen->element + " runtime-detected viable";
    }
    return selection;
}

struct AudioRouter::Impl final {
    RouterConfig config{};
    AudioMixer mixer{};
    std::array<std::unique_ptr<AudioDecoder>, kRoleCount> decoders{};
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
    // Disjoint, sorted [start, end) steady-clock intervals during which the
    // owner withheld peer credit (note_backpressure). Empty for every caller
    // that never records one, which keeps the strict gap policy unchanged.
    std::vector<std::pair<std::int64_t, std::int64_t>> backpressure{};
    struct IdleAuthorization final {
        std::pair<std::int64_t, std::int64_t> interval;
        std::optional<std::uint64_t> prefix_end{};
    };
    std::optional<IdleAuthorization> idle{};

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

core::Result<AudioRouter> open_router_with(const RouterConfig& config,
                                           std::unique_ptr<detail::ObservedAudioSink> injected) {
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
    impl->mixer = AudioMixer{config.mix};
    impl->report.sink.requested = config.backend;

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
        detail::SinkOpenOutcome outcome = detail::open_default_sink(request);

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

    auto decoded = impl_->decoders[index]->decode(chunk);
    if (!decoded.has_value()) {
        return decoded.error();
    }

    Run run;
    run.header = message.header;
    run.timestamp = message.header.timestamp;
    run.pcm.assign(decoded.value().pcm.begin(), decoded.value().pcm.end());
    const std::uint64_t frame_bytes =
        static_cast<std::uint64_t>(impl_->config.output_format.channels) * sizeof(std::int16_t);
    run.frames = frame_bytes == 0 ? 0 : run.pcm.size() / frame_bytes;
    run.submitted_at_ns = submitted_at_ns;
    if (run.frames == 0) {
        return Error{ErrorCode::audio_format_unsupported};
    }

    impl_->report.stats.frames_decoded += run.frames;
    impl_->pending[index].push_back(std::move(run));
    impl_->report.stats.chunks_submitted += 1;
    return {};
}

core::Result<void> AudioRouter::pump() {
    if (impl_ && impl_->incremental) {
        for (;;) {
            auto step = render_step();
            if (!step) { return step.error(); }
            if (!step.value().pending) { return {}; }
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
    std::uint64_t planned_retained = 0;
    std::uint64_t planned_trimmed = 0;
    bool schedule_ok = true;
    for (std::size_t i = 0; i < kRoleCount; ++i) {
        std::uint64_t role_end = impl_->role_end_frames[i];
        for (const Run& run : impl_->pending[i]) {
            std::int64_t intended = 0;
            if (!impl_->ns_to_frames_signed(core::Nanoseconds{run.timestamp.count - base_ns},
                                            intended)) {
                schedule_ok = false;
                continue;
            }
            if (intended < static_cast<std::int64_t>(cursor)) {
                const auto late =
                    static_cast<std::uint64_t>(static_cast<std::int64_t>(cursor) - intended);
                if (late > cap_frames) {
                    schedule_ok = false;
                }
            }
            ScheduledRun entry;
            entry.role = i;
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
            entry.head_trim_frames = impl_->drift.trim_frames(phase, run.frames);
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
            entry.run = &run;
            planned_retained += retained;
            planned_trimmed += entry.head_trim_frames;
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
                    core::Nanoseconds{write_done_ns}});
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
    impl_->report.stats.frames_retained += planned_retained;
    impl_->report.stats.frames_trimmed += planned_trimmed;
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
    impl_->role_end_frames[role_index(*index)] = impl_->playback_cursor_frames;
    ++impl_->generations[role_index(*index)];
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
        for (const auto& queue : impl_->pending) {
            for (const auto& run : queue) {
                if (!base || run.timestamp < *base) { base = run.timestamp; }
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
        for (const auto& run : impl_->pending[role]) {
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
                if (!run.preflighted && intended < static_cast<std::int64_t>(cursor)
                    && static_cast<std::uint64_t>(static_cast<std::int64_t>(cursor) - intended) > cap) {
                    fail(RenderFailureReason::late);
                    valid = false;
                    break;
                }
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
                entry.head_trim_frames = impl_->drift.trim_frames(
                    core::Nanoseconds{placed_ns - run.timestamp.count}, run.frames);
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
    const auto now_ns = steady_now_ns();
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
            impl_->role_end_frames[role] = head.end_frame;
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
                if (range.idle) { impl_->report.stats.frames_idle_skipped += skipped; }
                else { impl_->report.stats.frames_skipped += skipped; }
                result.pending = true;
                return result;
            }
            if (range.first > cursor) { end = std::min(end, range.first); }
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
    for (std::size_t role = 0; role < kRoleCount; ++role) {
        if (!heads[role] || heads[role]->start_frame > cursor) { continue; }
        const auto& head = *heads[role];
        auto& run = impl_->pending[role].front();
        if (!run.placed) {
            run.placed = head.start_frame;
            run.trimmed = head.head_trim_frames;
            impl_->report.stats.frames_trimmed += run.trimmed;
        }
        materialized[role] = PcmChunk{static_cast<Role>(role), impl_->config.output_format,
            core::Nanoseconds{scheduled_ns}, std::span<const std::byte>{run.pcm}.subspan(
                static_cast<std::size_t>(head.head_trim_frames) * frame_bytes,
                static_cast<std::size_t>(frames) * frame_bytes)};
        pieces[role] = &materialized[role];
        oldest = oldest ? std::min(*oldest, run.submitted_at_ns) : run.submitted_at_ns;
        result.contributors.push_back(run.header);
    }
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
        if (impl_->config.on_write_completed) {
            impl_->config.on_write_completed(WriteCompletion{core::Nanoseconds{scheduled_ns},
                core::Nanoseconds{end_ns}, core::Nanoseconds{done}});
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
