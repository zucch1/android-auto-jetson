// SPDX-License-Identifier: GPL-3.0-or-later
#include "Runtime.hpp"
#include <aa/config/VideoProfiles.hpp>
#include <aa/trust/PairingPolicy.hpp>
#include <aa/tls/Credentials.hpp>
#include <charconv>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <variant>
#include <vector>

namespace aa::wireless_receiver {
constexpr const char* usage =
    "usage: aa-wireless-receiver --store-dir PATH (--phone-id N | --pairing-only) [options]\n"
    "  PATH contains approved-phones.json; N selects an approved wireless phone.\n"
    "  --pairing-only       run only the authenticated control/pairing API; no projection.\n"
    "  --video-socket PATH   encoded video consumer socket\n"
    "  --input-socket PATH   normalized display input socket\n"
    "  --video-profiles WxH@FPS:CODEC,...  offered video profiles (same syntax as\n"
    "                       the wired receiver; default 1280x720@30:h264,800x480@30:h264)\n"
    "  --exact-video-profiles offer only --video-profiles without appending the\n"
    "                       1280x720@30:h264 fallback profile\n"
    "  --helper-bus-address ADDRESS  explicit alternate helper bus (default: system bus)\n"
    "  --idr-recovery auto|nudge|transition  IDR recovery strategy (default auto)\n"
    "  --audio-sink pulse|pipewire|automatic  audio routing element (default pulse)\n"
    "  --audio-catch-up-watermark-ms N        post-stall catch-up watermark in ms\n"
    "                                         (0 disables catch-up, default 0; max 1000;\n"
    "                                         needs --audio-playout-ms 0)\n"
    "  --audio-playout-ms N   wireless audio playout buffer in ms: chunks play back to\n"
    "                       back N ms after the arrival that anchored the stream\n"
    "                       (default 120, max 180; 0 = arrival-time model)\n"
    "  --audio-max-unacked N  audio credit window advertised to the phone (1..8,\n"
    "                       default 4; 1 = stop-and-wait)\n"
    "  --video-delay-ms N|match  hold video before forwarding it so it stays aligned\n"
    "                       with audio (default match: follow the audio playout\n"
    "                       offset, starting at --audio-playout-ms; N: fixed 0..180,\n"
    "                       needs --audio-playout-ms > 0 when N > 0)\n"
    "  --diagnostics        emit redacted diagnostic JSON to stdout\n"
    "  --duration-ms N      bounded run (1..86400000 ms; omitted: until signal)\n"
    "  --ap-channel N       AP channel; must equal ap.conf (36, 40, 44 or 48; default 36)\n"
    "  --help               show this help, exit 0\n"
    "  Env: DBUS_SESSION_BUS_ADDRESS selects the session control bus.\n"
    "       HU_KEY_PATH must name an owner-only certificate/private-key PEM bundle.\n"
    "  Default sockets: $XDG_RUNTIME_DIR/android-auto-receiver/{video,input}.sock\n"
    "  (runtime directory fallback: /run/user/$UID).\n"
    "  No pairing window is opened by automatic admission or recovery.\n"
    "  Exit: 0 clean stop, 1 runtime failure, 2 invalid arguments, 3 blocked startup.\n";

struct Options final {
    std::filesystem::path store_dir;
    core::PhoneId phone{};
    std::string video_socket{ipc::production_video_socket_path().value};
    std::string input_socket{ipc::production_input_socket_path().value};
    std::string helper_address;
    std::vector<protocol::VideoProfile> video_profiles{config::default_video_profiles()};
    ipc::IdrRecoveryStrategy idr_recovery{ipc::IdrRecoveryStrategy::auto_escalate};
    audio::SinkBackend audio_backend{audio::SinkBackend::pulse};
    std::int64_t audio_catch_up_watermark_ms{0};
    std::int64_t audio_playout_ms{120};
    std::uint32_t audio_max_unacked{audio::kWirelessCreditWindow};
    std::optional<std::int64_t> video_delay_ms{}; // absent: match the audio playout
    std::int64_t duration_ms{};
    std::uint32_t ap_channel{36};
    bool exact_video_profiles{};
    bool diagnostics{};
    bool pairing_only{};
};

std::optional<Options> parse_options(int argc, char** argv) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg{argv[i]};
        if (arg == "--diagnostics") { options.diagnostics = true; continue; }
        if (arg == "--pairing-only") { options.pairing_only = true; continue; }
        if (arg == "--exact-video-profiles") { options.exact_video_profiles = true; continue; }
        if (i + 1 == argc) return std::nullopt;
        const std::string_view value{argv[++i]};
        if (value.empty() || value.starts_with("--")) return std::nullopt;
        if (arg == "--store-dir") options.store_dir = value;
        else if (arg == "--video-socket") options.video_socket = value;
        else if (arg == "--input-socket") options.input_socket = value;
        else if (arg == "--video-profiles") {
            if (!aa::config::parse_video_profiles_option(value, options.video_profiles))
                return std::nullopt;
        }
        else if (arg == "--idr-recovery") {
            if (!aa::ipc::parse_idr_recovery_strategy(value, options.idr_recovery))
                return std::nullopt;
        }
        else if (arg == "--audio-sink") {
            if (value == "pulse") options.audio_backend = audio::SinkBackend::pulse;
            else if (value == "pipewire") options.audio_backend = audio::SinkBackend::pipewire;
            else if (value == "automatic") options.audio_backend = audio::SinkBackend::automatic;
            else return std::nullopt;
        }
        else if (arg == "--audio-catch-up-watermark-ms") {
            std::uint64_t number{};
            const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), number);
            if (error != std::errc{} || end != value.data() + value.size() || number > 1000)
                return std::nullopt;
            options.audio_catch_up_watermark_ms = static_cast<std::int64_t>(number);
        }
        else if (arg == "--audio-playout-ms" || arg == "--audio-max-unacked") {
            std::uint64_t number{};
            const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), number);
            if (error != std::errc{} || end != value.data() + value.size()) return std::nullopt;
            if (arg == "--audio-playout-ms") {
                if (number > 180) return std::nullopt;
                options.audio_playout_ms = static_cast<std::int64_t>(number);
            } else {
                if (number == 0 || number > audio::AudioCreditPolicy::kMaxWindow) return std::nullopt;
                options.audio_max_unacked = static_cast<std::uint32_t>(number);
            }
        }
        else if (arg == "--video-delay-ms") {
            if (value == "match") { options.video_delay_ms.reset(); }
            else {
                std::uint64_t number{};
                const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), number);
                if (error != std::errc{} || end != value.data() + value.size() || number > 180)
                    return std::nullopt;
                options.video_delay_ms = static_cast<std::int64_t>(number);
            }
        }
        else if (arg == "--helper-bus-address") options.helper_address = value;
        else if (arg == "--ap-channel") {
            std::uint32_t number{};
            const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), number);
            if (error != std::errc{} || end != value.data() + value.size() || !helper::ap_channel_allowed(number))
                return std::nullopt;
            options.ap_channel = number;
        } else if (arg == "--phone-id" || arg == "--duration-ms") {
            std::uint64_t number{};
            const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), number);
            if (error != std::errc{} || end != value.data() + value.size() || number == 0)
                return std::nullopt;
            if (arg == "--phone-id") {
                if (number > trust::kMaxPhoneId) return std::nullopt;
                options.phone = core::PhoneId{number};
            } else {
                if (number > 86'400'000) return std::nullopt;
                options.duration_ms = static_cast<std::int64_t>(number);
            }
        } else return std::nullopt;
    }
    if (options.store_dir.empty() || (options.pairing_only ? options.phone.value != 0 : options.phone.value == 0) ||
        !options.store_dir.is_absolute() || !std::filesystem::path{options.video_socket}.is_absolute() ||
        !std::filesystem::path{options.input_socket}.is_absolute() || options.video_socket == options.input_socket)
        return std::nullopt;
    // Catch-up trims audio the playout buffer exists to keep (rejected for
    // wireless, 2026-11-20): it applies only to the arrival model.
    if (options.audio_catch_up_watermark_ms > 0 && options.audio_playout_ms > 0) return std::nullopt;
    // F8: a video hold without a playout buffer only skews A/V.
    if (options.video_delay_ms && *options.video_delay_ms > 0 && options.audio_playout_ms == 0) return std::nullopt;
    aa::config::ensure_fallback_video_profile(options.video_profiles, options.exact_video_profiles);
    return options;
}

