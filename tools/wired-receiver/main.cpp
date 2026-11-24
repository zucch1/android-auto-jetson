// SPDX-License-Identifier: GPL-3.0-or-later
#include "../json_stdout.hpp"
#include "../media_playout_options.hpp"
#include "../video_capabilities.hpp"
//
// Wired end-to-end receiver entry point (task 30 phase-B harness driver): the
// composed receiver for the real approved phone over USB/AOA. It wires the
// task-28 USB/AOA link (open_wired_link: admit_via(TrustStore)) into the
// task-30 integrator (aa::app::WiredSession) and serves the task-22 video
// socket, the task-24 input socket, the task-25 audio router and the task-21
// D-Bus control contract that the dummy display (tools/dummy-display)
// registers against. It implements no module internals.
//
// TEST/QA TOOLING: this drives the live qualification round (tools/wired-qa);
// the production service packaging is todo 41. Pairing is deliberately NOT
// opened here: the approved-phones store must be pre-loaded (fail closed, task
// 27 semantics); only ForgetPhone mutates it (mid-session revoke).
//
// Threading: one SESSION thread runs every transport interaction (the
// Transport contract: the session thread is the only caller of
// open/send/receive/close). Display input events are queued by the input
// thread and drained by the session thread between receives (input is
// low-rate; during projection a media frame bounds the drain latency). The
// main thread owns the D-Bus dispatch loop, video-consumer accept and the run
// budget. The session thread is the only caller of WiredSession methods.

#include <aa/app/AvAudioRegistration.hpp>
#include <aa/app/WiredEnrolment.hpp>
#include <aa/app/WiredLink.hpp>
#include <aa/app/WiredPhoneControl.hpp>
#include <aa/app/WiredSession.hpp>
#include <aa/audio/AudioRenderWorker.hpp>
#include <aa/audio/AudioRouter.hpp>
#include <aa/audio/AudioTelemetry.hpp>
#include <aa/audio/AvCreditJson.hpp>
#include <aa/channels/SensorSink.hpp>
#include <aa/channels/MicrophoneSink.hpp>
#include <aa/channels/InputSink.hpp>
#include <aa/diagnostics/Diagnostics.hpp>
#include <aa/ipc/ControlBusService.hpp>
#include <aa/ipc/ControlService.hpp>
#include <aa/ipc/IdrRecovery.hpp>
#include <aa/ipc/InputChannel.hpp>
#include <aa/ipc/InputSocket.hpp>
#include <aa/ipc/PairingGate.hpp>
#include <aa/ipc/VideoChannel.hpp>
#include <aa/ipc/VideoSocket.hpp>
#include <aa/protocol/Messages.hpp>
#include <aa/protocol/Protocol.hpp>
#include <aa/config/VideoProfiles.hpp>
#include <aa/session/Events.hpp>
#include <aa/session/Session.hpp>
#include <aa/transport/TlsTransport.hpp>
#include <aa/trust/PairingPolicy.hpp>
#include <aa/trust/Store.hpp>

#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <system_error>
#include <thread>
#include <variant>
#include <vector>

#include <signal.h>
#include "VideoOutlet.hpp"
#include "StdoutLog.hpp"
#include "AvCreditJson.hpp"

namespace {

using aa::core::Milliseconds;
using aa::wired_receiver::VideoOutlet;

std::atomic<bool> g_running{true};

void log_line(const std::string &json) {
    auto *writer = aa::wired_receiver::active_stdout_log.load();
    if (!writer) { std::abort(); }
    if (writer->submit(json)) { g_running.store(false); }
}

// Phone-driven media telemetry (audit L13): rate-limited per kind and never
// blocks the calling media thread on the stdout sink. Dropped records are
// counted and reported in-band as media_telemetry_dropped.
void log_media(std::string_view kind, const std::string &json) {
    auto *writer = aa::wired_receiver::active_stdout_log.load();
    if (!writer) { std::abort(); }
    if (writer->submit_media(kind, aa::wired_receiver::with_emit_ns(json))) { g_running.store(false); }
}

// Every non-blocking record carries its emission time ("emit_ns", CLOCK_MONOTONIC
// at the source, #90 review): a record the log delivers late still tells a
// timing gate when it happened, never when it reached stdout.

// Steady-state runtime and control records that no gate keys state on (#90):
// never blocks a media, audio, or control thread on a full queue behind a
// stalled stdout sink. Dropped records are counted and reported in-band as
// runtime_log_dropped.
void log_runtime(const std::string &json) {
    auto *writer = aa::wired_receiver::active_stdout_log.load();
    if (!writer) { std::abort(); }
    if (writer->submit_runtime(aa::wired_receiver::with_emit_ns(json))) { g_running.store(false); }
}

// One-shot lifecycle / state-transition / control records that the projection
// collector and drive smoke key state on (`active`, runtime `err`,
// audio_stall_bypass begin/end, sink reopen, idr_*): never blocks the caller and
// is never dropped for backpressure. A full queue parks the record in the log's
// single pending_lossless FIFO; overflowing it latches ENOBUFS and stops the
// receiver (StdoutLog::submit_pending).
void log_line_nonblocking(const std::string &json) {
    auto *writer = aa::wired_receiver::active_stdout_log.load();
    if (!writer) { std::abort(); }
    if (writer->submit_pending(aa::wired_receiver::with_emit_ns(json))) { g_running.store(false); }
}

aa::audio::AudioTelemetryCallbacks wired_telemetry() {
    return {log_line, log_media, log_line_nonblocking};
}

// Run provenance (#60): the routing element a route actually uses, logged at
// the first open and again after every successful sink reopen (an automatic
// backend can select a different element on reopen).
void log_sink_selected(const aa::audio::SinkReport &selected, std::string_view origin) {
    aa::audio::log_sink_selected(selected, origin, wired_telemetry());
}

void on_signal(int) { g_running.store(false); }

// The handler only publishes the shutdown flag. A normal thread wakes USB waits,
// even if the D-Bus dispatcher is inside a synchronous ForgetPhone call.
std::jthread cancel_open_on_shutdown(aa::core::CancellationSource &source) {
    return std::jthread([&source](const std::stop_token &stop) {
        std::mutex mutex;
        std::condition_variable_any cv;
        std::unique_lock lock(mutex);
        while (g_running.load() && !stop.stop_requested()) {
            cv.wait_for(lock, stop, std::chrono::milliseconds{25}, [] { return false; });
        }
        if (!g_running.load()) {
            source.request_stop();
        }
    });
}

using aa::audio::AudioDispatchObservation;
using aa::audio::AudioFailureOrigin;
using aa::audio::AudioRejectionSnapshot;
using aa::audio::FailureContext;
using aa::audio::IdleDisposition;
using aa::audio::WorkerAdmission;
using aa::audio::WorkerFailureObservation;
using aa::audio::WorkerIdleObservation;
using aa::audio::WorkerQueueObservation;
using aa::audio::WorkerSuccessfulStep;
using aa::audio::audio_failure_event_json;
using aa::audio::worker_admission_json;
using aa::app::poll_error_json;

class StdoutDiagnostics final : public aa::diagnostics::DiagnosticSink {
  public:
    // Only periodic transport_stats is droppable. Every other diag kind is a
    // rare state record a gate reads (session_state_changed and reconnect for
    // no_session_drop and recovery-QA dip/retry attribution, decode_stats,
    // pairing, capability, helper calls), so it goes through the lossless slot.
    void emit_json(std::string_view redacted_json) override {
        auto line = aa::diagnostics::diag_event_json(redacted_json);
        if (redacted_json.starts_with("{\"kind\":\"transport_stats\"")) {
            log_runtime(line);
        } else {
            log_line_nonblocking(line);
        }
    }
};

class StorePhones final : public aa::ipc::PhoneDirectory {
  public:
    explicit StorePhones(aa::app::WiredPhoneControl &control) : control_(control) {}

