// SPDX-License-Identifier: GPL-3.0-or-later
#include "../../src/audio/AudioSinkFactory.hpp"
#include "../../src/helper/crypto_adapter/Crypto.hpp"
#include "../../src/transport/tls_adapter/TlsRecordCodec.hpp"
#include "../../tools/wireless-receiver/Runtime.hpp"
#include "../session/Fakes.hpp"
#include "../video_socket/pair.hpp"
#include "../wired_e2e/harness.hpp"

#include <aa/app/AvAudioRegistration.hpp>
#include <aa/app/WirelessReceiver.hpp>
#include <aa/audio/AudioRenderWorker.hpp>
#include <aa/audio/AudioRouter.hpp>
#include <aa/audio/AudioTelemetry.hpp>
#include <aa/diagnostics/StdoutLog.hpp>
#include <aa/helper/BusClient.hpp>
#include <aa/helper/BusService.hpp>
#include <aa/ipc/ControlBusClient.hpp>
#include <aa/ipc/ControlBusService.hpp>
#include <aa/ipc/InputSocket.hpp>
#include <aa/ipc/VideoSocket.hpp>
#include <aa/protocol/Messages.hpp>
#include <aa/protocol/WifiProjection.hpp>
#include <aa/session/Clock.hpp>
#include <aa/transport/Framing.hpp>
#include <aa/transport/TcpTransport.hpp>
#include <aa/trust/PairingPolicy.hpp>
#include <aa/trust/Store.hpp>

#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

namespace app = aa::app;
namespace audio = aa::audio;
namespace diag = aa::diagnostics;
namespace helper = aa::helper;
namespace ipc = aa::ipc;
namespace proto = aa::protocol;
namespace ss = aa::session;
namespace tr = aa::transport;
namespace wr = aa::wireless_receiver;

using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

std::vector<std::byte> av_data_msg(std::uint64_t peer_timestamp,
                                   const std::vector<std::byte>& media) {
    std::vector<std::byte> payload{std::byte{0}, std::byte{0}};
    for (int shift = 56; shift >= 0; shift -= 8) {
        payload.push_back(static_cast<std::byte>((peer_timestamp >> shift) & 0xffU));
    }
    payload.insert(payload.end(), media.begin(), media.end());
    return payload;
}

std::vector<std::byte> pcm_50ms() {
    return std::vector<std::byte>(9600, std::byte{0x40});
}

void maybe_dump_jsonl(const std::string& tag, const std::string& text) {
    const char* path = std::getenv("AA_TEST_JSONL_OUT");
    if (!path || !*path) return;
    std::ofstream out(std::string{path} + "." + tag, std::ios::app);
    out << text;
}

struct SinkGate final {
    std::mutex mutex;
    std::condition_variable cv;
    bool block_first_write{false};
    bool released{false};
    bool fail_first_write{false};
    bool populate_playback_slot{false};
    std::size_t writes_entered{0};
    std::size_t writes_completed{0};
    std::size_t sinks_opened{0};

    void release() {
        std::lock_guard lock(mutex);
        released = true;
        cv.notify_all();
    }

    bool wait_entered(std::size_t count,
                      std::chrono::milliseconds timeout = std::chrono::seconds{5}) {
        std::unique_lock lock(mutex);
        return cv.wait_for(lock, timeout, [&] { return writes_entered >= count; });
    }
};

class GatedSink final : public audio::detail::ObservedAudioSink {
public:
    explicit GatedSink(std::shared_ptr<SinkGate> gate) : gate_(std::move(gate)) {}

    aa::core::Result<void> write(const audio::MixedChunk&) override {
        std::unique_lock lock(gate_->mutex);
        const std::size_t index = ++gate_->writes_entered;
        gate_->cv.notify_all();
        if (index == 1 && gate_->block_first_write) {
            if (!gate_->cv.wait_for(lock, std::chrono::seconds{5},
                                    [&] { return gate_->released; })) {
                return aa::Error{aa::ErrorCode::timeout};
            }
        }
        if (index == 1 && gate_->fail_first_write) {
            return aa::Error{aa::ErrorCode::timeout};
        }
        ++gate_->writes_completed;
        if (gate_->populate_playback_slot) {
            last_slot_ = aa::core::Nanoseconds{
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    Clock::now().time_since_epoch())
                    .count() +
                5'000'000LL};
        }
        gate_->cv.notify_all();
        return {};
    }

    aa::core::Result<void> drain(aa::core::Milliseconds) override { return {}; }
    [[nodiscard]] audio::SinkProbeStats probe_stats() const override { return {}; }
    [[nodiscard]] audio::RenderBarrierStats render_stats() const override { return {}; }
    [[nodiscard]] std::optional<aa::core::Nanoseconds> last_playback_slot() const override {
        std::lock_guard lock(gate_->mutex);
        return last_slot_;
    }

private:
    std::shared_ptr<SinkGate> gate_;
    std::optional<aa::core::Nanoseconds> last_slot_{};
};

audio::AudioRenderWorker::OpenRouter make_gated_router(const std::shared_ptr<SinkGate>& gate) {
    return [gate](const audio::RouterConfig& config) {
        {
            std::lock_guard lock(gate->mutex);
            ++gate->sinks_opened;
            gate->cv.notify_all();
        }
        return audio::detail::open_router_with_sink_for_test(
            config, std::make_unique<GatedSink>(gate));
    };
}

struct StdoutCapture final {
    bool active{false};
    StdoutCapture() {
        testing::internal::CaptureStdout();
        active = true;
    }
    ~StdoutCapture() {
        if (active) {
            (void)testing::internal::GetCapturedStdout();
        }
    }
    std::string stop() {
        active = false;
        return testing::internal::GetCapturedStdout();
    }
    StdoutCapture(const StdoutCapture&) = delete;
    StdoutCapture& operator=(const StdoutCapture&) = delete;
};

struct TempDir final {
    std::filesystem::path path;
    TempDir() {
        char buf[] = "/tmp/aa-wr-XXXXXX";
        char* created = ::mkdtemp(buf);
        EXPECT_NE(created, nullptr);
        if (created) path = created;
    }
    ~TempDir() {
        if (!path.empty()) {
            std::error_code ec;
            std::filesystem::remove_all(path, ec);
        }
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
};

class FakeHelperBackend final : public helper::Backend {
public:
    explicit FakeHelperBackend(std::uint16_t port) : port_(port) {}

    aa::core::Result<helper::State> apply(helper::Action, std::uint32_t) override {
        return helper::State::active;
    }
    void cleanup() noexcept override {}
    aa::core::Result<helper::ObservedPeer> observe() override { return handoff(); }
    aa::core::Result<helper::State> reconnect_peer(std::uint64_t) override {
        std::lock_guard lock(mutex_);
        ++handle_;
        ++reconnect_calls_;
        listening_ready_ = false;
        return helper::State::active;
    }
    aa::core::Result<helper::ObservedPeer> handoff() override {
        std::lock_guard lock(mutex_);
        return helper::ObservedPeer{
            handle_, {"12:34:56:78:9A:BC", "12:34:56:78:9A:BD"}};
    }
    aa::core::Result<void> authorize_handoff(std::uint64_t handle, bool allow, bool ready) override {
        std::lock_guard lock(mutex_);
        if (allow && ready && handle == handle_) {
            listening_ready_ = true;
            ++authorized_count_;
            cv_.notify_all();
        } else if (!allow) {
            listening_ready_ = false;
        }
        return {};
    }
    aa::core::Result<helper::ApSnapshot> ap_snapshot() override {
        return helper::ApSnapshot{
            {"fixture", "fixture-pass", "12:34:56:78:9a:bd", "127.0.0.1", port_, 1}};
    }
    bool snapshot_current(std::uint64_t epoch) override { return epoch == 1; }
    bool with_snapshot(std::uint64_t epoch, const std::function<void()>& send) override {
        if (epoch != 1) return false;
        send();
        return true;
    }