int receiver_main(int argc, char** argv) {
    if (argc == 2 && std::strcmp(argv[1], "--help") == 0) {
        std::fputs(usage, stdout); return 0;
    }
    const auto options = parse_options(argc, argv);
    if (!options) { std::fputs(usage, stderr); return 2; }
    std::error_code fs_error;
    if (!std::filesystem::is_directory(options->store_dir, fs_error) || fs_error) {
        std::fputs("aa-wireless-receiver: --store-dir must name an existing directory\n", stderr);
        return 3;
    }
    aa::trust::ApprovedPhoneStore store{options->store_dir / "approved-phones.json"};
    if (auto loaded = store.load(); !loaded) { report("trust store", loaded.error().code()); return 3; }
    auto helper = options->helper_address.empty() ? aa::helper::BusClient::system(store)
        : aa::helper::BusClient::private_bus(options->helper_address, store);
    if (!helper) { report("system helper connection", helper.error().code()); return 3; }
    if (auto status = helper.value()->status(aa::helper::Action::ap_status); !status) {
        report("org.custom.AndroidAutoSystemHelper AP status", status.error().code()); return 3;
    }
    if (options->pairing_only) {
        DiagnosticOutput output{options->diagnostics};
        aa::diagnostics::Diagnostics diagnostics{output};
        aa::session::SteadyClock clock;
        aa::trust::SteadyClock trust_clock;
        aa::trust::PairingPolicy phones{store, trust_clock};
        aa::ipc::RandomPairingIdSource ids;
        aa::app::WirelessReceiver receiver{{store, clock, diagnostics}, *helper.value(),
            aa::app::WirelessLink::Config{}};
        auto bus = aa::ipc::ControlBusService::start({aa::ipc::current_uid()},
            {ids, phones, &receiver.confirmation(), [&receiver] {
            const auto* session = receiver.session();
            return session && session->state() == aa::session::State::active;
        }});
        if (!bus) { report("session control bus", bus.error().code()); return 3; }
        struct sigaction action{};
        action.sa_handler = on_signal;
        ::sigemptyset(&action.sa_mask);
        if (::sigaction(SIGINT, &action, nullptr) != 0 || ::sigaction(SIGTERM, &action, nullptr) != 0) {
            std::fputs("aa-wireless-receiver: signal handler installation failed\n", stderr); return 3;
        }
        const auto deadline = options->duration_ms ? Runtime::Clock::now() + std::chrono::milliseconds{options->duration_ms}
            : Runtime::Clock::time_point::max();
        log_line("{\"event\":\"ready\",\"state\":\"pairing_only\"}");
        while (running.load() && Runtime::Clock::now() < deadline) {
            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Runtime::Clock::now()).count();
            const auto wait = options->duration_ms && remaining < 50 ? remaining : 50;
            if (wait < 0) break;
            if (auto step = bus.value().cycle(aa::core::Milliseconds{wait}); !step) {
                report("session bus dispatch", step.error().code());
                running.store(false);
                bus.value().service().shutdown(); receiver.stop(); return 1;
            }
        }
        running.store(false);
        bus.value().service().shutdown(); receiver.stop();
        log_line("{\"event\":\"stopping\"}");
        return 0;
    }
    std::optional<aa::trust::TransportIdentity> identity;
    for (const auto& phone : store.phones()) {
        if (phone.phone_id == options->phone && std::holds_alternative<aa::trust::WirelessIdentity>(phone.identity))
            identity = phone.identity;
    }
    if (!identity) {
        std::fputs("aa-wireless-receiver: --phone-id is not an approved wireless identity\n", stderr);
        return 3;
    }
    try {
        const auto credentials = aa::tls::load_credentials();
    } catch (const aa::tls::Error& error) {
        std::fprintf(stderr, "aa-wireless-receiver: %s\n", error.what()); return 3;
    }
    DiagnosticOutput output{options->diagnostics};
    aa::diagnostics::Diagnostics diagnostics{output};
    aa::session::SteadyClock clock;
    aa::trust::SteadyClock trust_clock;
    aa::trust::PairingPolicy phones{store, trust_clock};
    aa::ipc::RandomPairingIdSource ids;
    aa::audio::AudioRenderWorker audio_sink{
        audio_config(options->audio_backend,
                     aa::core::Nanoseconds{options->audio_catch_up_watermark_ms * 1'000'000LL},
                     aa::core::Nanoseconds{options->audio_playout_ms * 1'000'000LL}),
        audio_telemetry(), aa::audio::AudioRouter::open, aa::audio::AudioRenderWorker::monotonic_now,
        aa::audio::AudioRenderWorker::kDefaultAckReleaseDrainTimeout,
        aa::audio::AudioCreditPolicy{options->audio_max_unacked}};
    const auto opened = audio_sink.ready();
    if (!opened) { running.store(false); }
    if (const auto report_sink = audio_sink.sink_report();
        opened || !report_sink.cause.empty() || !report_sink.candidates.empty()) {
        aa::audio::log_sink_selected(report_sink, "open", audio_telemetry());
    }
    if (!opened) {
        report("audio router", opened.error().code());
        log_line(aa::audio::audio_failure_event_json(
            "audio_router", opened.error().code(), audio_sink.failure_context()));
        return 3;
    }
    Media media{audio_sink};
    const auto video_hold = video_delay(options->video_delay_ms, options->audio_playout_ms);
    const bool follow = !options->video_delay_ms && options->audio_playout_ms > 0;
    if (follow) {
        // match: follow the audio playout offset, starting at the depth.
        media.match_video_to_audio(video_hold);
    } else {
        media.video.set_presentation_delay(video_hold);
    }
    // Held frames leave at their due on the outlet's own timer, not only
    // when the owner thread next reaches a checkpoint (window 14).
    if (follow || video_hold.count() > 0) {
        try {
            media.video.start_release_timer();
        } catch (const std::system_error&) {
            // Thread creation failed (EAGAIN under thread or memory limits).
            report("video release timer", ErrorCode::internal);
            log_line("{\"event\":\"video_release_timer_error\",\"reason\":\"start_failed\"}");
            return 3;
        }
    }
    // video_delay_ms is always a number: the fixed hold, or the hold used
    // until the first playout write when video_hold is "match".
    log_line("{\"event\":\"wireless_media_policy\",\"audio_playout_ms\":" +
             std::to_string(options->audio_playout_ms) + ",\"audio_max_unacked\":" +
             std::to_string(options->audio_max_unacked) + ",\"video_hold\":\"" +
             (follow ? "match" : "fixed") + "\",\"video_delay_ms\":" +
             std::to_string(video_hold.count()) + "}");
    // Receiver precedes ControlService so its confirmation observer is valid
    // throughout bus dispatch and the bus's final owner teardown.
    auto link_config = media.config(options->video_profiles);
    link_config.ap_channel = options->ap_channel;
    aa::app::WirelessReceiver receiver{{store, clock, diagnostics}, *helper.value(), std::move(link_config)};
    media.send = [&receiver](aa::protocol::ChannelRole role, std::vector<std::byte> payload) {
        auto* session = receiver.session();
        return session ? session->send_channel_payload(role, std::move(payload))
            : aa::core::Result<void>{aa::Error{aa::ErrorCode::transport_closed}};
    };
    media.video.set_idr_recovery({
        [&receiver] {
            auto* session = receiver.session();
            return session && session->video_projecting();
        },
        [&receiver] {
            auto* session = receiver.session();
            return session ? session->request_video_idr()
                           : aa::core::Result<void>{aa::Error{aa::ErrorCode::transport_closed}};
        },
        {},
        [&receiver](bool focus_transition) {
            auto* session = receiver.session();
            return session ? session->request_video_idr(focus_transition)
                           : aa::core::Result<void>{aa::Error{aa::ErrorCode::transport_closed}};
        },
        options->idr_recovery,
    });
    auto bus = aa::ipc::ControlBusService::start({aa::ipc::current_uid()},
        {ids, phones, &receiver.confirmation()});
    if (!bus) { report("session control bus", bus.error().code()); return 3; }
    auto video = aa::ipc::VideoServer::listen(aa::ipc::VideoSocketPath{options->video_socket},
        aa::ipc::VideoServerOptions{{}, aa::ipc::current_uid()});
    if (!video) { report("video socket", video.error().code()); return 3; }
    auto input = aa::ipc::InputServer::listen(aa::ipc::InputSocketPath{options->input_socket},
        aa::ipc::InputServerOptions{aa::ipc::current_uid()});
    if (!input) { report("input socket", input.error().code()); return 3; }
    struct sigaction action{};
    action.sa_handler = on_signal;
    ::sigemptyset(&action.sa_mask);
    if (::sigaction(SIGINT, &action, nullptr) != 0 || ::sigaction(SIGTERM, &action, nullptr) != 0) {
        std::fputs("aa-wireless-receiver: signal handler installation failed\n", stderr); return 3;
    }
    const auto deadline = options->duration_ms ? Runtime::Clock::now() + std::chrono::milliseconds{options->duration_ms}
        : Runtime::Clock::time_point::max();
    Runtime runtime{receiver, *helper.value(), bus.value(), video.value(), input.value(), media,
        *identity, deadline, std::nullopt, {}, std::nullopt};
    runtime.offered_video_profiles = options->video_profiles.size();
    log_line("{\"event\":\"ready\",\"state\":\"waiting_for_consumer\"}");
    return runtime.run();
}
} // namespace aa::wireless_receiver

int main(int argc, char** argv) {
    aa::diagnostics::StdoutLogScope logging{aa::wireless_receiver::running};
    const int code = aa::wireless_receiver::receiver_main(argc, argv);
    const auto error = logging.log.finish();
    return code != 0 ? code : (error ? 1 : 0);
}