    [[nodiscard]] aa::core::Result<void> forget(aa::core::PhoneId phone) override {
        return control_.forget(phone);
    }
    [[nodiscard]] aa::core::Result<std::vector<std::map<std::string, std::string>>> list_phones()
        const override {
        return control_.list_phones();
    }
    std::optional<aa::helper::RevocationV1> take_revocation_v1() override {
        return control_.take_revocation_v1();
    }
    [[nodiscard]] aa::core::Result<void> pairing_requested(aa::core::PairingRequestId) override {
        return aa::Error{aa::ErrorCode::invalid_argument};
    }
    [[nodiscard]] aa::core::Result<aa::core::PhoneId>
    pairing_confirmed(aa::core::PairingRequestId) override {
        return aa::Error{aa::ErrorCode::invalid_argument};
    }
    void pairing_cancelled(aa::core::PairingRequestId) override {}
    [[nodiscard]] std::vector<aa::core::PairingRequestId> reap_expired() override { return {}; }
    [[nodiscard]] std::vector<aa::core::PairingRequestId> take_invalidated_pairings() override {
        return {};
    }

  private:
    aa::app::WiredPhoneControl &control_;
};

class ConcreteAudioRenderWorker final : public aa::audio::AudioRenderWorker {
  public:
    explicit ConcreteAudioRenderWorker(
        aa::audio::RouterConfig config,
        OpenRouter open = aa::audio::AudioRouter::open,
        IdleClock idle_clock = aa::audio::AudioRenderWorker::monotonic_now)
        : aa::audio::AudioRenderWorker(std::move(config), wired_telemetry(),
                                       std::move(open), std::move(idle_clock)) {}
};


class InputQueue final {
  public:
    void push(const aa::ipc::InputEvent &event) {
        std::lock_guard lock(mutex_);
        if (queue_.size() < kMaxQueued) {
            queue_.push_back(event);
        }
    }
    std::optional<aa::ipc::InputEvent> pop() {
        std::lock_guard lock(mutex_);
        if (queue_.empty()) {
            return std::nullopt;
        }
        auto event = queue_.front();
        queue_.pop_front();
        return event;
    }

