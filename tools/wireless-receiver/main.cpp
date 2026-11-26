// SPDX-License-Identifier: GPL-3.0-or-later
#include "Options.hpp"
#include "Runtime.hpp"
#include "WiredWorker.hpp"
#include <aa/tls/Credentials.hpp>
#include <aa/trust/PairingPolicy.hpp>
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <optional>
#include <string>
#include <system_error>
#include <thread>
#include <variant>
#include <vector>

namespace aa::wireless_receiver {

int run_wireless_projection(const Options& options, app::WirelessReceiver& receiver,
                            helper::BusClient& helper, ipc::ControlBusService& bus,
                            const trust::TransportIdentity& identity,
                            Runtime::Clock::time_point deadline, ipc::VideoServer& video,
                            ipc::InputServer& input,
                            std::function<bool(ipc::StateSnapshot)> keep_running = {}) {
    aa::audio::AudioRenderWorker audio_sink{
        audio_config(options.audio_backend,
                     aa::core::Nanoseconds{options.audio_catch_up_watermark_ms * 1'000'000LL},
                     aa::core::Nanoseconds{options.audio_playout_ms * 1'000'000LL}),
        audio_telemetry(),
        aa::audio::AudioRouter::open,
        aa::audio::AudioRenderWorker::monotonic_now,
        aa::audio::AudioRenderWorker::kDefaultAckReleaseDrainTimeout,
        aa::audio::AudioCreditPolicy{options.audio_max_unacked}};
    const auto opened = audio_sink.ready();
    if (const auto report_sink = audio_sink.sink_report();
        opened || !report_sink.cause.empty() || !report_sink.candidates.empty()) {
        aa::audio::log_sink_selected(report_sink, "open", audio_telemetry());
    }
    if (!opened) {
        report("audio router", opened.error().code());
        log_line(aa::audio::audio_failure_event_json("audio_router", opened.error().code(),
                                                     audio_sink.failure_context()));
        return 3;
    }
    Media media{audio_sink};
    const aa::tools::PlayoutOptions playout_options{options.audio_playout_ms,
                                                    options.audio_catch_up_watermark_ms,
                                                    options.video_delay_ms};
    std::chrono::milliseconds video_hold{};
    bool follow = false;
    aa::tools::configure_video_hold(media.video, media.audio, playout_options, follow, video_hold);
    // Held frames leave at their due on the outlet's own timer, not only
    // when the owner thread next reaches a checkpoint (window 14).
    if (follow || video_hold.count() > 0) {
        try {
            media.video.start_release_timer();
        } catch (const std::system_error &) {
            // Thread creation failed (EAGAIN under thread or memory limits).
            report("video release timer", ErrorCode::internal);
            log_line("{\"event\":\"video_release_timer_error\",\"reason\":\"start_failed\"}");
            return 3;
        }
    }
    // video_delay_ms is always a number: the fixed hold, or the hold used
    // until the first playout write when video_hold is "match".
    log_line(aa::tools::media_policy_json("wireless_media_policy", playout_options,
                                          options.audio_max_unacked, follow, video_hold));
    // Receiver precedes ControlService so its confirmation observer is valid
    // throughout bus dispatch and the bus's final owner teardown.
    auto link_config = media.config(options.video_profiles);
    link_config.ap_channel = options.ap_channel;
    receiver.configure(std::move(link_config));
    struct ClearLinkConfig {
        app::WirelessReceiver& receiver;
        ~ClearLinkConfig() { receiver.configure({}); }
    } clear_link_config{receiver};
    media.send = [&receiver](aa::protocol::ChannelRole role, std::vector<std::byte> payload) {
        auto *session = receiver.session();
        return session ? session->send_channel_payload(role, std::move(payload))
                       : aa::core::Result<void>{aa::Error{aa::ErrorCode::transport_closed}};
    };
    media.video.set_idr_recovery({
        [&receiver] {
            auto *session = receiver.session();
            return session && session->video_projecting();
        },
        [&receiver] {
            auto *session = receiver.session();
            return session ? session->request_video_idr()
                           : aa::core::Result<void>{aa::Error{aa::ErrorCode::transport_closed}};
        },
        {},
        [&receiver](bool focus_transition) {
            auto *session = receiver.session();
            return session ? session->request_video_idr(focus_transition)
                           : aa::core::Result<void>{aa::Error{aa::ErrorCode::transport_closed}};
        },
        options.idr_recovery,
    });
    Runtime runtime{receiver, helper, bus, video, input,
                    media, identity, deadline, std::nullopt, {}, std::nullopt};
    runtime.keep_running = std::move(keep_running);
    runtime.offered_video_profiles = options.video_profiles.size();
    log_line("{\"event\":\"ready\",\"state\":\"waiting_for_consumer\"}");
    return runtime.run();
}

core::Result<void> check_projection_credentials() {
    try {
        (void)tls::load_credentials();
        return {};
    } catch (const tls::Error& error) {
        std::fprintf(stderr, "aa-wireless-receiver: %s\n", error.what());
        return Error{ErrorCode::session_illegal_transition};
    }
}

struct ProjectionSockets final {
    ipc::VideoServer video;
    ipc::InputServer input;

