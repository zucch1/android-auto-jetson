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
#include <optional>
#include <utility>
#include <vector>

namespace aa::audio {
namespace {

constexpr std::size_t kRoleCount = 4;
constexpr std::size_t kMaxAudioPayloadBytes = std::size_t{64} * 1024;
constexpr std::size_t kMaxPendingRunsPerRole = 512;
constexpr std::size_t kMaxOutputPieceBytes = std::size_t{64} * 1024;

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
    core::Nanoseconds timestamp{};
    std::vector<std::byte> pcm{};
    std::uint64_t frames{};
    std::int64_t submitted_at_ns{};
};

// One run placed on the sample timeline at an effective (non-overlapping)
// frame range. Same-role runs are sequentialized in arrival order so no
// samples are dropped when consecutive channel chunks overlap.
struct ScheduledRun final {
    std::size_t role{0};
    std::uint64_t start_frame{0};
    std::uint64_t end_frame{0};
    const Run* run{nullptr};
};

} // namespace

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
    std::vector<std::byte> silence_scratch{};

    // Persistent sample timeline across pumps: base timestamp anchored at the
    // FIRST nonempty pump, the playback cursor (frames already committed to
    // the sink), and each role's committed end. Split and batched pumps of the
    // same schedule therefore produce identical output.
    std::optional<core::Nanoseconds> timeline_base{};
    std::uint64_t playback_cursor_frames{0};
    std::array<std::uint64_t, kRoleCount> role_end_frames{};

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
    if (impl_->pending[index].size() >= kMaxPendingRunsPerRole) {
        return Error{ErrorCode::ipc_queue_full};
    }

    // Gate fix F4: the receiver-input timestamp is taken BEFORE decoding so
    // decoder waits (e.g. the AAC drain window) are inside the path latency.
    const std::int64_t submitted_at_ns = steady_now_ns();

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
    run.timestamp = message.header.timestamp;
    run.pcm.assign(decoded.value().pcm.begin(), decoded.value().pcm.end());
    const std::uint64_t frame_bytes =
        static_cast<std::uint64_t>(impl_->config.output_format.channels) * sizeof(std::int16_t);
    run.frames = frame_bytes == 0 ? 0 : run.pcm.size() / frame_bytes;
    run.submitted_at_ns = submitted_at_ns;
    if (run.frames == 0) {
        return Error{ErrorCode::audio_format_unsupported};
    }

    impl_->pending[index].push_back(std::move(run));
    impl_->report.stats.chunks_submitted += 1;
    return {};
}

core::Result<void> AudioRouter::pump() {
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
            if (run.frames > UINT64_MAX - entry.start_frame) {
                schedule_ok = false;
                continue;
            }
            entry.end_frame = entry.start_frame + run.frames;
            std::int64_t end_ns = 0;
            if (!impl_->frame_time(entry.end_frame, base_ns, end_ns)) {
                schedule_ok = false;
                continue;
            }
            role_end = entry.end_frame;
            new_role_ends[i] = entry.end_frame;
            entry.run = &run;
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
            // slice covers the whole interval.
            const std::uint64_t offset_frames = start - entry.start_frame;
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
    for (auto& queue : impl_->pending) {
        queue.clear();
    }

    impl_->report.sink_probe = impl_->sink->probe_stats();
    impl_->report.render_barrier = impl_->sink->render_stats();
    return {};
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
    if (!impl_) {
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
            (void)impl_->sink->drain(core::Milliseconds{2000});
        }
        impl_->report.sink_probe = impl_->sink->probe_stats();
        impl_->report.render_barrier = impl_->sink->render_stats();
    }
    impl_->sink.reset();
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