    bool wait_authorized(std::size_t min_count,
                         std::chrono::milliseconds timeout = std::chrono::seconds{5}) {
        std::unique_lock lock(mutex_);
        return cv_.wait_for(lock, timeout, [&] {
            return listening_ready_ && authorized_count_ >= min_count;
        });
    }

    std::size_t reconnect_calls() const {
        std::lock_guard lock(mutex_);
        return reconnect_calls_;
    }

private:
    std::uint16_t port_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::uint64_t handle_{100};
    std::size_t authorized_count_{0};
    std::size_t reconnect_calls_{0};
    bool listening_ready_{false};
};

struct PhonePeer final {
    using Codec = tr::tls_adapter::TlsRecordCodec;

    std::unique_ptr<tr::TcpTransport> tcp;
    Codec cipher{
        aa::tls::Policy{[] { return true; }, aa::tls::Mode::encryption_only_compatibility},
        Codec::Role::server};
    tr::MessageReassembler reassembler{&cipher, tr::PlaintextAfterHandshake::allow};

    static aa::core::Result<std::unique_ptr<PhonePeer>> connect(std::uint16_t port) {
        auto transport = tr::TcpTransport::connect("127.0.0.1", port);
        if (!transport) return transport.error();
        auto peer = std::make_unique<PhonePeer>();
        peer->tcp = std::move(transport).value();
        peer->tcp->set_io_deadline(Clock::now() + 5s);
        return peer;
    }

    aa::core::Result<void> send_msg(std::uint8_t channel, std::vector<std::byte> payload,
                                    bool encrypted = true) {
        auto frames = tr::encode_message(
            {channel, tr::MessageKind::specific, std::move(payload)},
            encrypted ? tr::Encryption::encrypted : tr::Encryption::plain,
            encrypted ? &cipher : nullptr);
        if (!frames) return frames.error();
        for (const auto& frame : frames.value()) {
            auto sent = tcp->send(frame);
            if (!sent) return sent.error();
        }
        return {};
    }

    aa::core::Result<tr::Message> recv_msg(std::chrono::milliseconds timeout = 5s) {
        tcp->set_io_deadline(Clock::now() + timeout);
        for (;;) {
            auto frame = tcp->receive();
            if (!frame) return frame.error();
            auto msg = reassembler.push(frame.value());
            if (!msg) return msg.error();
            if (msg.value()) return std::move(*msg.value());
        }
    }

    void handshake_and_open_av(std::int32_t audio_session_id = -7) {
        tr::MessageReassembler plain_reassembler{nullptr, tr::PlaintextAfterHandshake::allow};
        auto recv_plain = [&]() -> aa::core::Result<tr::Message> {
            tcp->set_io_deadline(Clock::now() + 5s);
            for (;;) {
                auto frame = tcp->receive();
                if (!frame) return frame.error();
                auto msg = plain_reassembler.push(frame.value());
                if (!msg) return msg.error();
                if (msg.value()) return std::move(*msg.value());
            }
        };

        auto version = recv_plain();
        ASSERT_TRUE(version);
        ASSERT_TRUE(proto::decode_version_request(version.value().payload));
        ASSERT_TRUE(send_msg(0, proto::encode(proto::VersionResponse{1, 6, 0}).value(), false));

        bool authenticated = false;
        for (int round = 0; round < 16 && !authenticated; ++round) {
            auto msg = recv_plain();
            ASSERT_TRUE(msg);
            if (auto auth = proto::decode_auth_complete(msg.value().payload); auth) {
                ASSERT_TRUE(cipher.is_active());
                authenticated = true;
            } else {
                auto ssl = proto::decode_encapsulated_ssl(msg.value().payload);
                ASSERT_TRUE(ssl);
                cipher.give_handshake(ssl.value().bytes);
                ASSERT_TRUE(cipher.step_handshake());
                auto output = cipher.take_handshake();
                if (!output.empty()) {
                    ASSERT_TRUE(
                        send_msg(0, proto::encode(proto::EncapsulatedSsl{std::move(output)}).value(),
                                 false));
                }
            }
        }
        ASSERT_TRUE(authenticated);

        ASSERT_TRUE(send_msg(0, {std::byte{0x00}, std::byte{0x05}}));
        auto discovery = recv_msg();
        ASSERT_TRUE(discovery);
        auto services = proto::decode_service_discovery_response(discovery.value().payload);
        ASSERT_TRUE(services);
        ASSERT_GE(services.value().services.size(), 4U);

        // Open wifi_projection (73) and request credentials so bootstrap completes.
        ASSERT_TRUE(
            send_msg(0, proto::encode(proto::ChannelOpenRequest{0, proto::ServiceKey{73}}).value()));
        auto open_wifi = recv_msg();
        ASSERT_TRUE(open_wifi);
        ASSERT_TRUE(send_msg(73, {std::byte{0x80}, std::byte{0x01}}));
        auto creds = recv_msg();
        ASSERT_TRUE(creds);
        EXPECT_EQ(creds.value().channel, 73U);

        // Open video (99) and media_audio (100).
        ASSERT_TRUE(
            send_msg(0, proto::encode(proto::ChannelOpenRequest{0, proto::ServiceKey{99}}).value()));
        ASSERT_TRUE(recv_msg());
        ASSERT_TRUE(
            send_msg(0, proto::encode(proto::ChannelOpenRequest{0, proto::ServiceKey{100}}).value()));
        ASSERT_TRUE(recv_msg());

        // Ping once so WirelessLink promotes session from negotiating -> active.
        ASSERT_TRUE(send_msg(0, proto::encode(proto::PingRequest{1, false, {}}).value()));
        auto ping = recv_msg();
        ASSERT_TRUE(ping);

        // Setup & start video (99) + media_audio (100), draining setup replies.
        ASSERT_TRUE(send_msg(99, proto::encode(proto::AvSinkSetup{proto::AvSinkCodec::h264}).value()));
        ASSERT_TRUE(recv_msg()); // AvSinkReady
        ASSERT_TRUE(recv_msg()); // VideoFocusNotification
        ASSERT_TRUE(send_msg(99, proto::encode(proto::AvSinkStart{1, 0}).value()));

        ASSERT_TRUE(send_msg(100, proto::encode(proto::AvSinkSetup{}).value()));
        ASSERT_TRUE(recv_msg()); // AvSinkReady
        ASSERT_TRUE(send_msg(100, proto::encode(proto::AvSinkStart{audio_session_id, 0}).value()));
    }

    aa::core::Result<void> wait_audio_ack(std::int32_t session_id,
                                          std::chrono::milliseconds timeout = 5s) {
        const auto expected = proto::encode_av_sink_ack(session_id).value();
        const auto deadline = Clock::now() + timeout;
        while (Clock::now() < deadline) {
            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - Clock::now());
            auto msg = recv_msg(std::max(1ms, remaining));
            if (!msg) return msg.error();
            if (msg.value().channel == 100U && msg.value().payload == expected) {
                return {};
            }
        }
        return aa::Error{aa::ErrorCode::timeout};
    }

