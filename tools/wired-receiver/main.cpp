// SPDX-License-Identifier: GPL-3.0-or-later
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

#include <aa/app/WiredLink.hpp>
#include <aa/app/WiredSession.hpp>
#include <aa/audio/AudioRouter.hpp>
#include <aa/diagnostics/Diagnostics.hpp>
#include <aa/ipc/ControlBusService.hpp>
#include <aa/ipc/ControlService.hpp>
#include <aa/ipc/InputChannel.hpp>
#include <aa/ipc/InputSocket.hpp>
#include <aa/ipc/PairingGate.hpp>
#include <aa/ipc/VideoChannel.hpp>
#include <aa/ipc/VideoSocket.hpp>
#include <aa/protocol/Messages.hpp>
#include <aa/protocol/Protocol.hpp>
#include <aa/session/Events.hpp>
#include <aa/session/Session.hpp>
#include <aa/trust/Store.hpp>

#include <atomic>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <system_error>
#include <thread>
#include <variant>
#include <vector>

#include <signal.h>

namespace {

using aa::core::Milliseconds;

std::atomic<bool> g_running{true};
std::mutex g_log_mutex;

void log_line(const std::string &json) {
    const std::lock_guard<std::mutex> lock(g_log_mutex);
    std::fputs(json.c_str(), stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout);
}

void on_signal(int) { g_running.store(false); }

// Task-48 media-timing diagnostics: nanoseconds -> milliseconds as an exact
// rational (ns / 1e6) carried in a double, with no lossy pre-rounding - the
// collector and reporter compare the value as-is against the ms budgets.
// Non-finite input (not producible from an int64 count) degrades to a JSON
// null, which the collector treats as an absent metric (BLOCKED), never 0.
std::string format_ms(aa::core::Nanoseconds value) {
    const double ms = static_cast<double>(value.count) / 1'000'000.0;
    if (!std::isfinite(ms)) {
        return "null";
    }
    char buffer[32];
    const auto [end, error] =
        std::to_chars(buffer, buffer + sizeof(buffer), ms, std::chars_format::general);
    if (error != std::errc{}) {
        return "null";
    }
    return std::string{buffer, end};
}

class StdoutDiagnostics final : public aa::diagnostics::DiagnosticSink {
  public:
    void emit_json(std::string_view redacted_json) override {
        log_line(std::string{"{\"event\":\"diag\","} + std::string{redacted_json}.substr(1));
    }
};

class StorePhones final : public aa::ipc::PhoneDirectory {
  public:
    explicit StorePhones(aa::trust::ApprovedPhoneStore &store) : store_(store) {}

    [[nodiscard]] aa::core::Result<void> forget(aa::core::PhoneId phone) override {
        return store_.forget_phone(phone);
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
    aa::trust::ApprovedPhoneStore &store_;
};

bool is_idr_au(const std::vector<std::uint8_t> &payload) {
    for (std::size_t index = 0; index + 4 < payload.size(); ++index) {
        if (payload[index] != 0 || payload[index + 1] != 0) {
            continue;
        }
        std::size_t nal = 0;
        if (payload[index + 2] == 1) {
            nal = index + 3;
        } else if (payload[index + 2] == 0 && payload[index + 3] == 1) {
            nal = index + 4;
        } else {
            continue;
        }
        if (nal < payload.size() && (payload[nal] & 0x1FU) == 5U) {
            return true;
        }
    }
    return false;
}

class VideoOutlet final : public aa::protocol::MessageSink {
  public:
    [[nodiscard]] aa::core::Result<void>
    on_message(const aa::protocol::MessageView &message) override {
        std::lock_guard lock(mutex_);
        if (!producer_.has_value()) {
            return {};
        }
        std::vector<std::uint8_t> payload;
        payload.reserve(message.payload.size());
        for (const std::byte byte : message.payload) {
            payload.push_back(std::to_integer<std::uint8_t>(byte));
        }
        ++units_;
        const auto sent = producer_->send_access_unit(aa::ipc::VideoSendRequest{
            units_, static_cast<std::uint64_t>(message.header.timestamp.count), is_idr_au(payload),
            payload});
        if (!sent) {
            return sent.error();
        }
        if (units_ % 30 == 1) {
            log_line("{\"event\":\"video_sent\",\"units\":" + std::to_string(units_) + "}");
        }
        return {};
    }

