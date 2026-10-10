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

    // Per-role decoded runs pending on the mix timeline.
    std::array<std::deque<Run>, kRoleCount> pending{};
    std::vector<std::byte> silence_scratch{};

    void mark_degraded(std::string cause) {
        report.state = RouterState::degraded;
        report.sink.usable = false;
        // The degrade cause always wins the recorded cause: it is why the
        // route is gone (gate fix F3 - the cause must record the loss, not
        // keep the stale startup "enabled" text).
        report.sink.cause = std::move(cause);
    }

    [[nodiscard]] core::Nanoseconds frames_to_ns(std::uint64_t frames) const {
        return core::Nanoseconds{
            static_cast<std::int64_t>(frames) * 1'000'000'000LL
            / static_cast<std::int64_t>(config.output_format.sample_rate_hz)};
    }

    [[nodiscard]] std::uint64_t ns_to_frames(core::Nanoseconds ns) const {
        return static_cast<std::uint64_t>(
            (ns.count * static_cast<std::int64_t>(config.output_format.sample_rate_hz))
            / 1'000'000'000LL);
    }
};

core::Result<AudioRouter> AudioRouter::open(const RouterConfig& config) {
    if (config.output_format.bits_per_sample != 16 || config.output_format.channels == 0
        || config.output_format.sample_rate_hz == 0) {
        return core::Result<AudioRouter>{Error{ErrorCode::audio_format_unsupported}};
    }

    auto impl = std::make_unique<Impl>();
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
        // No usable route: the router opens DEGRADED with the recorded cause.
        // Every submit()/pump() then fails typed - never silence pretending
        // success, never a crash.
        impl->mark_degraded(outcome.report.cause.empty()
                                ? "disabled: no PipeWire-capable sink element usable"
                                : outcome.report.cause);
    } else {
        impl->sink = std::move(outcome.sink);
    }

    return core::Result<AudioRouter>{AudioRouter{std::move(impl)}};
}

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
    if (!impl_->sink) {
        impl_->mark_degraded("disabled: routing sink missing");
        return Error{ErrorCode::audio_sink_unavailable};
    }

    // Gate fix F2: mix on a SAMPLE TIMELINE. Every pending run occupies a
    // frame range on one timeline anchored at the earliest run timestamp;
    // every run start/end is a boundary; each interval mixes exactly the
    // frames present in it (staggered starts get a ducked overlap instead of
    // being sequentialized into separate blocks).
    std::optional<core::Nanoseconds> timeline_origin;
    for (const auto& queue : impl_->pending) {
        for (const Run& run : queue) {
            if (!timeline_origin.has_value() || run.timestamp < *timeline_origin) {
                timeline_origin = run.timestamp;
            }
        }
    }
    if (!timeline_origin.has_value()) {
        impl_->report.sink_probe = impl_->sink->probe_stats();
        impl_->report.render_barrier = impl_->sink->render_stats();
        return {};
    }

    std::vector<ScheduledRun> scheduled;
    for (std::size_t i = 0; i < kRoleCount; ++i) {
        std::uint64_t role_end = 0;
        for (const Run& run : impl_->pending[i]) {
            ScheduledRun entry;
            entry.role = i;
            const core::Nanoseconds offset{run.timestamp.count - timeline_origin->count};
            // Same-role runs sequentialize in arrival order: a later chunk
            // starts no earlier than the previous one ends, so overlapping
            // channel chunks never drop samples or share a mixer slot. Across
            // roles the recorded timestamps place the runs (stagger included).
            entry.start_frame = std::max(impl_->ns_to_frames(offset), role_end);
            entry.end_frame = entry.start_frame + run.frames;
            role_end = entry.end_frame;
            entry.run = &run;
            scheduled.push_back(entry);
        }
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
    boundaries.reserve(scheduled.size() * 2 + 1);
    boundaries.push_back(0);
    for (const ScheduledRun& entry : scheduled) {
        boundaries.push_back(entry.start_frame);
        boundaries.push_back(entry.end_frame);
    }
    boundaries.push_back(timeline_frames);
    std::sort(boundaries.begin(), boundaries.end());
    boundaries.erase(std::unique(boundaries.begin(), boundaries.end()), boundaries.end());

    for (std::size_t b = 0; b + 1 < boundaries.size(); ++b) {
        const std::uint64_t start = boundaries[b];
        const std::uint64_t end = boundaries[b + 1];
        if (end <= start) {
            continue;
        }
        const std::uint64_t frames = end - start;

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
            const std::size_t frame_bytes = static_cast<std::size_t>(
                impl_->config.output_format.channels * sizeof(std::int16_t));
            const std::size_t byte_offset =
                static_cast<std::size_t>(offset_frames) * frame_bytes;
            const std::size_t byte_count = static_cast<std::size_t>(frames) * frame_bytes;
            materialized[entry.role].role = static_cast<Role>(entry.role);
            materialized[entry.role].format = impl_->config.output_format;
            materialized[entry.role].timestamp = core::Nanoseconds{
                entry.run->timestamp.count + impl_->frames_to_ns(offset_frames).count};
            materialized[entry.role].pcm =
                std::span<const std::byte>{entry.run->pcm}.subspan(byte_offset, byte_count);
            pieces[entry.role] = &materialized[entry.role];
            if (first || entry.run->submitted_at_ns < oldest_submit_ns) {
                oldest_submit_ns = entry.run->submitted_at_ns;
                first = false;
            }
        }

        const core::Nanoseconds interval_timestamp{
            timeline_origin->count + impl_->frames_to_ns(start).count};
        bool any_input = false;
        for (const PcmChunk* piece : pieces) {
            if (piece != nullptr) {
                any_input = true;
                break;
            }
        }

        MixedChunk to_write;
        if (any_input) {
            const MixFrame frame{interval_timestamp, frames, pieces};
            auto mixed = impl_->mixer.mix(frame);
            if (!mixed.has_value()) {
                return mixed.error();
            }
            to_write = mixed.value();
        } else {
            // Gap on the timeline (no run covers this interval): emit
            // correctly sized silence so the output duration stays truthful
            // and later runs are never blocked by an empty interval.
            const std::size_t byte_count = static_cast<std::size_t>(frames)
                * impl_->config.output_format.channels * sizeof(std::int16_t);
            impl_->silence_scratch.assign(byte_count, std::byte{0});
            to_write.format = impl_->config.output_format;
            to_write.timestamp = interval_timestamp;
            to_write.pcm = std::span<const std::byte>{impl_->silence_scratch};
        }

        auto written = impl_->sink->write(to_write);
        const std::int64_t write_done_ns = steady_now_ns();
        if (!written.has_value()) {
            impl_->report.sink_probe = impl_->sink->probe_stats();
            impl_->report.render_barrier = impl_->sink->render_stats();
            // Gate fix F3: a routing write failure is an output loss - the
            // router degrades typed with the recorded cause, never a silent
            // "active" state with dropped audio.
            impl_->mark_degraded("disabled: routing write failed ("
                                 + std::string{to_string(written.error().code())}
                                 + "); output no longer accepting audio");
            return Error{ErrorCode::audio_sink_unavailable};
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
    // partitioned by the boundary set).
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
    auto pumped = pump();
    if (!pumped.has_value()) {
        return pumped;
    }
    if (!impl_->sink) {
        impl_->mark_degraded("disabled: routing sink missing");
        return Error{ErrorCode::audio_sink_unavailable};
    }
    // Bounded render drain: everything acknowledged must be confirmed
    // rendered before the caller tears down (gate fix F1).
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
        (void)impl_->sink->drain(core::Milliseconds{2000});
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