    void close() {
        if (tcp) tcp->close();
    }
};

struct RuntimeFixture final {
    TempDir dir;
    std::uint16_t port{0};
    aa::trust::ApprovedPhoneStore store{dir.path / "approved-phones.json"};
    aa::trust::TransportIdentity identity{
        aa::trust::WirelessIdentity{"12:34:56:78:9A:BC", "12:34:56:78:9A:BD"}};
    std::unique_ptr<FakeHelperBackend> backend;
    std::optional<helper::BusService> helper_bus;
    std::jthread helper_pump;
    std::unique_ptr<helper::BusClient> helper_client;
    std::shared_ptr<SinkGate> gate{std::make_shared<SinkGate>()};
    std::unique_ptr<audio::AudioRenderWorker> audio_worker;
    std::unique_ptr<wr::DiagnosticOutput> diag_out;
    std::unique_ptr<aa::diagnostics::Diagnostics> diagnostics;
    ss::SteadyClock clock;
    aa::trust::SteadyClock trust_clock;
    std::unique_ptr<aa::trust::PairingPolicy> phones;
    ipc::RandomPairingIdSource ids;
    std::unique_ptr<wr::Media> media;
    std::unique_ptr<app::WirelessReceiver> receiver;
    std::optional<ipc::ControlBusService> bus;
    std::optional<ipc::VideoServer> video_server;
    std::optional<ipc::InputServer> input_server;
    std::optional<ipc::ControlBusClient> consumer_client;
    std::unique_ptr<wr::Runtime> runtime;
    std::thread runtime_thread;
    std::atomic<int> runtime_exit{-1};
    std::mutex state_mutex;
    std::condition_variable state_cv;
    bool session_live{false};

    void start(Clock::time_point deadline = Clock::now() + 20s) {
        wr::running.store(true);

        auto reservation = tr::TcpListener::listen("127.0.0.1", 0);
        ASSERT_TRUE(reservation);
        port = reservation.value()->port();
        ASSERT_GT(port, 1024U);
        reservation.value()->close();

        ASSERT_TRUE(store.load());
        ASSERT_TRUE(store.approve(identity));

        const int exe_fd = ::open("/proc/self/exe", O_RDONLY | O_CLOEXEC);
        ASSERT_GE(exe_fd, 0);
        const auto digest = helper::crypto_adapter::hash_executable(
            exe_fd, Clock::now() + 2s);
        ::close(exe_fd);
        ASSERT_TRUE(digest);

        const char* dbus_addr = std::getenv("DBUS_SESSION_BUS_ADDRESS");
        ASSERT_NE(dbus_addr, nullptr);
        backend = std::make_unique<FakeHelperBackend>(port);
        auto helper_bus_res = helper::BusService::private_bus(
            dbus_addr, {::getuid(), digest.value()}, *backend);
        ASSERT_TRUE(helper_bus_res);
        helper_bus.emplace(std::move(helper_bus_res).value());
        helper_pump = std::jthread([this](const std::stop_token& stop) {
            while (!stop.stop_requested()) {
                if (!helper_bus->cycle()) break;
            }
        });

        auto client_res = helper::BusClient::private_bus_for_test(dbus_addr, store);
        ASSERT_TRUE(client_res);
        helper_client = std::move(client_res).value();

        audio_worker = std::make_unique<audio::AudioRenderWorker>(
            wr::audio_config(audio::SinkBackend::pulse), wr::audio_telemetry(),
            make_gated_router(gate));
        ASSERT_TRUE(audio_worker->ready());

        diag_out = std::make_unique<wr::DiagnosticOutput>(true);
        diagnostics = std::make_unique<aa::diagnostics::Diagnostics>(*diag_out);
        phones = std::make_unique<aa::trust::PairingPolicy>(store, trust_clock);
        media = std::make_unique<wr::Media>(*audio_worker);
        media->send = [this](proto::ChannelRole role, std::vector<std::byte> payload) {
            auto* session = receiver->session();
            return session ? session->send_channel_payload(role, std::move(payload))
                           : aa::core::Result<void>{aa::Error{aa::ErrorCode::transport_closed}};
        };

        auto link_cfg = media->config();
        receiver = std::make_unique<app::WirelessReceiver>(
            app::WiredSessionDeps{store, clock, *diagnostics}, *helper_client,
            std::move(link_cfg));

        auto bus_res = ipc::ControlBusService::start(
            {ipc::current_uid()}, {ids, *phones, &receiver->confirmation()});
        ASSERT_TRUE(bus_res);
        bus = std::move(bus_res).value();

        const auto vsock = (dir.path / "v.sock").string();
        const auto isock = (dir.path / "i.sock").string();
        auto v_res = ipc::VideoServer::listen(
            ipc::VideoSocketPath{vsock}, ipc::VideoServerOptions{{}, ipc::current_uid()});
        ASSERT_TRUE(v_res);
        video_server = std::move(v_res).value();

        auto i_res = ipc::InputServer::listen(
            ipc::InputSocketPath{isock}, ipc::InputServerOptions{ipc::current_uid()});
        ASSERT_TRUE(i_res);
        input_server = std::move(i_res).value();

        // Pump ControlBusService briefly so ControlBusClient can register & start projection.
        std::jthread bus_setup_pump([this](const std::stop_token& stop) {
            while (!stop.stop_requested()) {
                (void)bus->cycle(aa::core::Milliseconds{5});
            }
        });
        auto c_res = ipc::ControlBusClient::connect();
        ASSERT_TRUE(c_res);
        consumer_client = std::move(c_res).value();
        ASSERT_TRUE(consumer_client->register_consumer(ipc::BusName{"org.custom.TestConsumer"}));
        ASSERT_TRUE(consumer_client->start_projection());
        bus_setup_pump.request_stop();
        bus_setup_pump.join();

        runtime = std::make_unique<wr::Runtime>(
            *receiver, *helper_client, *bus, *video_server, *input_server,
            *media, identity, deadline, std::nullopt, std::deque<ipc::InputEvent>{},
            std::nullopt);
        runtime->offered_video_profiles = 1;
        runtime->on_turn_for_test = [this](bool live) {
            std::lock_guard lock(state_mutex);
            session_live = live;
            state_cv.notify_all();
        };
        runtime_thread = std::thread([this] { runtime_exit.store(runtime->run()); });
    }

    int join_runtime() {
        if (runtime_thread.joinable()) runtime_thread.join();
        return runtime_exit.load();
    }

    int stop_and_join(std::chrono::milliseconds drain = 2s) {
        {
            std::unique_lock lock(state_mutex);
            (void)state_cv.wait_for(lock, drain, [&] { return !session_live; });
        }
        wr::running.store(false);
        return join_runtime();
    }

    ~RuntimeFixture() {
        wr::running.store(false);
        if (gate) gate->release();
        if (runtime_thread.joinable()) runtime_thread.join();
        consumer_client.reset();
        if (bus) bus->close();
        receiver.reset();
        helper_client.reset();
        helper_pump.request_stop();
        helper_pump.join();
        helper_bus.reset();
    }
};