    void install(aa::ipc::VideoProducer producer) {
        std::lock_guard lock(mutex_);
        if (producer_.has_value()) {
            producer_->close();
        }
        producer_.emplace(std::move(producer));
    }

    void close() noexcept {
        std::lock_guard lock(mutex_);
        if (producer_.has_value()) {
            producer_->close();
            producer_.reset();
        }
    }

  private:
    std::mutex mutex_{};
    std::optional<aa::ipc::VideoProducer> producer_{};
    std::uint64_t units_{0};
};

class RouterSink final : public aa::protocol::MessageSink {
  public:
    explicit RouterSink(aa::audio::AudioRouter &router) : router_(router) {}

    [[nodiscard]] aa::core::Result<void>
    on_message(const aa::protocol::MessageView &message) override {
        if (auto submitted = router_.submit(message); !submitted) {
            return submitted.error();
        }
        ++chunks_;
        if (chunks_ % 50 == 1) {
            log_line("{\"event\":\"audio_chunks\",\"count\":" + std::to_string(chunks_) + "}");
        }
        auto pumped = router_.pump();
        if (!pumped) {
            return pumped.error();
        }
        // Task-48: only after a successful pump do the router's statistics
        // carry a measured render completion. Emit on the same low-rate
        // cadence as audio_chunks (the receiver's event stream is documented
        // low-rate). The router runs entirely on this session thread, so
        // reading report() here is race-free.
        if (chunks_ % 50 == 1) {
            emit_media_timing();
        }
        return {};
    }

  private:
    // Task-48 media-timing diagnostics. Both gated phase-B metrics derive from
    // existing task-25 RouterStats observations on the common monotonic A/V
    // clock; nothing here invents a value:
    //   audio_latency_ms = max_path_latency    (submit() BEFORE decode, gate
    //                       fix F4, to DATA-HANDOFF-CONFIRMED WRITE COMPLETION
    //                       at the routing element, gate r3 F3; receiver-local
    //                       steady clock; NOT a device-delivery measurement)
    //   av_sync_ms       = max_completion_skew  (that write completion minus
    //                       the piece's SCHEDULED PRESENTATION TIME on the
    //                       common monotonic A/V clock - the wire message
    //                       timestamp-anchored sample timeline; the audio-side
    //                       A/V-sync envelope the task-25 gate binds to the
    //                       <=50 ms budget; NOT a video-presentation
    //                       comparison and NOT device delivery)
    // MAX (cumulative peak), not the last sample: the schema budgets
    // (av_sync_ms_max / audio_latency_ms_max) are run/soak envelope ceilings,
    // so the retained figure must be the worst observed - a transient breach
    // must never hide behind a healed later sample (misleading-success
    // guard). The maxima are read live from the router report at each
    // emission (current stats, never a session-start snapshot). Numeric
    // values only - no identifiers. When no audio has traversed the route
    // there is no measurement: emit nothing rather than a fabricated 0, so
    // the collector keeps the metric null and the verdict stays BLOCKED.
    void emit_media_timing() {
        const auto &stats = router_.report().stats;
        if (stats.chunks_submitted == 0) {
            return;
        }
        log_line(
            "{\"event\":\"media_timing\",\"av_sync_ms\":" + format_ms(stats.max_completion_skew) +
            ",\"audio_latency_ms\":" + format_ms(stats.max_path_latency) + "}");
    }

