// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "ServiceDescription.hpp"
#include <aa/app/AvAudioRegistration.hpp>
#include <aa/app/WirelessLink.hpp>
#include <aa/audio/AudioRenderWorker.hpp>
#include <aa/audio/AudioRouter.hpp>
#include <aa/audio/AudioTelemetry.hpp>
#include <aa/channels/InputSink.hpp>
#include <aa/channels/MicrophoneSink.hpp>
#include <aa/channels/SensorSink.hpp>
#include <aa/config/VideoProfiles.hpp>
#include <aa/diagnostics/Diagnostics.hpp>
#include <aa/diagnostics/StdoutLog.hpp>
#include "../wired-receiver/VideoOutlet.hpp"
#include <atomic>
#include <chrono>
#include <optional>
#include <cstdio>
#include <string>
#include <string_view>
#include <utility>

namespace aa::wireless_receiver {
inline std::atomic<bool> running{true};

// One line per call, whole: the session, render worker and video release
// threads all log, and fputs + fputc from two threads could interleave.
inline void write_stdout_line(const std::string& line) {
    ::flockfile(stdout);
    std::fputs(line.c_str(), stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout);
    ::funlockfile(stdout);
}

inline void log_line(const std::string& line) {
    if (auto* writer = diagnostics::active_stdout_log.load()) {
        if (writer->submit(line)) { running.store(false); }
        return;
    }
    write_stdout_line(line);
}

inline void log_media(std::string_view kind, const std::string& line) {
    if (auto* writer = diagnostics::active_stdout_log.load()) {
        if (writer->submit_media(kind, line)) { running.store(false); }
        return;
    }
    write_stdout_line(line);
}

inline void log_line_nonblocking(const std::string& line) {
    if (auto* writer = diagnostics::active_stdout_log.load()) {
        if (writer->submit_pending(line)) { running.store(false); }
        return;
    }
    write_stdout_line(line);
}

[[nodiscard]] inline audio::AudioTelemetryCallbacks audio_telemetry() {
    return {log_line, log_media, log_line_nonblocking};
}

inline void report(const char* operation, ErrorCode code) {
    std::fprintf(stderr, "aa-wireless-receiver: %s: %.*s\n", operation,
        static_cast<int>(to_string(code).size()), to_string(code).data());
}

class DiagnosticOutput final : public diagnostics::DiagnosticSink {
public:
    explicit DiagnosticOutput(bool enabled) : enabled_(enabled) {}
    void emit_json(std::string_view json) override {
        if (enabled_) log_line_nonblocking(diagnostics::diag_event_json(json));
    }
private:
    bool enabled_;
};

// #155: wireless catch-up is configurable and defaults to off (0). With
// receiver-arrival timestamps, a normal wireless TCP coalesced burst (e.g.
// 3 x 50 ms chunks within 5 ms) reaches >90 ms placement phase without a sink
// stall and would be trimmed by the wired 90 ms phase watermark. Wired keeps
// the 90 ms default in audio::pcm_router_config(); wireless stays off unless
// explicitly configured (e.g. --audio-catch-up-watermark-ms), and only on the
// legacy arrival model (--audio-playout-ms 0).
//
// Credit window + playout (owner decision 2026-11-20): wireless audio plays on
// a steady playout clock `playout_depth` (default 120 ms) behind the arrival
// that anchored each stream, with chunks back to back (audio::PlayoutPolicy),
// and the phone may keep audio::kWirelessCreditWindow chunks unacknowledged.
// Zero restores the arrival model.
[[nodiscard]] inline audio::RouterConfig audio_config(
    audio::SinkBackend backend = audio::SinkBackend::pulse,
    core::Nanoseconds catch_up_watermark = core::Nanoseconds{0},
    core::Nanoseconds playout_depth = audio::kWirelessPlayoutDepth) {
    auto config = audio::pcm_router_config(backend, audio_telemetry());
    config.live_pcm_drift.catch_up_watermark = catch_up_watermark;
    config.playout.enabled = playout_depth.count > 0;
    if (config.playout.enabled) { config.playout.depth = playout_depth; }
    return config;
}

// Video presentation delay (credit window + playout, 2026-11-20): with a fixed
// --video-delay-ms N it is N; with the default "match" this is only the hold
// used before the first playout audio write (the configured depth) - after
// it the hold follows the audio playout offset (match_video_to_audio).
[[nodiscard]] inline std::chrono::milliseconds video_delay(std::optional<std::int64_t> video_delay_ms,
                                                           std::int64_t audio_playout_ms) {
    return std::chrono::milliseconds{video_delay_ms.value_or(audio_playout_ms)};
}

// The media endpoints outlive every per-link session; callbacks resolve the
// current session instead of capturing an address that recovery can invalidate.
struct Media final {
    wired_receiver::VideoOutlet video{log_line_nonblocking, log_media};
    audio::AudioRenderWorker& audio;
    channels::InputSink input;
    input::CapabilityDescriptor capabilities{input::CapabilityDescriptor::bench_defaults()};
    std::function<core::Result<void>(protocol::ChannelRole, std::vector<std::byte>)> send;
    channels::SensorSink sensors{capabilities, [this](std::vector<std::byte> payload) {
        return send(protocol::ChannelRole::sensors, std::move(payload));
    }};
    channels::MicrophoneSink microphone{[this](std::vector<std::byte> payload) {
        return send(protocol::ChannelRole::microphone, std::move(payload));
    }};
    explicit Media(audio::AudioRenderWorker& audio_worker) : audio(audio_worker) {}
    app::WirelessLink::Config config(
        std::vector<protocol::VideoProfile> profiles = aa::config::default_video_profiles()) {
        using protocol::ChannelRole;
        app::WirelessLink::Config config;
        config.session.input_enabled = true;
        config.session.input = capabilities;
        config.session.on_video_start = [this] { return video.request_startup_idr(); };
        config.services = service_descriptors(capabilities, std::move(profiles));
        config.sinks = {{ChannelRole::input, &input}, {ChannelRole::sensors, &sensors},
            {ChannelRole::microphone, &microphone}};
        return config;
    }
    // --video-delay-ms match (N1): hold video for the offset audio really
    // plays with (audio_write_completed's playout_offset_ns), not the
    // configured buffer: a phone delivering in groups makes audio play later
    // than the buffer, and the hold follows. `fallback` (the configured
    // depth) holds video until the first playout audio write and after a
    // second without one (a pause without a stop message).
    void match_video_to_audio(std::chrono::nanoseconds fallback) {
        video.set_presentation_delay_source([this, fallback](core::Nanoseconds content) {
            const auto offset = audio.playout_offset_at(content);
            return offset.count() > 0 ? offset : fallback;
        });
    }
    core::Result<void> bind(app::WiredSession& session) {
        audio.begin_session();
        return app::register_av_sinks(session, video, audio);
    }
};
} // namespace aa::wireless_receiver