class SyncRouterAudioSink final : public proto::MessageSink {
public:
    explicit SyncRouterAudioSink(audio::AudioRouter& router) : router_(router) {}
    aa::core::Result<void> on_message(const proto::MessageView& message) override {
        if (auto submitted = router_.submit(message); !submitted) return submitted.error();
        return router_.pump();
    }
private:
    audio::AudioRouter& router_;
};

TEST(WirelessAudioRuntime, QuietTcpLinkDefersAudioCompletionAtFiveMsCadence) {
    StdoutCapture capture;
    std::string output;
    int exit_code = -1;
    std::chrono::milliseconds ack_latency{9999};
    {
        RuntimeFixture f;
        f.gate->block_first_write = true;
        f.gate->populate_playback_slot = true;
        ASSERT_NO_FATAL_FAILURE(f.start(Clock::now() + 15s));
        ASSERT_TRUE(f.backend->wait_authorized(1));

        auto phone_res = PhonePeer::connect(f.port);
        ASSERT_TRUE(phone_res);
        auto phone = std::move(phone_res).value();
        ASSERT_NO_FATAL_FAILURE(phone->handshake_and_open_av(-7));

        ASSERT_TRUE(phone->send_msg(100, av_data_msg(1'000'000'000ULL, pcm_50ms())));
        ASSERT_TRUE(phone->wait_audio_ack(-7));
        ASSERT_TRUE(f.gate->wait_entered(1));

        ASSERT_TRUE(phone->send_msg(100, av_data_msg(1'050'000'000ULL, pcm_50ms())));
        ASSERT_TRUE(phone->wait_audio_ack(-7));

        // Chunk 3 exceeds the 19200-byte per-slot envelope so credit is deferred.
        ASSERT_TRUE(phone->send_msg(100, av_data_msg(1'100'000'000ULL, pcm_50ms())));
        {
            std::unique_lock lock(f.gate->mutex);
            (void)f.gate->cv.wait_for(lock, 40ms, [] { return false; });
        }

        // TCP link is now completely idle while audio.outstanding() == true.
        // Release write 1 and verify ACK 3 completes on the 5 ms TcpIo cadence (< 15 ms).
        const auto t0 = Clock::now();
        f.gate->release();
        ASSERT_TRUE(phone->wait_audio_ack(-7, 2s));
        ack_latency = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0);

        ASSERT_TRUE(f.audio_worker->wait_idle(Clock::now() + 2s));
        {
            std::unique_lock lock(f.gate->mutex);
            ASSERT_TRUE(
                f.gate->cv.wait_for(lock, 2s, [&] { return f.gate->writes_completed >= 3U; }));
        }

        EXPECT_TRUE(f.consumer_client->stop_projection());
        phone->close();
        exit_code = f.stop_and_join();
    }
    output = capture.stop();
    maybe_dump_jsonl("quiet_5ms", output);

    EXPECT_EQ(exit_code, 0) << output;
    EXPECT_LT(ack_latency.count(), 15) << "ack_latency_ms=" << ack_latency.count();
    EXPECT_NE(output.find("\"event\":\"audio_write_completed\""), std::string::npos) << output;
    EXPECT_NE(output.find("\"event\":\"media_timing\""), std::string::npos) << output;
    EXPECT_EQ(output.find("\"event\":\"err\""), std::string::npos) << output;
}

TEST(WirelessAudioRuntime, ReconnectWithInFlightSessionOneSinkWriteIsolatesEpochTwo) {
    StdoutCapture capture;
    std::string output;
    int exit_code = -1;
    std::size_t entered = 0;
    std::size_t completed = 0;
    std::size_t opened = 0;
    {
        RuntimeFixture f;
        f.gate->block_first_write = true;
        f.gate->fail_first_write = true;
        f.gate->populate_playback_slot = true;
        ASSERT_NO_FATAL_FAILURE(f.start(Clock::now() + 20s));
        ASSERT_TRUE(f.backend->wait_authorized(1));

        // Session 1 sends timestamp 2.0s which blocks inside GatedSink::write().
        {
            auto p1_res = PhonePeer::connect(f.port);
            ASSERT_TRUE(p1_res);
            auto p1 = std::move(p1_res).value();
            ASSERT_NO_FATAL_FAILURE(p1->handshake_and_open_av(-7));
            ASSERT_TRUE(p1->send_msg(100, av_data_msg(2'000'000'000ULL, pcm_50ms())));
            ASSERT_TRUE(p1->wait_audio_ack(-7));
            ASSERT_TRUE(f.gate->wait_entered(1));
            p1->close();
        }

        // Wait for Runtime to retire Session 1 and re-open listener for Session 2.
        ASSERT_TRUE(f.backend->wait_authorized(2));
        auto p2_res = PhonePeer::connect(f.port);
        ASSERT_TRUE(p2_res);
        auto p2 = std::move(p2_res).value();
        // Reuse the same AV session ID (-7) across reconnect.
        ASSERT_NO_FATAL_FAILURE(p2->handshake_and_open_av(-7));

        // Send an EARLIER timestamp (1.0s < 2.0s) in Session 2 and wait until Session 2
        // has actually admitted PCM (ACK -7 delivered) while Session 1's failing write is
        // still held in GatedSink::write(), then release write 1 to fail and trigger reopen.
        ASSERT_TRUE(p2->send_msg(100, av_data_msg(1'000'000'000ULL, pcm_50ms())));
        ASSERT_TRUE(p2->wait_audio_ack(-7, 5s));
        f.gate->release();
        ASSERT_TRUE(f.audio_worker->wait_idle(Clock::now() + 2s));
        {
            std::unique_lock lock(f.gate->mutex);
            ASSERT_TRUE(
                f.gate->cv.wait_for(lock, 2s, [&] { return f.gate->writes_completed >= 1U; }));
            entered = f.gate->writes_entered;
            completed = f.gate->writes_completed;
            opened = f.gate->sinks_opened;
        }
        EXPECT_FALSE(f.audio_worker->failure().has_value());
        EXPECT_TRUE(f.consumer_client->stop_projection());
        p2->close();
        exit_code = f.stop_and_join();
    }
    output = capture.stop();
    maybe_dump_jsonl("reconnect_epoch", output);

    EXPECT_EQ(exit_code, 0) << output;
    EXPECT_GE(opened, 2U);
    EXPECT_EQ(entered, 2U);
    EXPECT_EQ(completed, 1U);
    EXPECT_EQ(output.find("\"event\":\"err\""), std::string::npos) << output;
}

TEST(WirelessAudioRuntime, AckSendDropRetiresRecoverablyWhileOwnerCancelCleanAndWorkerFailureFails) {
    // 1. Direct ACK send() failures (both inside poll() checkpoint and outside poll()) with
    //    live peer connections retire recoverably via defer(), while owner cancellation with a
    //    live pending ACK stops cleanly with exit code 0.
    StdoutCapture capture;
    std::string output;
    int exit_code = -1;
    std::size_t reconnects = 0;
    std::atomic<bool> saw_in_poll_inject{false};
    std::atomic<bool> saw_out_poll_inject{false};
    std::atomic<bool> saw_live_cancel_inject{false};
    {
        RuntimeFixture f;
        f.gate->block_first_write = true;
        ASSERT_NO_FATAL_FAILURE(f.start(Clock::now() + 20s));
        std::atomic<int> inject_phase{0};
        f.runtime->on_before_audio_completions_for_test = [&](app::WiredSession& session,
                                                             bool in_poll) {
            const int p = inject_phase.load();
            if (p == 1 && in_poll) {
                int expected = 1;
                if (inject_phase.compare_exchange_strong(expected, 0)) {
                    f.gate->release();
                    if (f.audio_worker->wait_idle(Clock::now() + 2s)) {
                        saw_in_poll_inject.store(true);
                        session.fail_next_send_for_test(aa::Error{aa::ErrorCode::transport_io});
                    }
                }
            } else if (p == 2 && !in_poll && f.audio_worker->outstanding()) {
                int expected = 2;
                if (inject_phase.compare_exchange_strong(expected, 0)) {
                    saw_out_poll_inject.store(true);
                    session.fail_next_send_for_test(aa::Error{aa::ErrorCode::transport_io});
                }
            } else if (p == 3 && f.audio_worker->outstanding()) {
                int expected = 3;
                if (inject_phase.compare_exchange_strong(expected, 0)) {
                    saw_live_cancel_inject.store(true);
                    wr::running.store(false);
                    session.request_stop();
                }
            }
        };
        ASSERT_TRUE(f.backend->wait_authorized(1));

        // Phase A (in_poll == true): p1 stays connected while chunk 3's deferred ACK send() fails.
        auto p1_res = PhonePeer::connect(f.port);
        ASSERT_TRUE(p1_res);
        auto p1 = std::move(p1_res).value();
        ASSERT_NO_FATAL_FAILURE(p1->handshake_and_open_av(-7));
        ASSERT_TRUE(p1->send_msg(100, av_data_msg(1'000'000'000ULL, pcm_50ms())));
        ASSERT_TRUE(p1->wait_audio_ack(-7));
        ASSERT_TRUE(f.gate->wait_entered(1));
        ASSERT_TRUE(p1->send_msg(100, av_data_msg(1'050'000'000ULL, pcm_50ms())));
        ASSERT_TRUE(p1->wait_audio_ack(-7));
        ASSERT_TRUE(p1->send_msg(100, av_data_msg(1'100'000'000ULL, pcm_50ms())));
        {
            std::unique_lock lock(f.gate->mutex);
            (void)f.gate->cv.wait_for(lock, 30ms, [] { return false; });
        }
        inject_phase.store(1);
        ASSERT_TRUE(f.backend->wait_authorized(2));
        p1->close();

        // Phase B (in_poll == false): p2 stays connected while immediate ACK send() fails outside poll().
        auto p2_res = PhonePeer::connect(f.port);
        ASSERT_TRUE(p2_res);
        auto p2 = std::move(p2_res).value();
        ASSERT_NO_FATAL_FAILURE(p2->handshake_and_open_av(-8));
        inject_phase.store(2);
        ASSERT_TRUE(p2->send_msg(100, av_data_msg(1'200'000'000ULL, pcm_50ms())));
        ASSERT_TRUE(f.backend->wait_authorized(3));
        p2->close();

        // Phase C (live pending ACK + owner cancellation): p3 stays connected, pending ACK hits
        // ErrorCode::cancelled with running == false -> clean exit 0.
        auto p3_res = PhonePeer::connect(f.port);
        ASSERT_TRUE(p3_res);
        auto p3 = std::move(p3_res).value();
        ASSERT_NO_FATAL_FAILURE(p3->handshake_and_open_av(-9));
        reconnects = f.backend->reconnect_calls();
        inject_phase.store(3);
        ASSERT_TRUE(p3->send_msg(100, av_data_msg(1'300'000'000ULL, pcm_50ms())));
        exit_code = f.join_runtime();
        p3->close();
    }
    output = capture.stop();
    EXPECT_TRUE(saw_in_poll_inject.load());
    EXPECT_TRUE(saw_out_poll_inject.load());
    EXPECT_TRUE(saw_live_cancel_inject.load());
    EXPECT_EQ(exit_code, 0) << output;
    EXPECT_GE(reconnects, 3U);
    EXPECT_EQ(output.find("audio_sink_failure"), std::string::npos) << output;

    // 2. Direct Runtime branches: clean owner/deadline cancellation (exit 0 / not failed)
    //    versus latched worker failure (audio_sink_failure JSON + failed = true).
    TempDir unit_dir;
    aa::trust::ApprovedPhoneStore trust{unit_dir.path / "approved-phones.json"};
    wiredtest::CaptureSink dcapture;
    aa::diagnostics::Diagnostics diags{dcapture};
    ASSERT_TRUE(trust.load());
    const aa::trust::TransportIdentity wid{
        aa::trust::WirelessIdentity{"12:34:56:78:9A:BC", "12:34:56:78:9A:BD"}};
    ASSERT_TRUE(trust.approve(wid));

    const char* dbus_addr = std::getenv("DBUS_SESSION_BUS_ADDRESS");
    ASSERT_NE(dbus_addr, nullptr);
    auto helper = helper::BusClient::private_bus_for_test(dbus_addr, trust);
    ASSERT_TRUE(helper);

    ss::SteadyClock sclock;
    aa::trust::SteadyClock tclock;
    aa::trust::PairingPolicy phones{trust, tclock};
    ipc::RandomPairingIdSource ids;

    auto gate = std::make_shared<SinkGate>();
    audio::AudioRenderWorker worker{
        wr::audio_config(audio::SinkBackend::pulse), wr::audio_telemetry(),
        make_gated_router(gate)};
    ASSERT_TRUE(worker.ready());
    wr::Media media{worker};
    app::WirelessReceiver wrx{{trust, sclock, diags}, *helper.value(), media.config()};
    auto bus = ipc::ControlBusService::start({ipc::current_uid()},
                                             {ids, phones, &wrx.confirmation()});
    ASSERT_TRUE(bus);
    auto vserv = ipc::VideoServer::listen(
        ipc::VideoSocketPath{(unit_dir.path / "v.sock").string()},
        ipc::VideoServerOptions{{}, ipc::current_uid()});
    ASSERT_TRUE(vserv);
    auto iserv = ipc::InputServer::listen(
        ipc::InputSocketPath{(unit_dir.path / "i.sock").string()},
        ipc::InputServerOptions{ipc::current_uid()});
    ASSERT_TRUE(iserv);

    // Expired deadline stops cleanly with exit code 0 (failed == false).
    wr::running.store(true);
    {
        StdoutCapture c;
        wr::Runtime rt{wrx, *helper.value(), bus.value(), vserv.value(), iserv.value(),
                       media, wid, Clock::now() - 1ms, std::nullopt, {}, std::nullopt};
        EXPECT_EQ(rt.run(), 0);
        EXPECT_FALSE(rt.failed);
        EXPECT_FALSE(wr::running.load());
        const std::string clean_log = c.stop();
        EXPECT_NE(clean_log.find("\"event\":\"stopping\""), std::string::npos) << clean_log;
    }

    // Latched AudioRenderWorker failure sets failed = true and logs audio_sink_failure JSON.
    wr::running.store(true);
    {
        StdoutCapture c;
        audio::AudioRenderWorker bad_worker{
            wr::audio_config(audio::SinkBackend::pulse), wr::audio_telemetry(),
            make_gated_router(std::make_shared<SinkGate>())};
        ASSERT_TRUE(bad_worker.ready());
        wr::Media bad_media{bad_worker};
        wr::Runtime rt{wrx, *helper.value(), bus.value(), vserv.value(), iserv.value(),
                       bad_media, wid, Clock::now() + 10s, std::nullopt, {}, std::nullopt};
        const auto role = proto::ChannelRole::media_audio;
        bad_worker.invalidate(role, 55);
        ASSERT_TRUE(bad_worker.submit({{role, 55, {-1LL}}, wiredtest::pcm_unit()}, {role, 55, 55, 1}));
        ASSERT_TRUE(bad_worker.wait_idle(Clock::now() + 2s));
        ASSERT_TRUE(bad_worker.failure());
        EXPECT_TRUE(rt.service_audio_failure());
        EXPECT_TRUE(rt.failed);
        EXPECT_TRUE(rt.stopping);
        EXPECT_EQ(rt.run(), 1);
        const std::string err_log = c.stop();
        EXPECT_NE(err_log.find("\"detail\":\"audio_sink_failure\""), std::string::npos) << err_log;
        bad_worker.stop();
        (void)bad_worker.finish();
    }

    worker.stop();
    (void)worker.finish();
    bus.value().close();
}

TEST(WirelessAudioRuntime, StdoutBackpressureDuringSinkReopenAndBoundedTeardown) {
    wr::running.store(true);
    int pfd[2]{-1, -1};
    ASSERT_EQ(::pipe(pfd), 0);
    const int saved_stdout = ::dup(STDOUT_FILENO);
    ASSERT_GE(saved_stdout, 0);

    // Fill kernel pipe buffer before StdoutLog starts so its writer thread blocks in ::write.
    {
        const int flags = ::fcntl(pfd[1], F_GETFL);
        ASSERT_GE(flags, 0);
        ASSERT_EQ(::fcntl(pfd[1], F_SETFL, flags | O_NONBLOCK), 0);
        std::vector<char> fill(4096, 'x');
        while (::write(pfd[1], fill.data(), fill.size()) > 0) {}
        ASSERT_EQ(::fcntl(pfd[1], F_SETFL, flags), 0);
    }

    ASSERT_EQ(::dup2(pfd[1], STDOUT_FILENO), STDOUT_FILENO);
    ::close(pfd[1]);
    pfd[1] = -1;

    auto restore_fd = [&] {
        // Close read end first so any abandoned writer sees EPIPE and exits immediately.
        if (pfd[0] >= 0) {
            ::close(pfd[0]);
            pfd[0] = -1;
        }
        std::this_thread::sleep_for(20ms);
        ::dup2(saved_stdout, STDOUT_FILENO);
        ::close(saved_stdout);
    };

    auto gate = std::make_shared<SinkGate>();
    gate->block_first_write = true;
    gate->fail_first_write = true;

    std::chrono::milliseconds teardown_elapsed{9999};
    bool reopened = false;
    bool worker_stopped_ok = false;
    bool no_failure = false;

    {
        diag::StdoutLogScope logging{wr::running};
        // Pre-fill entry_capacity - 1 lines while STDOUT_FILENO pipe is full: entry #0
        // blocks in ::write(STDOUT_FILENO), publish_bypass_logs fills slot #4096, and
        // log_sink_selected("reopen") blocks inside StdoutLog::submit on slot #4097.
        {
            const std::string line(200, 'a');
            for (std::size_t i = 0; i + 1 < diag::StdoutLog::entry_capacity; ++i) {
                if (logging.log.submit(line)) break;
            }
        }

        audio::AudioRenderWorker worker{
            wr::audio_config(audio::SinkBackend::pulse), wr::audio_telemetry(),
            make_gated_router(gate)};
        worker_stopped_ok = worker.ready().has_value();

        const auto role = proto::ChannelRole::media_audio;
        worker.begin_session();
        worker.invalidate(role, 1);
        const bool submitted = worker.submit(
            {{role, 1, {1'000'000'000LL}}, pcm_50ms()}, {role, 1, 1, 1}).has_value();
        worker_stopped_ok = worker_stopped_ok && submitted && gate->wait_entered(1);

        // Release blocked failing write so worker reopens sink and calls log_sink_selected("reopen")
        // under StdoutLog backpressure while holding mutex_ until running becomes false.
        gate->release();
        const auto reopen_deadline = Clock::now() + 2s;
        while (Clock::now() < reopen_deadline) {
            {
                std::lock_guard lock(gate->mutex);
                if (gate->sinks_opened >= 2U) {
                    reopened = true;
                    break;
                }
            }
            std::this_thread::sleep_for(5ms);
        }
        std::this_thread::sleep_for(20ms);

        // Runtime teardown sets running=false so StdoutLog shutdown wait finishes boundedly (~2s+2.2s).
        const auto t_stop = Clock::now();
        wr::running.store(false);
        worker.stop();
        no_failure = worker.finish().has_value() && !worker.failure().has_value();
        logging.log.finish(std::chrono::milliseconds{2200});
        teardown_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t_stop);
    }

    restore_fd();
    EXPECT_TRUE(worker_stopped_ok);
    EXPECT_TRUE(reopened);
    EXPECT_TRUE(no_failure);
    EXPECT_LT(teardown_elapsed.count(), 6000);
}

TEST(WirelessAudioRuntime, FatalWorkerFailureTeardownBoundedWithFullStalledStdout) {
    wr::running.store(true);
    int pfd[2]{-1, -1};
    ASSERT_EQ(::pipe(pfd), 0);
    const int saved_stdout = ::dup(STDOUT_FILENO);
    ASSERT_GE(saved_stdout, 0);

    {
        const int flags = ::fcntl(pfd[1], F_GETFL);
        ASSERT_GE(flags, 0);
        ASSERT_EQ(::fcntl(pfd[1], F_SETFL, flags | O_NONBLOCK), 0);
        std::vector<char> fill(4096, 'x');
        while (::write(pfd[1], fill.data(), fill.size()) > 0) {}
        ASSERT_EQ(::fcntl(pfd[1], F_SETFL, flags), 0);
    }

    ASSERT_EQ(::dup2(pfd[1], STDOUT_FILENO), STDOUT_FILENO);
    ::close(pfd[1]);
    pfd[1] = -1;

    auto close_pipe_and_restore_stdout = [&] {
        if (pfd[0] >= 0) {
            ::close(pfd[0]);
            pfd[0] = -1;
        }
        std::this_thread::sleep_for(20ms);
        ::dup2(saved_stdout, STDOUT_FILENO);
        ::close(saved_stdout);
    };

    std::future_status wait_status = std::future_status::timeout;
    int exit_code = -1;
    bool rt_failed = false;
    bool rt_stopping = false;
    bool running_after = true;
    std::chrono::milliseconds elapsed{9999};

    {
        TempDir unit_dir;
        aa::trust::ApprovedPhoneStore trust{unit_dir.path / "approved-phones.json"};
        wiredtest::CaptureSink dcapture;
        aa::diagnostics::Diagnostics diags{dcapture};
        ASSERT_TRUE(trust.load());
        const aa::trust::TransportIdentity wid{
            aa::trust::WirelessIdentity{"12:34:56:78:9A:BC", "12:34:56:78:9A:BD"}};
        ASSERT_TRUE(trust.approve(wid));

        const char* dbus_addr = std::getenv("DBUS_SESSION_BUS_ADDRESS");
        ASSERT_NE(dbus_addr, nullptr);
        auto helper = helper::BusClient::private_bus_for_test(dbus_addr, trust);
        ASSERT_TRUE(helper);

        ss::SteadyClock sclock;
        aa::trust::SteadyClock tclock;
        aa::trust::PairingPolicy phones{trust, tclock};
        ipc::RandomPairingIdSource ids;

        auto gate = std::make_shared<SinkGate>();
        audio::AudioRenderWorker bad_worker{
            wr::audio_config(audio::SinkBackend::pulse), wr::audio_telemetry(),
            make_gated_router(gate)};
        ASSERT_TRUE(bad_worker.ready());
        wr::Media bad_media{bad_worker};
        app::WirelessReceiver wrx{{trust, sclock, diags}, *helper.value(), bad_media.config()};
        auto bus = ipc::ControlBusService::start({ipc::current_uid()},
                                                 {ids, phones, &wrx.confirmation()});
        ASSERT_TRUE(bus);
        auto vserv = ipc::VideoServer::listen(
            ipc::VideoSocketPath{(unit_dir.path / "v.sock").string()},
            ipc::VideoServerOptions{{}, ipc::current_uid()});
        ASSERT_TRUE(vserv);
        auto iserv = ipc::InputServer::listen(
            ipc::InputSocketPath{(unit_dir.path / "i.sock").string()},
            ipc::InputServerOptions{ipc::current_uid()});
        ASSERT_TRUE(iserv);

        diag::StdoutLogScope logging{wr::running};
        {
            const std::string line(200, 'f');
            for (std::size_t i = 0; i < diag::StdoutLog::entry_capacity; ++i) {
                if (logging.log.submit(line)) break;
            }
        }

        // Pre-latch worker failure, then run real Runtime::run() with running==true, a 20s deadline,
        // and a full stalled stdout queue. service_audio_failure() must clear running BEFORE log_line().
        const auto role = proto::ChannelRole::media_audio;
        bad_worker.invalidate(role, 55);
        ASSERT_TRUE(bad_worker.submit({{role, 55, {-1LL}}, wiredtest::pcm_unit()}, {role, 55, 55, 1}));
        ASSERT_TRUE(bad_worker.wait_idle(Clock::now() + 2s));
        ASSERT_TRUE(bad_worker.failure());

        wr::Runtime rt{wrx, *helper.value(), bus.value(), vserv.value(), iserv.value(),
                       bad_media, wid, Clock::now() + 20s, std::nullopt, {}, std::nullopt};
        std::promise<int> done_promise;
        auto done_future = done_promise.get_future();
        const auto t0 = Clock::now();
        std::thread runner([&] { done_promise.set_value(rt.run()); });
        wait_status = done_future.wait_for(std::chrono::milliseconds{4500});
        elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0);
        if (wait_status == std::future_status::ready) {
            exit_code = done_future.get();
            rt_failed = rt.failed;
            rt_stopping = rt.stopping;
            running_after = wr::running.load();
            runner.join();
        } else {
            wr::running.store(false);
            close_pipe_and_restore_stdout();
            runner.join();
        }
        logging.log.finish(std::chrono::milliseconds{200});
        bus.value().close();
    }

    if (pfd[0] >= 0) {
        close_pipe_and_restore_stdout();
    }
    EXPECT_EQ(wait_status, std::future_status::ready)
        << "Runtime::run() blocked on full stalled stdout during fatal worker shutdown";
    EXPECT_EQ(exit_code, 1);
    EXPECT_TRUE(rt_failed);
    EXPECT_TRUE(rt_stopping);
    EXPECT_FALSE(running_after);
    EXPECT_LT(elapsed.count(), 4500);
}

TEST(WirelessAudioRuntime, RuntimeAvPendingDuplicateBeforeAckEmitsQueueRejectionAndCreditTrace) {
    StdoutCapture capture;
    std::string output;
    int exit_code = -1;
    {
        RuntimeFixture f;
        f.gate->block_first_write = true;
        ASSERT_NO_FATAL_FAILURE(f.start(Clock::now() + 20s));
        ASSERT_TRUE(f.backend->wait_authorized(1));

        auto phone_res = PhonePeer::connect(f.port);
        ASSERT_TRUE(phone_res);
        auto phone = std::move(phone_res).value();
        ASSERT_NO_FATAL_FAILURE(phone->handshake_and_open_av(-7));

        ASSERT_TRUE(phone->send_msg(100, av_data_msg(1'000'000'000ULL, pcm_50ms())));
        ASSERT_TRUE(phone->wait_audio_ack(-7));
        ASSERT_TRUE(f.gate->wait_entered(1));

        ASSERT_TRUE(phone->send_msg(100, av_data_msg(1'050'000'000ULL, pcm_50ms())));
        ASSERT_TRUE(phone->wait_audio_ack(-7));

        // Chunk 3 holds AvSink::pending_ while reservation credit is deferred;
        // Chunk 4 hits av_pending and emits poll_error_json with av_credit_trace.
        ASSERT_TRUE(phone->send_msg(100, av_data_msg(1'100'000'000ULL, pcm_50ms())));
        {
            std::unique_lock lock(f.gate->mutex);
            (void)f.gate->cv.wait_for(lock, 40ms, [] { return false; });
        }
        ASSERT_TRUE(phone->send_msg(100, av_data_msg(1'150'000'000ULL, pcm_50ms())));

        // Wait for Runtime to retire link 1 and open listener 2.
        ASSERT_TRUE(f.backend->wait_authorized(2));
        f.gate->release();
        EXPECT_TRUE(f.consumer_client->stop_projection());
        phone->close();
        exit_code = f.stop_and_join();
    }
    output = capture.stop();
    maybe_dump_jsonl("av_pending", output);

    EXPECT_EQ(exit_code, 0) << output;
    EXPECT_NE(output.find("\"event\":\"err\",\"detail\":\"poll\",\"code\":\"ipc_queue_full\""),
              std::string::npos) << output;
    EXPECT_NE(output.find("\"queue_rejection\":{\"source\":\"av_pending\""),
              std::string::npos) << output;
    EXPECT_NE(output.find("\"av_credit_trace\":{"), std::string::npos) << output;
}

TEST(WirelessAudioRuntime, UnregisteredChannelPollErrorEmitsRejectedFrameFields) {
    StdoutCapture capture;
    std::string output;
    int exit_code = -1;
    {
        RuntimeFixture f;
        ASSERT_NO_FATAL_FAILURE(f.start(Clock::now() + 20s));
        ASSERT_TRUE(f.backend->wait_authorized(1));

        auto phone_res = PhonePeer::connect(f.port);
        ASSERT_TRUE(phone_res);
        auto phone = std::move(phone_res).value();
        ASSERT_NO_FATAL_FAILURE(phone->handshake_and_open_av(-7));

        // Send an encrypted frame for unregistered channel 120 with message id 0x8005 (32773).
        ASSERT_TRUE(phone->send_msg(120, {std::byte{0x80}, std::byte{0x05}, std::byte{0x00}}));

        ASSERT_TRUE(f.backend->wait_authorized(2));
        EXPECT_TRUE(f.consumer_client->stop_projection());
        phone->close();
        exit_code = f.stop_and_join();
    }
    output = capture.stop();
    maybe_dump_jsonl("unregistered_channel", output);

    EXPECT_EQ(exit_code, 0) << output;
    EXPECT_NE(output.find("\"event\":\"err\",\"detail\":\"poll\",\"code\":\"channel_not_registered\","
                          "\"reject_channel\":120,\"reject_msg_id\":32773"),
              std::string::npos) << output;
}

TEST(WirelessAudioRuntime, SynchronousAudioSinkBaselineVsRenderWorkerBehavior) {
    // 1. Synchronous AudioSink baseline blocks poll_transport() while sink->write() holds.
    {
        auto gate = std::make_shared<SinkGate>();
        gate->block_first_write = true;
        auto router = audio::detail::open_router_with_sink_for_test(
            wr::audio_config(audio::SinkBackend::pulse), std::make_unique<GatedSink>(gate));
        ASSERT_TRUE(router);
        SyncRouterAudioSink baseline_sink{router.value()};

        ss::test::MapTrustStore trust;
        ss::test::ManualClock clock;
        wiredtest::CaptureSink dcapture;
        aa::diagnostics::Diagnostics diagnostics{dcapture};
        wiredtest::ScriptedTransport transport;
        app::WiredSession session{{trust, clock, diagnostics}};

        const aa::trust::PhoneIdentity phone{"baseline-phone"};
        trust.set(phone, aa::trust::Decision::approved);
        ASSERT_TRUE(session.configure({
            {proto::ServiceKey{99}, proto::ChannelRole::video,
             proto::VideoConfiguration{{{800, 480, 30}}}},
            {proto::ServiceKey{100}, proto::ChannelRole::media_audio,
             proto::AudioConfiguration{{{48000, 16, 2}}}},
        }));
        EXPECT_TRUE(session.register_av_sink(proto::ChannelRole::media_audio, baseline_sink));

        session.attach_transport(&transport);
        auto owned = std::make_unique<wiredtest::FakeHandshakeCipher>();
        auto* cipher = owned.get();
        session.attach_cipher(std::move(owned));
        ASSERT_TRUE(session.apply(ss::Event::start_discovery));
        ASSERT_TRUE(session.phone_discovered(phone));
        ASSERT_TRUE(session.apply(ss::Event::transport_ready));
        wiredtest::walk_auth(session, transport);
        ASSERT_TRUE(session.announce_services());
        for (const std::int32_t s : {99, 100}) {
            transport.push_inbound(wiredtest::open_request_frame(s));
            ASSERT_TRUE(session.poll_transport());
        }
        ASSERT_TRUE(session.apply(ss::Event::negotiation_succeeded));

        auto push_enc = [&](std::uint8_t ch, std::vector<std::byte> payload) {
            auto frames = tr::encode_message(
                {ch, tr::MessageKind::specific, std::move(payload)},
                tr::Encryption::encrypted, cipher);
            ASSERT_TRUE(frames);
            for (auto& fr : frames.value()) transport.push_inbound(std::move(fr));
        };
        push_enc(100, proto::encode(proto::AvSinkSetup{}).value());
        ASSERT_TRUE(session.poll_transport());
        push_enc(100, proto::encode(proto::AvSinkStart{-7, 0}).value());
        ASSERT_TRUE(session.poll_transport());
        push_enc(100, av_data_msg(1'000'000'000ULL, pcm_50ms()));

        std::atomic<bool> poll_finished{false};
        std::thread poller([&] {
            EXPECT_TRUE(session.poll_transport());
            poll_finished.store(true);
        });
        ASSERT_TRUE(gate->wait_entered(1));
        std::this_thread::sleep_for(60ms);
        EXPECT_FALSE(poll_finished.load());
        gate->release();
        poller.join();
        EXPECT_TRUE(poll_finished.load());
    }

    // 2. AudioRenderWorker path returns poll_transport() immediately while sink->write() holds.
    {
        auto gate = std::make_shared<SinkGate>();
        gate->block_first_write = true;
        audio::AudioRenderWorker worker{
            wr::audio_config(audio::SinkBackend::pulse), wr::audio_telemetry(),
            make_gated_router(gate)};
        ASSERT_TRUE(worker.ready());

        ss::test::MapTrustStore trust;
        ss::test::ManualClock clock;
        wiredtest::CaptureSink dcapture;
        aa::diagnostics::Diagnostics diagnostics{dcapture};
        wiredtest::ScriptedTransport transport;
        app::WiredSession session{{trust, clock, diagnostics}};

        const aa::trust::PhoneIdentity phone{"worker-phone"};
        trust.set(phone, aa::trust::Decision::approved);
        wr::Media media{worker};
        ASSERT_TRUE(session.configure(media.config().services));
        auto video_pair = videotest::make_pair(ipc::NegotiatedLimits{});
        ASSERT_TRUE(video_pair);
        media.video.install(std::move(video_pair.value().producer));
        ASSERT_TRUE(media.bind(session));

        session.attach_transport(&transport);
        auto owned = std::make_unique<wiredtest::FakeHandshakeCipher>();
        auto* cipher = owned.get();
        session.attach_cipher(std::move(owned));
        ASSERT_TRUE(session.apply(ss::Event::start_discovery));
        ASSERT_TRUE(session.phone_discovered(phone));
        ASSERT_TRUE(session.apply(ss::Event::transport_ready));
        wiredtest::walk_auth(session, transport);
        ASSERT_TRUE(session.announce_services());
        for (const std::int32_t s : {99, 100, 101, 102, 104}) {
            transport.push_inbound(wiredtest::open_request_frame(s));
            ASSERT_TRUE(session.poll_transport());
        }
        ASSERT_TRUE(session.apply(ss::Event::negotiation_succeeded));

        auto push_enc = [&](std::uint8_t ch, std::vector<std::byte> payload) {
            auto frames = tr::encode_message(
                {ch, tr::MessageKind::specific, std::move(payload)},
                tr::Encryption::encrypted, cipher);
            ASSERT_TRUE(frames);
            for (auto& fr : frames.value()) transport.push_inbound(std::move(fr));
        };
        push_enc(99, proto::encode(proto::AvSinkSetup{proto::AvSinkCodec::h264}).value());
        ASSERT_TRUE(session.poll_transport());
        push_enc(99, proto::encode(proto::AvSinkStart{1, 0}).value());
        ASSERT_TRUE(session.poll_transport());
        push_enc(100, proto::encode(proto::AvSinkSetup{}).value());
        ASSERT_TRUE(session.poll_transport());
        push_enc(100, proto::encode(proto::AvSinkStart{-7, 0}).value());
        ASSERT_TRUE(session.poll_transport());
        push_enc(100, av_data_msg(1'000'000'000ULL, pcm_50ms()));

        const auto t0 = Clock::now();
        ASSERT_TRUE(session.poll_transport());
        EXPECT_LT(Clock::now() - t0, 50ms);
        ASSERT_TRUE(gate->wait_entered(1));

        // Control ping and video IDR complete immediately while audio sink write remains blocked.
        const std::size_t sent_before = transport.sent_count();
        push_enc(0, proto::encode(proto::PingRequest{77, false, {}}).value());
        ASSERT_TRUE(session.poll_transport());
        EXPECT_GT(transport.sent_count(), sent_before);

        push_enc(99, av_data_msg(1'050'000'000ULL, wiredtest::kVideoIdr));
        ASSERT_TRUE(session.poll_transport());
        const auto v = video_pair.value().consumer.receive(aa::core::Milliseconds{500});
        ASSERT_TRUE(v);
        EXPECT_EQ(v.value(), ipc::ReceiveStatus::complete);

        gate->release();
        ASSERT_TRUE(worker.wait_idle(Clock::now() + 2s));
        worker.stop();
        EXPECT_TRUE(worker.finish());
    }
}

} // namespace