  private:
    static constexpr std::size_t kMaxQueued = 64;
    std::mutex mutex_{};
    std::deque<aa::ipc::InputEvent> queue_{};
};

// Default routing element (#60, #80): pulsesink via pipewire-pulse. On the
// target (PipeWire 1.0.5, Ubuntu noble) pipewiresink has no rate control and
// degrades after ~1.5-2.3 h of continuous playback into ~340 ms blocks, with no
// fixed PipeWire packaged for noble; pulsesink ran clean side by side and is
// the default and only supported real-time route (automatic also selects
// pulsesink; explicit pipewire is refused at startup).
aa::audio::RouterConfig pcm_router_config(
    aa::audio::SinkBackend backend = aa::audio::SinkBackend::pulse,
    aa::core::Nanoseconds playout_depth = aa::audio::kWiredPlayoutDepth,
    aa::core::Nanoseconds catch_up_watermark = aa::core::Nanoseconds{0}) {
    return aa::audio::pcm_router_config(backend, wired_telemetry(), playout_depth, catch_up_watermark);
}

// Backend-supported video profiles, preferred first. The first is the primary
// profile: --screen-* pixel quantities are given in its pixels and scaled to
// the others (aa::config::Screen).
std::vector<aa::protocol::ServiceDescriptor>
service_descriptors(const aa::input::CapabilityDescriptor &input,
                    const aa::config::Screen &screen = {},
                    const std::vector<aa::protocol::VideoProfile>& profiles = aa::config::default_video_profiles()) {
    const aa::protocol::VideoConfiguration video{profiles,
                                                 screen};
    const aa::protocol::AudioConfiguration media_audio{{{48000, 16, 2}}};
    const aa::protocol::AudioConfiguration voice_audio{{{16000, 16, 1}}};
    aa::protocol::SensorConfiguration sensors;
    if (input.night_mode() == aa::input::NightModePolicy::manual) {
        sensors.kinds.push_back(aa::protocol::SensorKind::night_mode);
    }
    for (const auto kind : input.vehicle_sensors()) {
        // The capability vocabulary follows the pinned sensor kinds from
        // location (1) to toll card (22); unsupported kinds fail validation.
        sensors.kinds.push_back(static_cast<aa::protocol::SensorKind>(
            static_cast<std::int32_t>(kind) + 1));
    }
    return {
        {aa::protocol::ServiceKey{99}, aa::protocol::ChannelRole::video, video},
        {aa::protocol::ServiceKey{100}, aa::protocol::ChannelRole::media_audio, media_audio},
        {aa::protocol::ServiceKey{101}, aa::protocol::ChannelRole::guidance_audio, voice_audio},
        {aa::protocol::ServiceKey{102}, aa::protocol::ChannelRole::system_audio, voice_audio},
        {aa::protocol::ServiceKey{104}, aa::protocol::ChannelRole::input,
         aa::protocol::ButtonConfiguration{input.keycodes()}},
        {aa::protocol::ServiceKey{105}, aa::protocol::ChannelRole::sensors, sensors},
        {aa::protocol::ServiceKey{106}, aa::protocol::ChannelRole::microphone,
         aa::protocol::MicrophoneConfiguration{{16000, 16, 1}}},
    };
}

using aa::app::register_av_sinks;
using aa::app::service_audio_completions;

aa::core::Result<bool> receive_with_audio(aa::app::WiredSession &session,
                                         ConcreteAudioRenderWorker &audio,
                                         const VideoOutlet *video = nullptr) {
    const bool had_audio = audio.outstanding();
    const bool has_pending_video = video != nullptr && video->has_pending();
    if (auto serviced = service_audio_completions(session, audio); !serviced) {
        return serviced.error();
    }
    auto received = (had_audio || audio.outstanding() || has_pending_video)
        ? session.poll_transport(aa::core::Deadline::Clock::now() + std::chrono::milliseconds{5})
        : session.poll_transport();
    if (auto serviced = service_audio_completions(session, audio); !serviced) {
        return serviced.error();
    }
    return received;
}

// Numeric installer settings contain no phone identifiers or credentials.
std::string screen_description_event(const aa::config::Screen &screen) {
    std::string screen_event = "{\"event\":\"screen_description\",\"density\":" + std::to_string(screen.density);
    const auto add_screen_field = [&](const char* name, const std::optional<std::uint32_t>& value) {
        screen_event += ",\"" + std::string{name} + "\":" + (value ? std::to_string(*value) : "null");
    };
    add_screen_field("real_density", screen.real_density);
    add_screen_field("pixel_aspect_ratio_e4", screen.pixel_aspect_ratio_e4);
    add_screen_field("decoder_additional_depth", screen.decoder_additional_depth);
    add_screen_field("width_margin", screen.width_margin);
    add_screen_field("height_margin", screen.height_margin);
    add_screen_field("theme", screen.theme);
    const auto add_screen_insets = [&](const char* name, const std::optional<aa::config::ScreenInsets>& value) {
        screen_event += ",\"" + std::string{name} + "\":";
        if (!value) { screen_event += "null"; return; }
        screen_event += "{\"top\":" + std::to_string(value->top)
            + ",\"bottom\":" + std::to_string(value->bottom)
            + ",\"left\":" + std::to_string(value->left)
            + ",\"right\":" + std::to_string(value->right) + "}";
    };
    add_screen_insets("margins", screen.margins);
    add_screen_insets("content_insets", screen.content_insets);
    add_screen_insets("stable_content_insets", screen.stable_content_insets);
    return screen_event + "}";
}

// #133: serve the control contract on this (store-owner) thread while the
// refused phone may be approved by the registered consumer. Nothing is
// approved here; only the consumer's explicit confirm writes the store. The
// loop ends on the enrolment's own completion result, never on store
// admission: a committed-but-not-durable approval admits in memory yet is not
// a completed enrolment (#141 B2), so it logs wired_enrolment_not_durable and
// exits 1.
int run_wired_enrolment(aa::ipc::ControlBusService &bus, aa::app::WiredEnrolment &enrolment,
                        const aa::trust::WiredIdentity &refused,
                        std::chrono::steady_clock::time_point deadline) {
    const std::string label = aa::trust::redacted_label(aa::trust::TransportIdentity{refused});
    const auto window = std::chrono::milliseconds{aa::app::WiredEnrolment::kCandidateFreshness.count};
    const auto until = std::min(deadline, std::chrono::steady_clock::now() + window);
    log_line("{\"event\":\"wired_enrolment_open\",\"phone_id\":\"" + label +
             "\",\"window_ms\":" + std::to_string(window.count()) + "}");
    while (g_running.load() && std::chrono::steady_clock::now() < until && !enrolment.completion()) {
        const auto left = std::chrono::ceil<std::chrono::milliseconds>(
            until - std::chrono::steady_clock::now()).count();
        const auto step = bus.cycle(Milliseconds{std::clamp<std::int64_t>(left, 0, 1000)});
        if (!step.has_value()) {
            log_line("{\"event\":\"err\",\"detail\":\"dbus\"}");
            break;
        }
    }
    const auto completion = enrolment.completion();
    enrolment.close();
    // Close any request still outstanding (PairingClosed) before leaving.
    bus.service().refresh_consumer_owner();
    if (completion && !completion->durable) {
        log_line("{\"event\":\"wired_enrolment_not_durable\",\"phone_id\":\"" + label +
                 "\",\"detail\":\"store directory sync failed; approval may not survive a crash\"}");
        return 1;
    }
    if (completion) {
        log_line("{\"event\":\"wired_enrolment_approved\",\"phone_id\":\"" + label +
                 "\",\"next\":\"replug the phone, then start the receiver again\"}");
        return 0;
    }
    log_line("{\"event\":\"wired_enrolment_closed\",\"phone_id\":\"" + label + "\"}");
    return 3;
}

const char *const kUsage =
    "usage: aa-wired-receiver --port <usb-port-context> [--store PATH]\n"
    "                         [--video-socket PATH] [--input-socket PATH]\n"
    "                         [--duration-ms N] [--reconnect-ms N] [--no-bus]\n"
    "                         [--require-video-consumer] [--wired-enrolment]\n"
    "                         [--screen-<field> N]... [--screen-experimental-geometry]\n"
    "                         [--audio-sink pulse|pipewire|automatic]\n"
    "                         [--audio-playout-ms N] [--video-delay-ms N|match]\n"
    "                         [--audio-catch-up-watermark-ms N]\n"
    "                         [--require-usb-serial SERIAL]\n"
    "  runs the wired session over USB/AOA with TrustStore admission;\n"
    "  the approved-phones store must already contain the phone (fail closed).\n"
    "  --reconnect-ms bounds recovery from link loss to active\n"
    "  (1..120000 ms, default 30000).\n"
    "  --wired-enrolment: when admission refuses the phone, keep the D-Bus\n"
    "  contract up for 120 s so the registered consumer can approve that exact\n"
    "  refused phone (RequestAddPhone, GetPairingCandidate, then\n"
    "  ConfirmPhonePairing = yes or CancelPhonePairing = no; aa-wired-enrol is\n"
    "  the terminal reference). An approval exits 0 (restart to connect); no\n"
    "  approval exits 3. Needs the bus (not with --no-bus).\n"
    "  --require-usb-serial pins the run to one physical phone: the first link\n"
    "  and every reconnect must carry this pre-switch USB serial (the adb\n"
    "  serial), else err code phone_serial_mismatch and exit 3 (qualification).\n"
    "  --require-video-consumer defers upstream negotiation until a video\n"
    "  consumer has been accepted and installed (bounded, fail closed).\n"
    "  --audio-sink picks the routing element (default pulse: pulsesink via\n"
    "  pipewire-pulse; automatic: pulsesink; pipewire: refused because\n"
    "  pipewiresink has no rate control on PipeWire 1.0.5 / Ubuntu noble and\n"
    "  degrades after 1.5-2.3 h; use pulse).\n"
    "  --audio-playout-ms N: wired audio playout buffer in ms: chunks play back\n"
    "  to back N ms after the arrival that anchored the stream (default 60, max\n"
    "  180; 0 = legacy arrival-time model).\n"
    "  --video-delay-ms N|match: hold video before forwarding it so it stays\n"
    "  aligned with audio (default match: follow the audio playout offset,\n"
    "  starting at --audio-playout-ms; N: fixed 0..180, needs --audio-playout-ms\n"
    "  > 0 when N > 0).\n"
    "  --audio-catch-up-watermark-ms N: post-stall catch-up watermark in ms\n"
    "  (0 disables catch-up, default 0; else 20..200; needs --audio-playout-ms 0).\n"
    "  --screen-<field> N describes the panel in service discovery. Fields:\n"
    "  density (120|160|240|320|480|640), real-density, pixel-aspect-ratio-e4,\n"
    "  decoder-additional-depth, theme (0|1|2), width-margin, height-margin,\n"
    "  {margins,content-insets,stable-content-insets}-{top,bottom,left,right}.\n"
    "  --decoder software|hardware|auto; --list-video-capabilities\n"
    "  --idr-recovery auto|nudge|transition  IDR recovery strategy (default auto)\n"
    "  --video-profiles WxH@FPS:CODEC,... (default 1280x720@30:h264,800x480@30:h264)\n"
    "  --exact-video-profiles offer only --video-profiles without appending the\n"
    "  1280x720@30:h264 fallback profile.\n"
    "  Pixel values are in the first offered profile pixels (default\n"
    "  1280x720 (primary profile)) and scaled to every fallback offer. width-margin, height-margin, margins-* and a\n"
    "  pixel-aspect-ratio-e4 other than 10000 need --screen-experimental-geometry:\n"
    "  the display does not crop or map touch for them yet. See ARCHITECTURE.md.\n"
    "  exit codes: 0 clean stop, 1 runtime failure, 2 usage error, 3 blocked,\n"
    "  4 stdout isolation failed (fd 1 unusable at start).\n";

int receiver_main(int argc, char **argv) {
    auto video_profiles = aa::config::default_video_profiles();
    bool exact_video_profiles = false;
    auto video_decoder = aa::consumer::DecodeBackend::software;
    auto idr_strategy = aa::ipc::IdrRecoveryStrategy::auto_escalate;
    aa::config::Screen screen;
    bool experimental_geometry = false;
    std::string port;
    std::string store_path;
    std::string video_socket = aa::ipc::production_video_socket_path().value;
    std::string input_socket = aa::ipc::production_input_socket_path().value;
    std::int64_t duration_ms = 0;
    std::int64_t reconnect_ms = 30'000;
    bool no_bus = false;
    bool require_video_consumer = false;
    bool wired_enrolment = false;
    std::string required_usb_serial;
    aa::audio::SinkBackend audio_backend = aa::audio::SinkBackend::pulse;
    aa::tools::PlayoutOptions playout{
        aa::audio::kWiredPlayoutDepth.count / 1'000'000, 0, std::nullopt};
    for (int index = 1; index < argc; ++index) {
        if (std::strcmp(argv[index], "--port") == 0 && index + 1 < argc) {
            port = argv[++index];
        } else if (std::strcmp(argv[index], "--screen-experimental-geometry") == 0) {
            experimental_geometry = true;
        } else if (std::string_view{argv[index]}.starts_with("--screen-")) {
            const auto field = std::string_view{argv[index]}.substr(9);
            if (index + 1 >= argc || !aa::config::parse_screen_option(screen, field, argv[++index])) {
                std::fprintf(stderr, "invalid screen field: %.*s\n", static_cast<int>(field.size()), field.data());
                return 2;
            }
        } else if (std::strcmp(argv[index], "--store") == 0 && index + 1 < argc) {
            store_path = argv[++index];
        } else if (std::strcmp(argv[index], "--list-video-capabilities") == 0) {
            return aa::tools::write_all(aa::tools::json_fd(), aa::tools::list_video_capabilities()) ? 0 : 1;
        } else if (std::strcmp(argv[index], "--decoder") == 0 && index + 1 < argc) {
            if (!aa::tools::parse_decoder(argv[++index],video_decoder)) { std::fputs("invalid --decoder\n",stderr); return 2; }
        } else if (std::strcmp(argv[index], "--idr-recovery") == 0 && index + 1 < argc) {
            if (!aa::ipc::parse_idr_recovery_strategy(argv[++index], idr_strategy)) {
                std::fputs("invalid --idr-recovery\n", stderr);
                return 2;
            }
        } else if (std::strcmp(argv[index], "--video-profiles") == 0 && index + 1 < argc) {
            if (!aa::config::parse_video_profiles_option(argv[++index], video_profiles)) {
                std::fputs("invalid --video-profiles\n", stderr);
                return 2;
            }
        } else if (std::strcmp(argv[index], "--exact-video-profiles") == 0) {
            exact_video_profiles = true;
        } else if (std::strcmp(argv[index], "--video-socket") == 0 && index + 1 < argc) {
            video_socket = argv[++index];
        } else if (std::strcmp(argv[index], "--input-socket") == 0 && index + 1 < argc) {
            input_socket = argv[++index];
        } else if (std::strcmp(argv[index], "--duration-ms") == 0 && index + 1 < argc) {
            duration_ms = std::strtoll(argv[++index], nullptr, 10);
        } else if (std::strcmp(argv[index], "--reconnect-ms") == 0 && index + 1 < argc) {
            const std::string_view value{argv[++index]};
            const auto end = value.data() + value.size();
            const auto [ptr, ec] = std::from_chars(value.data(), end, reconnect_ms);
            if (ec != std::errc{} || ptr != end || reconnect_ms < 1 || reconnect_ms > 120'000) {
                std::fputs("invalid --reconnect-ms\n", stderr);
                return 2;
            }
        } else if (std::strcmp(argv[index], "--no-bus") == 0) {
            no_bus = true;
        } else if (std::strcmp(argv[index], "--require-video-consumer") == 0) {
            require_video_consumer = true;
        } else if (std::strcmp(argv[index], "--wired-enrolment") == 0) {
            wired_enrolment = true;
        } else if (std::strcmp(argv[index], "--require-usb-serial") == 0 && index + 1 < argc) {
            required_usb_serial = argv[++index];
            if (required_usb_serial.empty()) {
                std::fputs("invalid --require-usb-serial\n", stderr);
                return 2;
            }
        } else if (std::strcmp(argv[index], "--audio-sink") == 0 && index + 1 < argc) {
            const std::string_view value{argv[++index]};
            if (value == "pulse") {
                audio_backend = aa::audio::SinkBackend::pulse;
            } else if (value == "pipewire") {
                audio_backend = aa::audio::SinkBackend::pipewire;
            } else if (value == "automatic") {
                audio_backend = aa::audio::SinkBackend::automatic;
            } else {
                std::fputs(kUsage, stderr);
                return 2;
            }
        } else if (std::strcmp(argv[index], "--audio-playout-ms") == 0 && index + 1 < argc) {
            if (!aa::tools::parse_audio_playout_ms(argv[++index], playout.audio_playout_ms)) {
                std::fputs(kUsage, stderr);
                return 2;
            }
        } else if (std::strcmp(argv[index], "--audio-catch-up-watermark-ms") == 0 &&
                   index + 1 < argc) {
            if (!aa::tools::parse_audio_catch_up_watermark_ms(
                    argv[++index], playout.audio_catch_up_watermark_ms)) {
                std::fputs(kUsage, stderr);
                return 2;
            }
        } else if (std::strcmp(argv[index], "--video-delay-ms") == 0 && index + 1 < argc) {
            if (!aa::tools::parse_video_delay_ms(argv[++index], playout.video_delay_ms)) {
                std::fputs(kUsage, stderr);
                return 2;
            }
        } else {
            std::fputs(kUsage, stderr);
            return 2;
        }
    }
    if (port.empty() || duration_ms < 0 || (wired_enrolment && no_bus) ||
        !aa::tools::validate_playout_options(playout)) {
        std::fputs(kUsage, stderr);
        return 2;
    }

    aa::config::ensure_fallback_video_profile(video_profiles, exact_video_profiles);

    // The description is given in the primary profile's pixels and must stay
    // valid once scaled into every offered profile.
    std::vector<aa::config::ScreenExtent> extents;
    const auto refusal = aa::tools::unavailable_profiles(video_profiles,video_decoder);
    if (!refusal.empty()) { std::fprintf(stderr,"%s\n",refusal.c_str()); return 2; }
    for (const auto &profile : video_profiles) { extents.push_back({profile.width,profile.height}); }
    if (const auto field = aa::config::invalid_screen_field(screen, extents); !field.empty()) {
        std::fprintf(stderr, "invalid screen field: %.*s\n", static_cast<int>(field.size()), field.data());
        return 2;
    }
    // Settings whose behaviour behind them does not exist yet are never
    // reachable by accident.
    const auto experimental = aa::config::experimental_screen_field(screen);
    if (!experimental.empty() && !experimental_geometry) {
        std::fprintf(stderr,
                     "screen field needs --screen-experimental-geometry: %.*s (the display does not "
                     "crop or map touch for it yet)\n",
                     static_cast<int>(experimental.size()), experimental.data());
        return 2;
    }
    const auto started = std::chrono::steady_clock::now();
    const auto deadline = duration_ms > 0 ? started + std::chrono::milliseconds{duration_ms}
                                          : std::chrono::steady_clock::time_point::max();
#ifdef AA_WIRED_RECEIVER_SHUTDOWN_TEST
    if (deadline_observation) {
        deadline_observation->cutoff = started + std::chrono::milliseconds{duration_ms};
    }
#endif
    ::signal(SIGINT, on_signal);
    ::signal(SIGTERM, on_signal);

    std::filesystem::path store_file{};
    if (store_path.empty()) {
        auto resolved = aa::trust::ApprovedPhoneStore::default_path();
        if (!resolved.has_value()) {
            log_line("{\"event\":\"err\",\"detail\":\"trust_store_path\"}");
            return 3;
        }
        store_file = resolved.value();
    } else {
        store_file = std::filesystem::path{store_path};
    }
    aa::trust::ApprovedPhoneStore store{store_file};
    if (!store.load().has_value() || !store.usable()) {
        log_line("{\"event\":\"err\",\"detail\":\"trust_store\"}");
        return 3;
    }

    if (store.dropped_port_only_count() != 0) {
        log_line(std::string{"{\"event\":\"trust_store_migrated_dropped_port_only\",\"count\":"} +
                 std::to_string(store.dropped_port_only_count()) + "}");
    }

    StdoutDiagnostics sink;
    aa::diagnostics::Diagnostics diagnostics{sink};
    aa::session::SteadyClock clock{};
    aa::input::CapabilityDescriptor input = aa::input::CapabilityDescriptor::bench_defaults();

    aa::audio::RouterConfig audio_config = pcm_router_config(
        audio_backend,
        aa::core::Nanoseconds{playout.audio_playout_ms * 1'000'000LL},
        aa::core::Nanoseconds{playout.audio_catch_up_watermark_ms * 1'000'000LL});
    ConcreteAudioRenderWorker audio_sink{audio_config};
    const auto opened = audio_sink.ready();
    // Run provenance (#60, #80): log the sink selection (including any refusal
    // cause) before failing closed on startup.
    if (const auto report = audio_sink.sink_report();
        opened || !report.cause.empty() || !report.candidates.empty()) {
        log_sink_selected(report, "open");
    }
    if (!opened) {
        log_line(std::string{"{\"event\":\"err\",\"detail\":\"audio_router\",\"code\":\""} +
                 std::string{aa::to_string(opened.error().code())} + "\"}");
        return 3;
    }

    auto video =
        aa::ipc::VideoServer::listen(aa::ipc::VideoSocketPath{video_socket},
                                     aa::ipc::VideoServerOptions{{}, aa::ipc::current_uid()});
    auto input_server =
        aa::ipc::InputServer::listen(aa::ipc::InputSocketPath{input_socket},
                                     aa::ipc::InputServerOptions{aa::ipc::current_uid()});
    if (!video.has_value() || !input_server.has_value()) {
        log_line("{\"event\":\"err\",\"detail\":\"socket\"}");
        return 3;
    }

    aa::app::WiredSessionConfig session_config{};
    session_config.lifecycle.phases.negotiate = Milliseconds{15'000};
    session_config.lifecycle.wired_reconnect = Milliseconds{reconnect_ms};
    session_config.input_enabled = true;
    session_config.input = input;
    aa::app::WiredSession session{{store, clock, diagnostics}, session_config};
    aa::app::WiredPhoneControl control{store, session};
    aa::ipc::RandomPairingIdSource ids;
    StorePhones phones{control};
    // #133 wired enrolment: before a session owns the store (this thread runs
    // the bus until then), the bus directory is the enrolment state machine;
    // close() hands pairing refusal and ForgetPhone back to `phones`.
    aa::trust::SteadyClock enrolment_clock{};
    aa::app::WiredEnrolment enrolment{store, enrolment_clock, phones};
    if (!wired_enrolment) {
        enrolment.close();
    }
    std::optional<aa::ipc::ControlBusService> bus{};
    if (!no_bus) {
        auto started_bus = aa::ipc::ControlBusService::start(
            aa::ipc::ControlBusService::Config{aa::ipc::current_uid()},
            aa::ipc::ControlBusService::Deps{ids, wired_enrolment
                                                      ? static_cast<aa::ipc::PhoneDirectory &>(enrolment)
                                                      : static_cast<aa::ipc::PhoneDirectory &>(phones)});
        if (!started_bus.has_value()) {
            log_line("{\"event\":\"err\",\"detail\":\"dbus\"}");
            return 3;
        }
        bus.emplace(std::move(started_bus.value()));
    }
    log_line(screen_description_event(screen));
    if (!experimental.empty()) {
        log_line("{\"event\":\"screen_experimental\",\"warning\":\"" + std::string{experimental}
                 + " is advertised but the display does not crop to it and touch is not mapped through it; "
                   "expect a wrong touch mapping and a cropped or offset picture\"}");
    }
    if (!session.configure(service_descriptors(input, screen, video_profiles)).has_value()) {
        log_line("{\"event\":\"err\",\"detail\":\"configure\"}");
        return 1;
    }
    VideoOutlet video_outlet{log_line_nonblocking, log_media};
    std::chrono::milliseconds video_hold{};
    bool follow_audio_playout = false;
    aa::tools::configure_video_hold(video_outlet, audio_sink, playout, follow_audio_playout,
                                    video_hold);
    if (follow_audio_playout || video_hold.count() > 0) {
        try {
            video_outlet.start_release_timer();
        } catch (const std::system_error &) {
            log_line("{\"event\":\"video_release_timer_error\",\"reason\":\"start_failed\"}");
            return 3;
        }
    }
    log_line(aa::tools::media_policy_json("wired_media_policy", playout,
                                          audio_sink.credit_window(), follow_audio_playout,
                                          video_hold));
    video_outlet.set_idr_recovery({
        [&session] { return session.video_projecting(); },
        [&session] { return session.request_video_idr(); },
        {},
        [&session](bool focus_transition) { return session.request_video_idr(focus_transition); },
        idr_strategy,
    });
    aa::channels::InputSink input_sink;
    aa::channels::SensorSink sensor_sink{input, [&session](std::vector<std::byte> payload) {
        return session.send_channel_payload(aa::protocol::ChannelRole::sensors, std::move(payload));
    }};
    aa::channels::MicrophoneSink microphone_sink{[&session](std::vector<std::byte> payload) {
        return session.send_channel_payload(aa::protocol::ChannelRole::microphone, std::move(payload));
    }};
    if (!register_av_sinks(session, video_outlet, audio_sink).has_value() ||
        !session.register_sink(aa::protocol::ChannelRole::microphone, microphone_sink).has_value() ||
        !session.register_sink(aa::protocol::ChannelRole::input, input_sink).has_value() ||
        !session.register_sink(aa::protocol::ChannelRole::sensors, sensor_sink).has_value()) {
        log_line("{\"event\":\"err\",\"detail\":\"sink\"}");
        return 1;
    }

    aa::transport::UsbDeviceSelector selector{};
    selector.port_context = port;
    aa::transport::AoaAccessoryInfo accessory{};
    const auto redacted_port = aa::diagnostics::redact_identifier(port);
    log_line("{\"event\":\"connecting\",\"port\":\"" + redacted_port + "\"}");
    const auto log_reset = [&](std::string_view status) {
        log_line_nonblocking("{\"event\":\"diag\",\"detail\":\"aoa_reset\",\"port\":\"" +
                             redacted_port + "\",\"status\":\"" + std::string{status} + "\"}");
    };

    // Per-link cipher (P1-B/P1-C): the initial attach AND every reconnect build
    // a FRESH cipher bound to the identity the TrustStore admitted for THIS
    // link - never the prior link's cipher object and never the initially
    // captured trust predicate. The predicate re-checks the store on each
    // handshake step, so a mid-handshake revoke fails closed.
    auto bind_link_cipher = [&session, &store](const aa::trust::PhoneIdentity &phone) -> bool {
        auto cipher = aa::transport::make_client_handshake_cipher(
            [&store, phone] { return store.lookup(phone) == aa::trust::Decision::approved; });
        if (!cipher) {
            return false;
        }
        session.attach_cipher(std::move(cipher));
        return true;
    };

    // --require-usb-serial: the admitted link must be the one expected phone.
    // Checked on the admitted identity, before any session use of the link.
    auto pinned = [&required_usb_serial](const aa::trust::PhoneIdentity &phone) {
        return required_usb_serial.empty() ||
               aa::trust::wired_key_usb_serial(phone.key) == required_usb_serial;
    };
    constexpr std::string_view kSerialMismatch =
        "{\"event\":\"err\",\"detail\":\"link\",\"code\":\"phone_serial_mismatch\"}";

    aa::trust::WiredIdentity attempted_identity{};
    auto first_link = [&] {
        aa::core::CancellationSource startup_stop;
        auto watch_shutdown = cancel_open_on_shutdown(startup_stop);
        aa::core::Deadline startup_budget;
        startup_budget.cancellation = startup_stop.get_token();
        aa::transport::AoaRecovery startup_recovery;
        startup_recovery.reset_status = log_reset;
        return aa::app::open_wired_link(selector, accessory, store, startup_budget,
                                        &attempted_identity, &startup_recovery);
    }();
    if (!g_running.load()) {
        return 0;
    }
    if (!first_link.has_value()) {
        // ADDITIVE attribution at admission rejection (item 2): the link-error
        // record names the rejected attempt's identity+port. The identity is
        // pseudonymized (trust::redacted_label) and the fields only appear when
        // admission actually refused a captured identity; the existing fields
        // (event/detail/code) are unchanged.
        std::string line = std::string{"{\"event\":\"err\",\"detail\":\"link\",\"code\":\""} +
                          std::string{aa::to_string(first_link.error().code())} + "\"";
        if (!attempted_identity.port_context.empty()) {
            line += ",\"phone_id\":\"" +
                    aa::trust::redacted_label(aa::trust::TransportIdentity{attempted_identity}) +
                    "\",\"port\":\"" +
                    aa::diagnostics::redact_identifier(attempted_identity.port_context) + "\"";
        }
        line += "}";
        log_line(line);
        if (wired_enrolment && bus.has_value() &&
            first_link.error().code() == aa::ErrorCode::transport_unauthorized_peer &&
            !attempted_identity.port_context.empty() &&
            enrolment.note_refused(attempted_identity).has_value()) {
            return run_wired_enrolment(bus.value(), enrolment, attempted_identity, deadline);
        }
        return 3;
    }
    if (!pinned(first_link.value().phone)) {
        log_line(std::string{kSerialMismatch});
        return 3;
    }
    enrolment.close();  // a session now owns the store; pairing is refused again
    std::optional<aa::app::WiredLink> link{std::move(first_link.value())};
    session.attach_transport(link->transport.get());
    if (!bind_link_cipher(link->phone)) {
        // The factory reports why out of band (credential class, cipher
        // configuration); without it every cause looked like a bare null cipher.
        std::string line = "{\"event\":\"err\",\"detail\":\"tls_cipher\"";
        if (const auto reason = aa::transport::last_handshake_cipher_failure(); !reason.empty()) {
            line += ",\"reason\":\"" + reason + "\"";
        }
        line += "}";
        log_line(line);
        return 3;
    }
    if (!session.apply(aa::session::Event::start_discovery).has_value() ||
        !session.phone_discovered(link->phone).has_value() ||
        !session.apply(aa::session::Event::transport_ready).has_value()) {
        log_line("{\"event\":\"err\",\"detail\":\"lifecycle\"}");
        return 1;
    }
    if (!require_video_consumer) {
        if (!session.send_version_request(1, 6).has_value()) {
            log_line("{\"event\":\"err\",\"detail\":\"version_request\"}");
            return 1;
        }
    }
    log_line("{\"event\":\"ready\",\"state\":\"negotiating\"}");

    InputQueue input_queue;
#ifdef AA_WIRED_RECEIVER_SHUTDOWN_TEST
    ObservedSessionMutex session_mutex;
#else
    std::mutex session_mutex;
#endif
    std::atomic<std::int64_t> active_ms{-1};
    std::atomic<bool> session_failed{false};
    std::atomic<bool> admitted{false};
    std::mutex gate_mutex;
    std::condition_variable gate_cv;
    aa::core::CancellationSource reconnect_stop;
    aa::transport::AoaRecovery recovery;
    recovery.reset_status = log_reset;
    bool runtime_failed = false;

    auto service_control = [&] {
        if (const auto error = audio_sink.failure()) {
            session_failed.store(true);
            g_running.store(false);
            (void)session.fail(*error);
            log_line_nonblocking(audio_failure_event_json(
                "audio_sink_failure", error->code(), audio_sink.failure_context()));
            return true;
        }
        control.checkpoint();
        if (control.revoked()) {
            session_failed.store(true);
            g_running.store(false);
        }
        if (control.finish()) {
            session_failed.store(true);
            g_running.store(false);
            return true;
        }
        return false;
    };

    aa::ipc::VideoSelectionWatchdog selection_watchdog{};
    bool in_video_recovery = false;
    bool in_receive_poll = false;
    auto service_video_recovery = [&]() -> bool {
        if (in_video_recovery) {
            return true;
        }
        in_video_recovery = true;
        struct Guard {
            bool &active;
            ~Guard() { active = false; }
        } guard{in_video_recovery};
        // Held video leaves on time even while the session is mid-send:
        // release_held() needs no session I/O (IDR requests wait for service_pending).
        if (auto held = video_outlet.release_held(); !held) {
            session_failed.store(true);
            g_running.store(false);
            log_line_nonblocking(poll_error_json(held.error().code(), session, audio_sink));
            if (!session.failure()) {
                (void)session.fail(held.error());
            }
            return false;
        }
        if (session.outbound_in_progress()) {
            return true;
        }
        if (!video_outlet.idr_dispatching()) {
            if (auto flushed = video_outlet.service_pending(); !flushed) {
                session_failed.store(true);
                g_running.store(false);
                log_line_nonblocking(poll_error_json(flushed.error().code(), session, audio_sink));
                if (!session.failure()) {
                    (void)session.fail(flushed.error());
                }
                return false;
            }
        }
        if (auto pending = session.service_pending_video_idr(); !pending) {
            if (session.failure()) {
                session_failed.store(true);
                g_running.store(false);
            }
            log_line_nonblocking(poll_error_json(pending.error().code(), session, audio_sink));
            if (session.failure()) {
                return false;
            }
        }
        if (bus && !video_outlet.idr_dispatching() &&
            bus->service().consume_idr_recovery_request()) {
            const auto idr = video_outlet.request_idr_external("consumer_ipc");
            if (!idr) {
                const bool fatal = idr.error().code() == aa::ErrorCode::channel_focus_restore_failed;
                if (fatal) {
                    session_failed.store(true);
                    g_running.store(false);
                }
                log_line_nonblocking(poll_error_json(idr.error().code(), session, audio_sink));
                if (fatal) {
                    if (!session.failure()) {
                        (void)session.fail(idr.error());
                    }
                    return false;
                }
            }
        }
        const bool session_ready =
            session.state() == aa::session::State::active && session.authenticated();
        const bool has_selection = session.selected_video_profile().has_value();
        if (selection_watchdog.check(session_ready, has_selection,
                                     std::chrono::steady_clock::now())) {
            log_line_nonblocking(aa::ipc::video_selection_timeout_json(
                aa::ipc::kVideoSelectionWarningTimeout, video_profiles.size()));
        }
        return true;
    };

    auto io_checkpoint = [&] {
        control.checkpoint();
        if (in_receive_poll && !session.failure() && g_running.load()) {
            (void)service_video_recovery();
        }
    };

    auto reconnect = [&]() -> aa::core::Result<void> {
        if (service_control()) {
            return aa::Error{aa::ErrorCode::cancelled};
        }
        if (auto lost = session.apply(aa::session::Event::link_lost); !lost) {
            return lost.error();
        }
        video_outlet.reset();
        const auto session_budget = session.io_deadline();
        aa::core::CancellationSource open_stop;
        const std::stop_callback cancel_on_session_stop{session_budget.cancellation,
                                                        [&] { open_stop.request_stop(); }};
        const std::stop_callback cancel_on_dispatcher_stop{reconnect_stop.get_token(),
                                                           [&] { open_stop.request_stop(); }};
        auto watch_shutdown = cancel_open_on_shutdown(open_stop);
        const aa::core::Deadline budget{session_budget.until, open_stop.get_token()};
        while (g_running.load()) {
            if (service_control()) {
                return aa::Error{aa::ErrorCode::cancelled};
            }
            if (auto checked = budget.check(); !checked) {
                session.tick();
                return checked.error();
            }
            const aa::core::Deadline slice{
                std::min(budget.until, aa::core::Deadline::Clock::now() + std::chrono::seconds{1}),
                budget.cancellation};
            auto relink = aa::app::open_wired_link(selector, accessory, store, slice,
                                                  nullptr, &recovery);
            if (!g_running.load() || service_control()) {
                return aa::Error{aa::ErrorCode::cancelled};
            }
            if (relink) {
                if (!pinned(relink.value().phone)) {
                    log_line_nonblocking(std::string{kSerialMismatch});
                    return aa::Error{aa::ErrorCode::transport_unauthorized_peer};
                }
                if (relink.value().phone.key != link->phone.key) {
                    return aa::Error{aa::ErrorCode::transport_unauthorized_peer};
                }
                link.emplace(std::move(relink.value()));
                control.bind_phone(link->phone);
                link->transport->set_io_checkpoint(io_checkpoint);
                session.attach_transport(link->transport.get());
                if (!bind_link_cipher(link->phone)) {
                    return aa::Error{aa::ErrorCode::transport_unauthorized_peer};
                }
                if (auto connected = session.apply(aa::session::Event::reconnect); !connected) {
                    return connected.error();
                }
                if (auto ready = session.apply(aa::session::Event::transport_ready); !ready) {
                    return ready.error();
                }
                const auto sent = session.send_version_request(1, 6);
                if (sent) {
                    return {};
                }
                return sent.error();
            } else if (relink.error().code() != aa::ErrorCode::transport_unavailable &&
                       relink.error().code() != aa::ErrorCode::timeout &&
                       relink.error().code() != aa::ErrorCode::transport_closed &&
                       relink.error().code() != aa::ErrorCode::transport_io) {
                return relink.error();
            }
            std::mutex mutex;
            std::condition_variable_any cv;
            std::unique_lock lock(mutex);
            cv.wait_until(lock, budget.cancellation,
                          std::min(budget.until, std::chrono::steady_clock::now() +
                                                    std::chrono::milliseconds{50}),
                          [] { return false; });
            lock.unlock();
            if (service_control()) {
                return aa::Error{aa::ErrorCode::cancelled};
            }
        }
        return aa::Error{aa::ErrorCode::cancelled};
    };

    std::thread session_thread([&] {
        control.bind_owner();
        control.bind_phone(link->phone);
        link->transport->set_io_checkpoint(io_checkpoint);
        if (require_video_consumer) {
            while (!admitted.load() && g_running.load()) {
                {
                    std::lock_guard lock(session_mutex);
                    if (service_control()) {
                        break;
                    }
                    session.tick();
                    if (session.failure()) {
#ifdef AA_WIRED_RECEIVER_SHUTDOWN_TEST
                        if (required_failure_observation) {
                            required_failure_observation->negotiation_timeout =
                                session.failure()->code() == aa::ErrorCode::timeout;
                        }
#endif
                        session_failed.store(true);
                        g_running.store(false);
                        break;
                    }
                    if (std::chrono::steady_clock::now() >= deadline) {
                        g_running.store(false);
                        break;
                    }
                }
                std::unique_lock gate_lock(gate_mutex);
                gate_cv.wait_until(gate_lock, std::min(deadline, std::chrono::steady_clock::now() +
                                                     std::chrono::milliseconds{50}), [&] {
                    return admitted.load() || !g_running.load();
                });
            }
            if (admitted.load() && g_running.load()) {
#ifdef AA_WIRED_RECEIVER_SHUTDOWN_TEST
                if (deadline_observation) {
                    deadline_observation->cross(DeadlineObservation::Edge::owner);
                }
#endif
                const std::lock_guard lock(session_mutex);
                if (!g_running.load() || std::chrono::steady_clock::now() >= deadline) {
                    g_running.store(false);
                } else if (!session.send_version_request(1, 6).has_value()) {
                    session_failed.store(true);
                    g_running.store(false);
                    log_line_nonblocking("{\"event\":\"err\",\"detail\":\"version_request\"}");
                }
#ifdef AA_WIRED_RECEIVER_SHUTDOWN_TEST
                if (deadline_observation) {
                    deadline_observation->finish_owner();
                }
#endif
            }
        }
        while (g_running.load()) {
            aa::core::Result<void> audio_service{};
            {
                std::lock_guard lock(session_mutex);
                audio_service = service_audio_completions(session, audio_sink);
                if (service_control()) {
                    break;
                }
                session.tick();
                if (audio_service) {
                    audio_service = service_audio_completions(session, audio_sink);
                }
                if (session.failure()) {
                    session_failed.store(true);
                    g_running.store(false);
                    break;
                }
                if (!service_video_recovery()) {
                    break;
                }
                while (auto event = input_queue.pop()) {
                    const auto outcome = session.submit_input(*event);
                    if (service_control()) {
                        break;
                    }
                    log_runtime(std::string{"{\"event\":\"input_forwarded\",\"ok\":\""} +
                                (outcome.has_value() ? "true" : "false") + "\"}");
                }
            }
            if (!g_running.load()) {
                break;
            }
            in_receive_poll = true;
            auto polled = audio_service ? receive_with_audio(session, audio_sink, &video_outlet)
                                        : aa::core::Result<bool>{audio_service.error()};
            in_receive_poll = false;
            {
                std::lock_guard lock(session_mutex);
                if (service_control()) {
                    break;
                }
                if (session.failure()) {
                    session_failed.store(true);
                    g_running.store(false);
                    break;
                }
                if (polled) {
                    if (auto serviced = service_audio_completions(session, audio_sink); !serviced) {
                        polled = serviced.error();
                    }
                }
                if (session.state() == aa::session::State::stopping) {
                    g_running.store(false);
                    break;
                }
                if (!polled.has_value()) {
                    const auto code = polled.error().code();
                    if (!g_running.load()) {
                        break;
                    }
                    log_line_nonblocking(poll_error_json(code, session, audio_sink));
                    if (!session.failure() &&
                        (code == aa::ErrorCode::transport_closed ||
                         code == aa::ErrorCode::transport_io || code == aa::ErrorCode::timeout)) {
                        const auto recovered = reconnect();
                        if (recovered) {
                            continue;
                        }
                        if (!g_running.load()) {
                            break;
                        }
                        session_failed.store(true);
                        g_running.store(false);
                        (void)session.fail(recovered.error());
                    } else if (!session.failure()) {
                        session_failed.store(true);
                        g_running.store(false);
                        (void)session.fail(polled.error());
                    }
                    session_failed.store(true);
                    g_running.store(false);
                    break;
                }
                if (session.state() == aa::session::State::negotiating &&
                    session.channel_opens() > 0) {
                    if (session.apply(aa::session::Event::negotiation_succeeded)) {
                        recovery.reset_attempted = false;
                    }
                }
                if (session.state() == aa::session::State::active && active_ms.load() < 0) {
                    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                                             std::chrono::steady_clock::now() - started)
                                             .count();
                    active_ms.store(elapsed);
                    log_line_nonblocking("{\"event\":\"active\",\"startup_ms\":" +
                                         std::to_string(elapsed) + "}");
                }
                if (!service_video_recovery()) {
                    break;
                }
            }
        }
        // Bounded wait for an in-flight ForgetPhone write, never under the
        // session mutex (the input thread and video recovery take it).
        control.drain_persistence();
        std::lock_guard lock(session_mutex);
        audio_sink.stop();
        (void)control.finish();
        session.request_stop();
        session.tick();
        if (session.state() == aa::session::State::stopping) {
            (void)session.apply(aa::session::Event::cleanup_finished);
#ifdef AA_WIRED_RECEIVER_SHUTDOWN_TEST
            if (shutdown_observation) {
                shutdown_observation->reset_finished(
                    session.state() == aa::session::State::disconnected);
            }
#endif
        }
        link->transport->set_io_checkpoint({});
        control.close();
    });

    std::thread input_thread([&] {
        while (g_running.load()) {
            auto endpoint = input_server.value().accept_sender(Milliseconds{200});
            if (!endpoint) {
                continue;
            }
            log_line_nonblocking("{\"event\":\"input_accepted\"}");
            while (g_running.load()) {
                auto event = endpoint.value().receive(Milliseconds{200});
                if (!event) {
                    if (event.error().code() == aa::ErrorCode::timeout) {
                        continue;
                    }
                    break;
                }
                input_queue.push(event.value());
            }
            endpoint.value().close();
        }
    });

    const auto runtime_slice = [&] {
        return Milliseconds{std::max<std::int64_t>(0, std::min<std::int64_t>(200,
            std::chrono::ceil<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now()).count()))};
    };
    while (g_running.load() && std::chrono::steady_clock::now() < deadline) {
        if (bus.has_value()) {
            auto step = bus.value().cycle(runtime_slice());
            if (!step.has_value()) {
                runtime_failed = true;
                break;
            }
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds{runtime_slice().count});
        }
#ifdef AA_WIRED_RECEIVER_SHUTDOWN_TEST
        if (deadline_observation) {
            deadline_observation->cross(DeadlineObservation::Edge::dispatch);
        }
#endif
        if (!g_running.load() || std::chrono::steady_clock::now() >= deadline) {
            break;
        }
        const auto accept_timeout = (require_video_consumer && !admitted.load())
                                        ? runtime_slice()
                                        : Milliseconds{0};
        auto producer = video.value().accept_consumer(accept_timeout);
        if (producer.has_value()) {
#ifdef AA_WIRED_RECEIVER_SHUTDOWN_TEST
            if (deadline_observation) {
                deadline_observation->cross(DeadlineObservation::Edge::accepted);
            }
#endif
            if (!g_running.load() || std::chrono::steady_clock::now() >= deadline) {
                break;
            }
            log_line_nonblocking("{\"event\":\"video_accepted\"}");
            video_outlet.install(std::move(producer.value()));
            {
                std::lock_guard gate_lock(gate_mutex);
                if (!g_running.load() || std::chrono::steady_clock::now() >= deadline) {
                    break;
                }
                admitted.store(true);
            }
            gate_cv.notify_all();
#ifdef AA_WIRED_RECEIVER_SHUTDOWN_TEST
            if (deadline_observation) {
                deadline_observation->hold_dispatcher();
            }
#endif
        }
    }

    g_running.store(false);
    // A link open can wait for re-enumeration while the owner holds the mutex.
    // Wake it before taking the mutex for the ordered session stop.
    reconnect_stop.request_stop();