    aa::audio::AudioRouter &router_;
    std::uint64_t chunks_{0};
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

aa::audio::RouterConfig pcm_router_config() {
    aa::audio::RouterConfig config;
    config.output_format = aa::audio::AudioFormat{48000, 2, 16};
    for (auto &role : config.roles) {
        role.codec = aa::audio::Codec::pcm;
        role.input_format = aa::audio::AudioFormat{48000, 2, 16};
    }
    return config;
}

std::vector<aa::protocol::ServiceDescriptor>
service_descriptors(const aa::input::CapabilityDescriptor &input) {
    const aa::protocol::VideoConfiguration video{{{1280, 720, 30}, {800, 480, 30}}};
    const aa::protocol::AudioConfiguration audio{{{48000, 16, 2}}};
    return {
        {aa::protocol::ServiceKey{99}, aa::protocol::ChannelRole::video, video},
        {aa::protocol::ServiceKey{100}, aa::protocol::ChannelRole::media_audio, audio},
        {aa::protocol::ServiceKey{101}, aa::protocol::ChannelRole::guidance_audio, audio},
        {aa::protocol::ServiceKey{102}, aa::protocol::ChannelRole::system_audio, audio},
        {aa::protocol::ServiceKey{103}, aa::protocol::ChannelRole::speech_audio, audio},
        {aa::protocol::ServiceKey{104}, aa::protocol::ChannelRole::input,
         aa::protocol::ButtonConfiguration{input.keycodes()}},
    };
}

const char *const kUsage =
    "usage: aa-wired-receiver --port <usb-port-context> [--store PATH]\n"
    "                         [--video-socket PATH] [--input-socket PATH]\n"
    "                         [--duration-ms N] [--no-bus]\n"
    "  runs the wired session over USB/AOA with TrustStore admission;\n"
    "  the approved-phones store must already contain the phone (fail closed).\n"
    "  exit codes: 0 clean stop, 1 runtime failure, 2 usage error, 3 blocked.\n";

} // namespace

int main(int argc, char **argv) {
    std::string port;
    std::string store_path;
    std::string video_socket = aa::ipc::production_video_socket_path().value;
    std::string input_socket = aa::ipc::production_input_socket_path().value;
    std::int64_t duration_ms = 0;
    bool no_bus = false;
    for (int index = 1; index < argc; ++index) {
        if (std::strcmp(argv[index], "--port") == 0 && index + 1 < argc) {
            port = argv[++index];
        } else if (std::strcmp(argv[index], "--store") == 0 && index + 1 < argc) {
            store_path = argv[++index];
        } else if (std::strcmp(argv[index], "--video-socket") == 0 && index + 1 < argc) {
            video_socket = argv[++index];
        } else if (std::strcmp(argv[index], "--input-socket") == 0 && index + 1 < argc) {
            input_socket = argv[++index];
        } else if (std::strcmp(argv[index], "--duration-ms") == 0 && index + 1 < argc) {
            duration_ms = std::strtoll(argv[++index], nullptr, 10);
        } else if (std::strcmp(argv[index], "--no-bus") == 0) {
            no_bus = true;
        } else {
            std::fputs(kUsage, stderr);
            return 2;
        }
    }
    if (port.empty() || duration_ms < 0) {
        std::fputs(kUsage, stderr);
        return 2;
    }

    const auto started = std::chrono::steady_clock::now();
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

    StdoutDiagnostics sink;
    aa::diagnostics::Diagnostics diagnostics{sink};
    aa::session::SteadyClock clock{};
    aa::input::CapabilityDescriptor input = aa::input::CapabilityDescriptor::bench_defaults();

    aa::audio::RouterConfig audio_config = pcm_router_config();
    auto router = aa::audio::AudioRouter::open(audio_config);
    if (!router.has_value()) {
        log_line("{\"event\":\"err\",\"detail\":\"audio_router\"}");
        return 3;
    }
    RouterSink audio_sink{router.value()};

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

    aa::ipc::RandomPairingIdSource ids;
    StorePhones phones{store};
    std::optional<aa::ipc::ControlBusService> bus{};
    if (!no_bus) {
        auto started_bus = aa::ipc::ControlBusService::start(
            aa::ipc::ControlBusService::Config{aa::ipc::current_uid()},
            aa::ipc::ControlBusService::Deps{ids, phones});
        if (!started_bus.has_value()) {
            log_line("{\"event\":\"err\",\"detail\":\"dbus\"}");
            return 3;
        }
        bus.emplace(std::move(started_bus.value()));
    }

    aa::app::WiredSessionConfig session_config{};
    session_config.input_enabled = true;
    session_config.input = input;
    aa::app::WiredSession session{{store, clock, diagnostics}, session_config};
    if (!session.configure(service_descriptors(input)).has_value()) {
        log_line("{\"event\":\"err\",\"detail\":\"configure\"}");
        return 1;
    }
    VideoOutlet video_outlet;
    if (!session.register_sink(aa::protocol::ChannelRole::video, video_outlet).has_value() ||
        !session.register_sink(aa::protocol::ChannelRole::media_audio, audio_sink).has_value() ||
        !session.register_sink(aa::protocol::ChannelRole::guidance_audio, audio_sink).has_value() ||
        !session.register_sink(aa::protocol::ChannelRole::system_audio, audio_sink).has_value() ||
        !session.register_sink(aa::protocol::ChannelRole::speech_audio, audio_sink).has_value()) {
        log_line("{\"event\":\"err\",\"detail\":\"sink\"}");
        return 1;
    }

    aa::transport::UsbDeviceSelector selector{};
    selector.port_context = port;
    aa::transport::AoaAccessoryInfo accessory{};
    log_line("{\"event\":\"connecting\",\"port\":\"" + port + "\"}");
    auto link = aa::app::open_wired_link(selector, accessory, store);
    if (!link.has_value()) {
        log_line(std::string{"{\"event\":\"err\",\"detail\":\"link\",\"code\":\""} +
                 std::string{aa::to_string(link.error().code())} + "\"}");
        return 3;
    }
    session.attach_transport(link.value().transport.get());
    if (!session.apply(aa::session::Event::start_discovery).has_value() ||
        !session.phone_discovered(link.value().phone).has_value() ||
        !session.apply(aa::session::Event::transport_ready).has_value()) {
        log_line("{\"event\":\"err\",\"detail\":\"lifecycle\"}");
        return 1;
    }
    if (!session.send_version_request(1, 6).has_value()) {
        log_line("{\"event\":\"err\",\"detail\":\"version_request\"}");
        return 1;
    }
    log_line("{\"event\":\"ready\",\"state\":\"negotiating\"}");

    InputQueue input_queue;
    std::mutex session_mutex;
    std::atomic<std::int64_t> active_ms{-1};

    std::thread session_thread([&] {
        while (g_running.load()) {
            {
                std::lock_guard lock(session_mutex);
                while (auto event = input_queue.pop()) {
                    const auto outcome = session.submit_input(*event);
                    log_line(std::string{"{\"event\":\"input_forwarded\",\"ok\":\""} +
                             (outcome.has_value() ? "true" : "false") + "\"}");
                }
            }
            auto polled = session.poll_transport();
            {
                std::lock_guard lock(session_mutex);
                if (!polled.has_value()) {
                    log_line(std::string{"{\"event\":\"err\",\"detail\":\"poll\",\"code\":\""} +
                             std::string{aa::to_string(polled.error().code())} + "\"}");
                    break;
                }
                if (session.state() == aa::session::State::negotiating &&
                    session.channel_opens() > 0) {
                    (void)session.apply(aa::session::Event::negotiation_succeeded);
                }
                if (session.state() == aa::session::State::active && active_ms.load() < 0) {
                    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                                             std::chrono::steady_clock::now() - started)
                                             .count();
                    active_ms.store(elapsed);
                    log_line("{\"event\":\"active\",\"startup_ms\":" + std::to_string(elapsed) +
                             "}");
                }
            }
        }
    });

    std::thread input_thread([&] {
        while (g_running.load()) {
            auto endpoint = input_server.value().accept_sender(Milliseconds{200});
            if (!endpoint) {
                continue;
            }
            log_line("{\"event\":\"input_accepted\"}");
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

    const auto deadline = duration_ms > 0 ? started + std::chrono::milliseconds{duration_ms}
                                          : std::chrono::steady_clock::time_point::max();
    while (g_running.load() && std::chrono::steady_clock::now() < deadline) {
        if (bus.has_value()) {
            auto step = bus.value().cycle(Milliseconds{200});
            if (!step.has_value()) {
                break;
            }
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds{200});
        }
        auto producer = video.value().accept_consumer(Milliseconds{0});
        if (producer.has_value()) {
            log_line("{\"event\":\"video_accepted\"}");
            video_outlet.install(std::move(producer.value()));
        }
    }

    g_running.store(false);
    session.request_stop();
    session_thread.join();
    input_thread.join();

    {
        std::lock_guard lock(session_mutex);
        (void)session.apply(aa::session::Event::cleanup_finished);
    }
    video_outlet.close();
    router.value().close();
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
             ",\"version_responses\":" + std::to_string(session.version_responses()) + "}");
    return 0;
}