    static core::Result<ProjectionSockets> listen(const Options& options) {
        auto video = ipc::VideoServer::listen({options.video_socket}, {{}, ipc::current_uid()});
        if (!video) return video.error();
        auto input = ipc::InputServer::listen({options.input_socket}, {ipc::current_uid()});
        if (!input) return input.error();
        return ProjectionSockets{std::move(video.value()), std::move(input.value())};
    }
};

int run_managed(const Options& options, trust::ApprovedPhoneStore& store, helper::BusClient& helper,
                app::WirelessReceiver& receiver, ipc::PairingIdSource& ids, ipc::PhoneDirectory& phones,
                Runtime::Clock::time_point deadline) {
    std::optional<ProjectionSockets> sockets;
    ipc::ProjectionPolicy policy{};
    policy.check_ready = [&]() -> core::Result<void> {
        if (auto credentials = check_projection_credentials(); !credentials) return credentials.error();
        auto opened = ProjectionSockets::listen(options);
        if (!opened) {
            report("projection sockets", opened.error().code());
            return Error{ErrorCode::session_illegal_transition};
        }
        sockets.emplace(std::move(opened.value()));
        return {};
    };
    WiredWorker wired;
    auto bus = ipc::ControlBusService::start({ipc::current_uid()},
        {ids, phones, &receiver.confirmation(),
         [&] { return receiver.active() || wired.running(); }, &policy});
    if (!bus) {
        report("session control bus", bus.error().code());
        return 3;
    }

    std::error_code path_error;
    const auto wired_executable = std::filesystem::read_symlink("/proc/self/exe", path_error)
                                      .parent_path() / "aa-wired-receiver";
    const auto store_path = options.store_dir / "approved-phones.json";
    std::optional<core::PhoneId> wired_phone;
    std::optional<WiredCandidate> last_wired_candidate;
    Runtime::Clock::time_point next_usb_scan{};
    log_line("{\"event\":\"ready\",\"state\":\"pairing_only\"}");
    while (running.load() && Runtime::Clock::now() < deadline) {
        const auto remaining = std::chrono::ceil<std::chrono::milliseconds>(
            deadline - Runtime::Clock::now()).count();
        const core::Milliseconds wait{std::clamp<std::int64_t>(remaining, 0, 50)};
        if (auto step = bus.value().cycle(wait); !step) {
            report("session bus dispatch", step.error().code());
            return 1;
        }
        const auto state = bus.value().service().state();
        const bool wants_projection = state.consumer.value && policy.ready &&
                                      state.projection == ipc::ProjectionState::projecting;
        if (!wants_projection) {
            wired.stop();
            wired_phone.reset();
            policy.active_transport.reset();
            next_usb_scan = {};
            if (!policy.ready) sockets.reset();
            continue;
        }
        const auto& approved = store.phones();
        if (wired_phone) {
            if (std::none_of(approved.begin(), approved.end(), [&](const auto& phone) {
                    return phone.phone_id == *wired_phone;
                })) {
                wired.stop();
            }
            if (wired.running()) {
                if (bus.value().service().consume_idr_recovery_request()) wired.request_idr();
                continue;
            }
            wired_phone.reset();
            policy.active_transport.reset();
            bus.value().service().projection_ended();
            continue;
        }

        const auto now = Runtime::Clock::now();
        if (policy.wired && !path_error && now >= next_usb_scan) {
            next_usb_scan = now + std::chrono::milliseconds{250};
            if (const auto candidate = find_wired_candidate(approved, last_wired_candidate)) {
                if (wired.start(wired_executable, *candidate, store_path,
                                sockets->video.native_handle(), sockets->input.native_handle())) {
                    wired_phone = candidate->phone;
                    last_wired_candidate = candidate;
                    policy.active_transport = ipc::ProjectionTransport::wired;
                    continue;
                }
                report("wired projection worker", ErrorCode::transport_unavailable);
            }
        }

        if (policy.wireless) {
            const auto phone = std::find_if(approved.begin(), approved.end(), [](const auto& candidate) {
                return std::holds_alternative<trust::WirelessIdentity>(candidate.identity);
            });
            if (phone != approved.end()) {
                const auto phone_id = phone->phone_id;
                const auto identity = phone->identity;
                policy.active_transport = ipc::ProjectionTransport::wireless;
                const auto result = run_wireless_projection(options, receiver, helper, bus.value(),
                    identity, deadline, sockets->video, sockets->input, [&](ipc::StateSnapshot current) {
                        const auto& current_phones = store.phones();
                        return policy.wireless && current.consumer == state.consumer &&
                               current.projection == ipc::ProjectionState::projecting &&
                               std::any_of(current_phones.begin(), current_phones.end(), [&](const auto& entry) {
                                   return entry.phone_id == phone_id;
                               });
                    });
                policy.active_transport.reset();
                bus.value().service().projection_ended();
                if (!policy.ready) sockets.reset();
                if (result != 0 && !running.load()) return result;
            }
        }
    }
    wired.stop();
    bus.value().service().shutdown();
    log_line("{\"event\":\"stopping\"}");
    return 0;
}

int receiver_main(int argc, char **argv) {
    if (argc == 2 && std::strcmp(argv[1], "--help") == 0) {
        std::fputs(usage, stdout);
        return 0;
    }
    std::string parse_error;
    const auto options = parse_options(argc, argv, &parse_error);
    if (!options) {
        if (!parse_error.empty())
            std::fputs(parse_error.c_str(), stderr);
        std::fputs(usage, stderr);
        return 2;
    }
    std::error_code fs_error;
    if (!std::filesystem::is_directory(options->store_dir, fs_error) || fs_error) {
        std::fputs("aa-wireless-receiver: --store-dir must name an existing directory\n", stderr);
        return 3;
    }
    aa::trust::ApprovedPhoneStore store{options->store_dir / "approved-phones.json"};
    if (auto loaded = store.load(); !loaded) {
        report("trust store", loaded.error().code());
        return 3;
    }
#if defined(AA_WIRELESS_ALLOW_HELPER_BUS_ADDRESS) && AA_WIRELESS_ALLOW_HELPER_BUS_ADDRESS
    const auto* same_uid_fixture = std::getenv("AA_WIRELESS_TEST_SAME_UID_HELPER");
    const auto open_private_helper =
        same_uid_fixture && std::strcmp(same_uid_fixture, "1") == 0
            ? aa::helper::BusClient::private_bus_for_test
            : aa::helper::BusClient::private_bus;
    auto helper = options->helper_address.empty()
                      ? aa::helper::BusClient::system(store)
                      : open_private_helper(options->helper_address, store);
#else
    auto helper = aa::helper::BusClient::system(store);
#endif
    if (!helper) {
        report("system helper connection", helper.error().code());
        return 3;
    }
    using namespace std::chrono_literals;
    // Leave one second of the startup budget for the final status RPC.
    const auto retry_until = Runtime::Clock::now() + 4s;
    for (auto delay = 50ms;; delay = std::min(delay * 2, 500ms)) {
        const auto status = helper.value()->status(aa::helper::Action::ap_status);
        if (status)
            break;
        const auto error = status.error().code();
        if (error == ErrorCode::helper_unavailable && helper.value()->connected() &&
            Runtime::Clock::now() + delay < retry_until) {
            std::this_thread::sleep_for(delay);
            continue;
        }
        report("org.custom.AndroidAutoSystemHelper AP status", error);
        return 3;
    }
    struct sigaction action {};
    action.sa_handler = on_signal;
    ::sigemptyset(&action.sa_mask);
    if (::sigaction(SIGINT, &action, nullptr) != 0 || ::sigaction(SIGTERM, &action, nullptr) != 0) {
        std::fputs("aa-wireless-receiver: signal handler installation failed\n", stderr);
        return 3;
    }
    const auto deadline =
        options->duration_ms
            ? Runtime::Clock::now() + std::chrono::milliseconds{options->duration_ms}
            : Runtime::Clock::time_point::max();
    DiagnosticOutput output{options->diagnostics};
    aa::diagnostics::Diagnostics diagnostics{output};
    aa::session::SteadyClock clock;
    aa::trust::SteadyClock trust_clock;
    aa::trust::PairingPolicy phones{store, trust_clock};
    aa::ipc::RandomPairingIdSource ids;
    aa::app::WirelessReceiver receiver{
        {store, clock, diagnostics}, *helper.value(), aa::app::WirelessLink::Config{}};
    if (options->pairing_only) {
        return run_managed(*options, store, *helper.value(), receiver, ids, phones, deadline);
    }
    std::optional<aa::trust::TransportIdentity> identity;
    for (const auto &phone : store.phones()) {
        if (phone.phone_id == options->phone &&
            std::holds_alternative<aa::trust::WirelessIdentity>(phone.identity))
            identity = phone.identity;
    }
    if (!identity) {
        std::fputs("aa-wireless-receiver: --phone-id is not an approved wireless identity\n",
                   stderr);
        return 3;
    }
    if (!check_projection_credentials()) return 3;
    auto bus = aa::ipc::ControlBusService::start({aa::ipc::current_uid()},
                                                 {ids, phones, &receiver.confirmation(),
                                                  [&receiver] { return receiver.active(); }});
    if (!bus) {
        report("session control bus", bus.error().code());
        return 3;
    }
    auto sockets = ProjectionSockets::listen(*options);
    if (!sockets) {
        report("projection sockets", sockets.error().code());
        return 3;
    }
    return run_wireless_projection(*options, receiver, *helper.value(), bus.value(), *identity,
                                   deadline, sockets.value().video, sockets.value().input);
}
} // namespace aa::wireless_receiver

int main(int argc, char **argv) {
    aa::diagnostics::StdoutLogScope logging{aa::wireless_receiver::running};
    const int code = aa::wireless_receiver::receiver_main(argc, argv);
    const auto error = logging.log.finish();
    return code != 0 ? code : (error ? 1 : 0);
}