#ifdef AA_WIRED_RECEIVER_SHUTDOWN_TEST
    if (shutdown_observation) {
        shutdown_observation->published_false();
    }
#endif
    {
        const std::lock_guard lock(session_mutex);
#ifdef AA_WIRED_RECEIVER_SHUTDOWN_TEST
        if (shutdown_observation) {
            shutdown_observation->main_stop();
        }
#endif
        session.request_stop();
    }
#ifdef AA_WIRED_RECEIVER_SHUTDOWN_TEST
    if (shutdown_observation) {
        shutdown_observation->main_join();
    }
#endif
    session_thread.join();
    input_thread.join();

    if (auto finished = audio_sink.finish(); !finished) {
        log_line_nonblocking(audio_failure_event_json("audio_finish", finished.error().code(), audio_sink.failure_context()));
        runtime_failed = true;
    }

    video_outlet.close();
    input_server.value().close();
    video.value().close();
    if (bus.has_value()) {
        bus.value().close();
    }

    const auto total = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now() - started)
                           .count();
    log_line("{\"event\":\"stopping\",\"duration_ms\":" + std::to_string(total) +
             ",\"startup_ms\":" + std::to_string(active_ms.load()) +
             ",\"input_sends\":" + std::to_string(session.input_sends()) +
             ",\"input_failures\":" + std::to_string(session.input_send_failures()) +
             ",\"channel_opens\":" + std::to_string(session.channel_opens()) +
             ",\"version_responses\":" + std::to_string(session.version_responses()) +
             ",\"unhandled_controls\":" + std::to_string(session.unhandled_controls()) + "}");
    if (const auto *writer = aa::wired_receiver::active_stdout_log.load();
        writer && writer->latched_error()) {
        runtime_failed = true;
    }
    return runtime_failed || session_failed.load() ? 1 : 0;
}
} // namespace

int main(int argc, char **argv) {
    // Before any GStreamer/NVIDIA load: JSON moves to a private descriptor and
    // fd 1 becomes /dev/null (tools/json_stdout.hpp).
    aa::tools::StdoutIsolation isolation;
    if (!isolation.ok()) {
        isolation.report_failure("aa-wired-receiver");
#ifdef AA_WIRED_RECEIVER_SHUTDOWN_TEST
        return aa::tools::kStdoutIsolationFailed;
#else
        aa::tools::StdoutIsolation::end_process(aa::tools::kStdoutIsolationFailed);
#endif
    }
    int code = 0;
    {
        aa::wired_receiver::StdoutLogScope logging{g_running};
        code = receiver_main(argc, argv);
        const auto error = logging.log.finish();
        code = code != 0 ? code : (error ? 1 : 0);
    }
#ifdef AA_WIRED_RECEIVER_SHUTDOWN_TEST
    return isolation.release(code);
#else
    aa::tools::StdoutIsolation::end_process(code);
#endif
}
