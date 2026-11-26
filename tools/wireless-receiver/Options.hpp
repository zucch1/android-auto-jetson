// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "../media_playout_options.hpp"
#include <aa/audio/AudioRenderWorker.hpp>
#include <aa/audio/AudioRouter.hpp>
#include <aa/config/VideoProfiles.hpp>
#include <aa/helper/SystemHelper.hpp>
#include <aa/ipc/IdrRecovery.hpp>
#include <aa/ipc/InputChannel.hpp>
#include <aa/ipc/VideoChannel.hpp>
#include <aa/trust/Store.hpp>
#include <charconv>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace aa::wireless_receiver {

#if defined(AA_WIRELESS_ALLOW_HELPER_BUS_ADDRESS) && AA_WIRELESS_ALLOW_HELPER_BUS_ADDRESS
inline constexpr bool kAllowHelperBusAddress = true;
#else
inline constexpr bool kAllowHelperBusAddress = false;
#endif

inline constexpr const char *usage =
    "usage: aa-wireless-receiver --store-dir PATH (--phone-id N | --pairing-only) [options]\n"
    "  PATH contains approved-phones.json; N selects an approved wireless phone.\n"
    "  --pairing-only       start in pairing mode; the consumer can enable projection.\n"
    "  --video-socket PATH   encoded video consumer socket\n"
    "  --input-socket PATH   normalized display input socket\n"
    "  --video-profiles WxH@FPS:CODEC,...  offered video profiles (same syntax as\n"
    "                       the wired receiver; default 1280x720@30:h264,800x480@30:h264)\n"
    "  --exact-video-profiles offer only --video-profiles without appending the\n"
    "                       1280x720@30:h264 fallback profile\n"
#if defined(AA_WIRELESS_ALLOW_HELPER_BUS_ADDRESS) && AA_WIRELESS_ALLOW_HELPER_BUS_ADDRESS
    "  --helper-bus-address ADDRESS  explicit alternate helper bus (default: system bus)\n"
#endif
    "  --idr-recovery auto|nudge|transition  IDR recovery strategy (default auto)\n"
    "  --audio-sink pulse|pipewire|automatic  audio routing element (default pulse)\n"
    "  --audio-catch-up-watermark-ms N        post-stall catch-up watermark in ms\n"
    "                                         (0 disables catch-up, default 0; else 20..200;\n"
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
#if defined(AA_WIRELESS_ALLOW_HELPER_BUS_ADDRESS) && AA_WIRELESS_ALLOW_HELPER_BUS_ADDRESS
    std::string helper_address;
#endif
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

inline std::optional<Options> parse_options(int argc, const char *const *argv,
                                            std::string *error_out = nullptr) {
    if (error_out)
        error_out->clear();
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg{argv[i]};
        if (arg == "--diagnostics") {
            options.diagnostics = true;
            continue;
        }
        if (arg == "--pairing-only") {
            options.pairing_only = true;
            continue;
        }
        if (arg == "--exact-video-profiles") {
            options.exact_video_profiles = true;
            continue;
        }
        if (arg == "--helper-bus-address" || arg.starts_with("--helper-bus-address=")) {
#if defined(AA_WIRELESS_ALLOW_HELPER_BUS_ADDRESS) && AA_WIRELESS_ALLOW_HELPER_BUS_ADDRESS
            if (arg != "--helper-bus-address" || i + 1 == argc)
                return std::nullopt;
            const std::string_view value{argv[++i]};
            if (value.empty() || value.starts_with("--"))
                return std::nullopt;
            options.helper_address = value;
            continue;
#else
            if (error_out) {
                *error_out =
                    "aa-wireless-receiver: --helper-bus-address is only supported in test builds\n";
            }
            return std::nullopt;
#endif
        }
        if (i + 1 == argc)
            return std::nullopt;
        const std::string_view value{argv[++i]};
        if (value.empty() || value.starts_with("--"))
            return std::nullopt;
        if (arg == "--store-dir")
            options.store_dir = value;
        else if (arg == "--video-socket")
            options.video_socket = value;
        else if (arg == "--input-socket")
            options.input_socket = value;
        else if (arg == "--video-profiles") {
            if (!aa::config::parse_video_profiles_option(value, options.video_profiles))
                return std::nullopt;
        } else if (arg == "--idr-recovery") {
            if (!aa::ipc::parse_idr_recovery_strategy(value, options.idr_recovery))
                return std::nullopt;
        } else if (arg == "--audio-sink") {
            if (value == "pulse")
                options.audio_backend = audio::SinkBackend::pulse;
            else if (value == "pipewire")
                options.audio_backend = audio::SinkBackend::pipewire;
            else if (value == "automatic")
                options.audio_backend = audio::SinkBackend::automatic;
            else
                return std::nullopt;
        } else if (arg == "--audio-catch-up-watermark-ms") {
            if (!aa::tools::parse_audio_catch_up_watermark_ms(
                    value, options.audio_catch_up_watermark_ms))
                return std::nullopt;
        } else if (arg == "--audio-playout-ms") {
            if (!aa::tools::parse_audio_playout_ms(value, options.audio_playout_ms))
                return std::nullopt;
        } else if (arg == "--audio-max-unacked") {
            std::uint64_t number{};
            if (!aa::tools::parse_uint64_strict(value, number) ||
                number == 0 || number > audio::AudioCreditPolicy::kMaxWindow)
                return std::nullopt;
            options.audio_max_unacked = static_cast<std::uint32_t>(number);
        } else if (arg == "--video-delay-ms") {
            if (!aa::tools::parse_video_delay_ms(value, options.video_delay_ms))
                return std::nullopt;
        } else if (arg == "--ap-channel") {
            std::uint32_t number{};
            const auto [end, error] =
                std::from_chars(value.data(), value.data() + value.size(), number);
            if (error != std::errc{} || end != value.data() + value.size() ||
                !helper::ap_channel_allowed(number))
                return std::nullopt;
            options.ap_channel = number;
        } else if (arg == "--phone-id" || arg == "--duration-ms") {
            std::uint64_t number{};
            const auto [end, error] =
                std::from_chars(value.data(), value.data() + value.size(), number);
            if (error != std::errc{} || end != value.data() + value.size() || number == 0)
                return std::nullopt;
            if (arg == "--phone-id") {
                if (number > trust::kMaxPhoneId)
                    return std::nullopt;
                options.phone = core::PhoneId{number};
            } else {
                if (number > 86'400'000)
                    return std::nullopt;
                options.duration_ms = static_cast<std::int64_t>(number);
            }
        } else
            return std::nullopt;
    }
    if (options.store_dir.empty() ||
        (options.pairing_only ? options.phone.value != 0 : options.phone.value == 0) ||
        !options.store_dir.is_absolute() ||
        !std::filesystem::path{options.video_socket}.is_absolute() ||
        !std::filesystem::path{options.input_socket}.is_absolute() ||
        options.video_socket == options.input_socket)
        return std::nullopt;
    if (!aa::tools::validate_playout_options({options.audio_playout_ms,
                                              options.audio_catch_up_watermark_ms,
                                              options.video_delay_ms}))
        return std::nullopt;
    aa::config::ensure_fallback_video_profile(options.video_profiles, options.exact_video_profiles);
    return options;
}

} // namespace aa::wireless_receiver
