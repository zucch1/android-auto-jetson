// SPDX-License-Identifier: GPL-3.0-or-later
#include "Runtime.hpp"
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
    "  --video-socket PATH   encoded H.264 consumer socket\n"
    "  --input-socket PATH   normalized display input socket\n"
    "  --helper-bus-address ADDRESS  explicit alternate helper bus (default: system bus)\n"
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
    std::int64_t duration_ms{};
    std::uint32_t ap_channel{36};
    bool diagnostics{};
    bool pairing_only{};
};

std::optional<Options> parse_options(int argc, char** argv) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg{argv[i]};
        if (arg == "--diagnostics") { options.diagnostics = true; continue; }
        if (arg == "--pairing-only") { options.pairing_only = true; continue; }
        if (i + 1 == argc) return std::nullopt;
        const std::string_view value{argv[++i]};
        if (value.empty() || value.starts_with("--")) return std::nullopt;
        if (arg == "--store-dir") options.store_dir = value;
        else if (arg == "--video-socket") options.video_socket = value;
        else if (arg == "--input-socket") options.input_socket = value;
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
    return options;
}
}

int main(int argc, char** argv) {
    using namespace aa::wireless_receiver;
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
            {ids, phones, &receiver.confirmation()});
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
        while (running && Runtime::Clock::now() < deadline) {
            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Runtime::Clock::now()).count();
            const auto wait = options->duration_ms && remaining < 50 ? remaining : 50;
            if (wait < 0) break;
            if (auto step = bus.value().cycle(aa::core::Milliseconds{wait}); !step) {
                report("session bus dispatch", step.error().code());
                bus.value().service().shutdown(); receiver.stop(); return 1;
            }
        }
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
    auto router = aa::audio::AudioRouter::open(audio_config());
    if (!router) { report("audio router", router.error().code()); return 3; }
    Media media{router.value()};
    // Receiver precedes ControlService so its confirmation observer is valid
    // throughout bus dispatch and the bus's final owner teardown.
    auto link_config = media.config();
    link_config.ap_channel = options->ap_channel;
    aa::app::WirelessReceiver receiver{{store, clock, diagnostics}, *helper.value(), std::move(link_config)};
    media.send = [&receiver](aa::protocol::ChannelRole role, std::vector<std::byte> payload) {
        auto* session = receiver.session();
        return session ? session->send_channel_payload(role, std::move(payload))
            : aa::core::Result<void>{aa::Error{aa::ErrorCode::transport_closed}};
    };
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
    log_line("{\"event\":\"ready\",\"state\":\"waiting_for_consumer\"}");
    return runtime.run();
}
