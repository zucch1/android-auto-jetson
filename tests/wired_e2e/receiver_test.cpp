// SPDX-License-Identifier: GPL-3.0-or-later
// Exercise the actual harness adapters, not copies of their logging logic.
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {
// This state is private to the actual-main test inclusion. No callback registry
// lock is involved, and lock() never waits on the real mutex with state held.
struct ShutdownObservation {
    using Clock = std::chrono::steady_clock;
    struct Event {
        std::string name;
        std::thread::id thread;
    };
    std::mutex state;
    std::condition_variable cv;
    std::vector<Event> trace;
    std::thread::id owner;
    std::thread::id dispatcher;
    std::thread::id reset_owner;
    Clock::time_point published_at{};
    bool published = false;
    bool receive_held = false;
    bool release_receive = false;
    bool release_main = false;
    bool cleanup_held = false;
    bool release_cleanup = false;
    bool main_attempt = false;
    bool unprotected_stop = false;
    bool unprotected_during_cleanup = false;
    bool stop_owned = false;
    bool join_owned = false;
    bool reset_ok = false;
    bool gate_failed = false;
    bool closed = false;
    int resets = 0;
    int stops = 0;
    int joins = 0;
    int pings = 0;
    int replies = 0;
    int final_receives = 0;
    int post_close_io = 0;

    void record(const char *name) {
        trace.push_back({name, std::this_thread::get_id()});
        cv.notify_all();
    }
    void release_all() {
        release_receive = release_main = release_cleanup = true;
        cv.notify_all();
    }
    template <class Predicate>
    bool wait(std::unique_lock<std::mutex> &lock, Predicate predicate) {
        if (cv.wait_for(lock, std::chrono::seconds{2}, predicate)) {
            return true;
        }
        gate_failed = true;
        release_all();
        return false;
    }
    void published_false() {
        std::unique_lock lock(state);
        dispatcher = std::this_thread::get_id();
        published_at = Clock::now();
        published = true;
        record("false");
        (void)wait(lock, [&] { return release_main; });
    }
    void main_stop() {
        std::unique_lock lock(state);
        ++stops;
        stop_owned = owner == std::this_thread::get_id();
        if (!stop_owned) {
            unprotected_stop = true;
            unprotected_during_cleanup = cleanup_held && owner == reset_owner;
            record("MAIN_STOP_WITHOUT_SESSION_MUTEX");
            // The mutant proves the invariant violation, never a data race.
            // Release all gates on failure; do not access the source until reset.
            if (!wait(lock, [&] { return resets == 1; }) &&
                !wait(lock, [&] { return resets == 1; })) {
                std::abort();
            }
        }
        record("stop");
    }
    void main_join() {
        const std::lock_guard lock(state);
        ++joins;
        join_owned = owner == std::this_thread::get_id();
        record("join");
    }
    void reset_finished(bool ok) {
        const std::lock_guard lock(state);
        ++resets;
        reset_ok = ok && owner == std::this_thread::get_id();
        record("reset");
    }
    void io() {
        const std::lock_guard lock(state);
        if (closed) {
            ++post_close_io;
        }
    }
    void cleanup_close() {
        std::unique_lock lock(state);
        closed = true;
        cleanup_held = true;
        reset_owner = std::this_thread::get_id();
        record("close");
        (void)wait(lock, [&] { return release_cleanup; });
        cleanup_held = false;
    }
};
ShutdownObservation *shutdown_observation = nullptr;

class ObservedSessionMutex {
  public:
    void lock() {
        if (shutdown_observation) {
            const std::lock_guard lock(shutdown_observation->state);
            if (shutdown_observation->published &&
                shutdown_observation->dispatcher == std::this_thread::get_id()) {
                shutdown_observation->main_attempt = true;
            }
            shutdown_observation->record("attempt");
        }
        mutex_.lock();
        if (shutdown_observation) {
            const std::lock_guard lock(shutdown_observation->state);
            shutdown_observation->owner = std::this_thread::get_id();
            shutdown_observation->record("acquire");
        }
    }
    void unlock() {
        if (shutdown_observation) {
            const std::lock_guard lock(shutdown_observation->state);
            shutdown_observation->record("release");
            shutdown_observation->owner = {};
            mutex_.unlock();
        } else {
            mutex_.unlock();
        }
    }
  private:
    std::mutex mutex_;
};
} // namespace

#define AA_WIRED_RECEIVER_SHUTDOWN_TEST
#define main wired_receiver_main
#include "../../tools/wired-receiver/main.cpp" // NOLINT(bugprone-suspicious-include)
#undef main
#undef AA_WIRED_RECEIVER_SHUTDOWN_TEST

#include "harness.hpp"
#include "../session/Fakes.hpp"
#include "../video_socket/pair.hpp"
#include "../trust/syscall_faults.hpp"
#include "../transport/UsbAoaFakes.hpp"
#include "../../src/transport/usb_adapter/LibusbWorld.hpp"
#include "../../src/ipc/dbus_adapter/BusConnection.hpp"
#include <aap_protobuf/service/control/message/ServiceDiscoveryResponse.pb.h>
#include <gtest/gtest.h>
#include <array>
#include <condition_variable>
#include <fstream>
#include <iterator>
#include <pthread.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <sstream>
#include <unistd.h>

namespace {
namespace media_wire = aap_protobuf::service::media::sink::message;

TEST(WiredReceiver, VideoOutletNoConsumerDropsAreNotSent) {
    VideoOutlet outlet{log_line};
    const std::vector<std::byte> payload{std::byte{0}, std::byte{0}, std::byte{1},
                                         std::byte{0x65}, std::byte{0x10}};
    for (int index = 0; index < 31; ++index) {
        testing::internal::CaptureStdout();
        const auto result = outlet.on_message({{aa::protocol::ChannelRole::video, 1, {}}, payload});
        const auto log = testing::internal::GetCapturedStdout();
        ASSERT_TRUE(result);
        if (index % 30 == 0) {
            EXPECT_EQ(log, "{\"event\":\"video_dropped\",\"reason\":\"no_consumer\",\"count\":" +
                               std::to_string(index + 1) + "}\n" +
                               "{\"event\":\"decode_stats\",\"frame_drops\":" +
                               std::to_string(index + 1) + "}\n");
        } else {
            EXPECT_TRUE(log.empty());
        }
    }
}

TEST(WiredReceiver, VideoOutletWaitingIdrDropsDoNotCountAsSent) {
    auto pair = videotest::make_pair({});
    ASSERT_TRUE(pair);
    VideoOutlet outlet{log_line};
    outlet.install(std::move(pair.value().producer));
    const std::vector<std::byte> non_idr{std::byte{0}, std::byte{0}, std::byte{1},
                                         std::byte{0x41}, std::byte{0x10}};
    const std::vector<std::byte> idr{std::byte{0}, std::byte{0}, std::byte{1},
                                     std::byte{0x65}, std::byte{0x10}};
    for (int index = 0; index < 31; ++index) {
        testing::internal::CaptureStdout();
        const auto result = outlet.on_message({{aa::protocol::ChannelRole::video, 1, {}}, non_idr});
        const auto log = testing::internal::GetCapturedStdout();
        ASSERT_TRUE(result);
        if (index % 30 == 0) {
            EXPECT_EQ(log, "{\"event\":\"video_dropped\",\"reason\":\"waiting_idr\",\"count\":" +
                               std::to_string(index + 1) + "}\n" +
                               "{\"event\":\"decode_stats\",\"frame_drops\":" +
                               std::to_string(index + 1) + "}\n");
        } else {
            EXPECT_TRUE(log.empty());
        }
    }
    const auto empty = pair.value().consumer.receive(Milliseconds{0});
    ASSERT_FALSE(empty);
    EXPECT_EQ(empty.error().code(), aa::ErrorCode::timeout);

    for (std::uint64_t delivered = 1; delivered <= 31; ++delivered) {
        const auto &payload = delivered == 1 ? idr : non_idr;
        testing::internal::CaptureStdout();
        const auto result = outlet.on_message({{aa::protocol::ChannelRole::video, 1, {123}}, payload});
        const auto log = testing::internal::GetCapturedStdout();
        ASSERT_TRUE(result);
        ASSERT_TRUE(log.starts_with("{\"event\":\"video_ipc_send\","));
        const auto endpoint_end = log.find('\n');
        ASSERT_NE(endpoint_end, std::string::npos);
        const auto summary = log.substr(endpoint_end + 1);
        if (delivered % 30 == 1) {
            EXPECT_EQ(summary, "{\"event\":\"video_sent\",\"units\":" +
                               std::to_string(delivered) + "}\n" +
                               "{\"event\":\"decode_stats\",\"frame_drops\":31}\n");
        } else {
            EXPECT_TRUE(summary.empty());
        }
        const auto received = pair.value().consumer.receive(Milliseconds{500});
        ASSERT_TRUE(received);
        ASSERT_EQ(received.value(), aa::ipc::ReceiveStatus::complete);
        const auto &unit = pair.value().consumer.access_unit();
        EXPECT_EQ(unit.frame, 31 + delivered);
        EXPECT_EQ(unit.timestamp_ns, 123U);
        EXPECT_EQ(unit.idr, delivered == 1);
        EXPECT_EQ(unit.payload, (std::vector<std::uint8_t>{0, 0, 1,
                                  static_cast<std::uint8_t>(delivered == 1 ? 0x65 : 0x41), 0x10}));
    }
}

TEST(WiredReceiver, VideoOutletSendErrorsDoNotCountAsSent) {
    auto pair = videotest::make_pair({});
    ASSERT_TRUE(pair);
    VideoOutlet outlet{log_line};
    outlet.install(std::move(pair.value().producer));
    testing::internal::CaptureStdout();
    const auto invalid = outlet.on_message({{aa::protocol::ChannelRole::video, 1, {}}, {}});
    const auto invalid_log = testing::internal::GetCapturedStdout();
    ASSERT_FALSE(invalid);
    EXPECT_EQ(invalid.error().code(), aa::ErrorCode::invalid_argument);
    EXPECT_TRUE(invalid_log.empty());

    const std::vector<std::byte> idr{std::byte{0}, std::byte{0}, std::byte{1},
                                     std::byte{0x65}, std::byte{0x10}};
    testing::internal::CaptureStdout();
    const auto sent = outlet.on_message({{aa::protocol::ChannelRole::video, 1, {}}, idr});
    const auto sent_log = testing::internal::GetCapturedStdout();
    ASSERT_TRUE(sent);
    ASSERT_TRUE(sent_log.starts_with("{\"event\":\"video_ipc_send\","));
    const auto endpoint_end = sent_log.find('\n');
    ASSERT_NE(endpoint_end, std::string::npos);
    EXPECT_EQ(sent_log.substr(endpoint_end + 1), "{\"event\":\"video_sent\",\"units\":1}\n"
                                               "{\"event\":\"decode_stats\",\"frame_drops\":0}\n");
    const auto received = pair.value().consumer.receive(Milliseconds{500});
    ASSERT_TRUE(received);
    ASSERT_EQ(received.value(), aa::ipc::ReceiveStatus::complete);
    EXPECT_EQ(pair.value().consumer.access_unit().frame, 2U);
}

struct ReceiverAudioExpectation {
    std::int32_t service;
    aa::protocol::ChannelRole channel;
    aa::audio::Role role;
    int wire_role;
    aa::audio::AudioFormat input;
};

const std::array receiver_audio_expectations{
    ReceiverAudioExpectation{100, aa::protocol::ChannelRole::media_audio, aa::audio::Role::media,
                             media_wire::AUDIO_STREAM_MEDIA, {48000, 2, 16}},
    ReceiverAudioExpectation{101, aa::protocol::ChannelRole::guidance_audio, aa::audio::Role::guidance,
                             media_wire::AUDIO_STREAM_GUIDANCE, {16000, 1, 16}},
    ReceiverAudioExpectation{102, aa::protocol::ChannelRole::system_audio, aa::audio::Role::system,
                             media_wire::AUDIO_STREAM_SYSTEM_AUDIO, {16000, 1, 16}},
    ReceiverAudioExpectation{103, aa::protocol::ChannelRole::speech_audio, aa::audio::Role::speech,
                             media_wire::AUDIO_STREAM_TELEPHONY, {16000, 1, 16}},
};

TEST(WiredReceiver, ProductionAudioProfilesMatchWireAndRouterInputs) {
    const auto services = service_descriptors(aa::input::CapabilityDescriptor::bench_defaults());
    const auto encoded = aa::protocol::encode(aa::protocol::ServiceDiscoveryResponse{services});
    ASSERT_TRUE(encoded.has_value());
    ASSERT_GE(encoded.value().size(), 2U);
    aap_protobuf::service::control::message::ServiceDiscoveryResponse wire;
    ASSERT_TRUE(wire.ParseFromArray(encoded.value().data() + 2,
                                   static_cast<int>(encoded.value().size() - 2)));
    const auto decoded = aa::protocol::decode_service_discovery_response(encoded.value());
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(decoded.value().services, services);
    ASSERT_EQ(wire.channels_size(), 7);
    const auto router = pcm_router_config();
    EXPECT_EQ(router.output_format, (aa::audio::AudioFormat{48000, 2, 16}));
    EXPECT_TRUE(router.on_write_completed);
    for (const auto &expected : receiver_audio_expectations) {
        if (expected.channel == aa::protocol::ChannelRole::speech_audio) { continue; }
        SCOPED_TRACE(expected.service);
        const auto entry = std::find_if(wire.channels().begin(), wire.channels().end(),
            [&](const auto &channel) { return channel.id() == expected.service; });
        ASSERT_NE(entry, wire.channels().end());
        ASSERT_TRUE(entry->has_media_sink_service());
        const auto &media = entry->media_sink_service();
        EXPECT_EQ(media.audio_type(), expected.wire_role);
        EXPECT_EQ(media.available_type(),
                  aap_protobuf::service::media::shared::message::MEDIA_CODEC_AUDIO_PCM);
        ASSERT_EQ(media.audio_configs_size(), 1);
        const auto &profile = media.audio_configs(0);
        EXPECT_EQ(profile.sampling_rate(), expected.input.sample_rate_hz);
        EXPECT_EQ(profile.number_of_bits(), expected.input.bits_per_sample);
        EXPECT_EQ(profile.number_of_channels(), expected.input.channels);
        const auto &input = router.roles[aa::audio::role_index(expected.role)];
        EXPECT_EQ(input.codec, aa::audio::Codec::pcm);
        EXPECT_EQ(input.input_format, expected.input);
        EXPECT_EQ(input.input_format.sample_rate_hz, profile.sampling_rate());
        EXPECT_EQ(input.input_format.bits_per_sample, profile.number_of_bits());
        EXPECT_EQ(input.input_format.channels, profile.number_of_channels());
        const auto descriptor = std::find_if(decoded.value().services.begin(),
            decoded.value().services.end(),
            [&](const auto &service) { return service.id.value == expected.service; });
        ASSERT_NE(descriptor, decoded.value().services.end());
        EXPECT_EQ(descriptor->role, expected.channel);
    }
    const auto mic = std::find_if(wire.channels().begin(), wire.channels().end(),
        [](const auto &entry) { return entry.id() == 106; });
    ASSERT_NE(mic, wire.channels().end());
    ASSERT_TRUE(mic->has_media_source_service());
    const auto &source = mic->media_source_service();
    EXPECT_EQ(source.available_type(),
              aap_protobuf::service::media::shared::message::MEDIA_CODEC_AUDIO_PCM);
    EXPECT_TRUE(source.has_available_while_in_call());
    EXPECT_FALSE(source.available_while_in_call());
    ASSERT_TRUE(source.has_audio_config());
    EXPECT_EQ(source.audio_config().sampling_rate(), 16000u);
    EXPECT_EQ(source.audio_config().number_of_bits(), 16u);
    EXPECT_EQ(source.audio_config().number_of_channels(), 1u);
    for (const auto &entry : wire.channels()) { EXPECT_NE(entry.id(), 103); }
}

class ReceiverProfileSink final : public aa::audio::detail::ObservedAudioSink {
  public:
    aa::audio::AudioFormat format{};
    std::size_t writes{};
    std::vector<std::byte> pcm;
    aa::core::Result<void> write(const aa::audio::MixedChunk &chunk) override {
        ++writes;
        format = chunk.format;
        pcm.insert(pcm.end(), chunk.pcm.begin(), chunk.pcm.end());
        return {};
    }
    aa::core::Result<void> drain(aa::core::Milliseconds) override { return {}; }
    aa::audio::SinkProbeStats probe_stats() const override { return {}; }
    aa::audio::RenderBarrierStats render_stats() const override { return {}; }
};

TEST(WiredReceiver, ProductionAvRegistrationNegotiatesEncryptedMediaWithoutSetupPcm) {
    namespace p = aa::protocol;
    namespace tr = aa::transport;
    aa::session::test::MapTrustStore trust;
    aa::session::test::ManualClock clock;
    wiredtest::CaptureSink capture;
    aa::diagnostics::Diagnostics diagnostics{capture};
    wiredtest::ScriptedTransport transport;
    auto output = std::make_unique<ReceiverProfileSink>();
    const auto *observed = output.get();
    auto router = aa::audio::detail::open_router_with_sink_for_test(
        pcm_router_config(), std::move(output));
    ASSERT_TRUE(router);
    RouterSink audio{router.value()};
    auto pair = videotest::make_pair({});
    ASSERT_TRUE(pair);
    VideoOutlet video{log_line};
    video.install(std::move(pair.value().producer));
    aa::app::WiredSession session{{trust, clock, diagnostics}};
    ASSERT_TRUE(session.configure(service_descriptors(aa::input::CapabilityDescriptor::bench_defaults())));
    ASSERT_TRUE(register_av_sinks(session, video, audio));
    session.attach_transport(&transport);
    auto owned = std::make_unique<wiredtest::FakeHandshakeCipher>();
    auto *cipher = owned.get();
    session.attach_cipher(std::move(owned));
    wiredtest::walk_auth(session, transport);
    ASSERT_TRUE(session.authenticated());
    ASSERT_TRUE(session.announce_services());
    const auto receive = [&](std::uint8_t channel, std::vector<std::byte> payload) {
        auto frames = tr::encode_message({channel, tr::MessageKind::specific, std::move(payload)},
                                         tr::Encryption::encrypted, cipher);
        if (!frames) { return aa::core::Result<bool>{frames.error()}; }
        for (auto &frame : frames.value()) { transport.push_inbound(std::move(frame)); }
        return session.poll_transport();
    };
    const auto reply = [&](std::uint8_t channel, const std::vector<std::byte> &expected) {
        const auto frame = tr::decode_frame(transport.last_sent());
        ASSERT_TRUE(frame);
        EXPECT_EQ(frame.value().header.channel, channel);
        EXPECT_EQ(frame.value().header.kind, tr::MessageKind::specific);
        EXPECT_EQ(frame.value().header.encryption, tr::Encryption::encrypted);
        const auto payload = cipher->open(frame.value().payload);
        ASSERT_TRUE(payload);
        EXPECT_EQ(payload.value(), expected);
    };
    for (const std::uint8_t channel : {std::uint8_t{100}, std::uint8_t{101}, std::uint8_t{102}}) {
        const auto before = transport.sent_count();
        ASSERT_TRUE(receive(channel, {std::byte{0x80}, std::byte{0}, std::byte{8}, std::byte{1}}));
        EXPECT_EQ(transport.sent_count(), before + 1);
        EXPECT_TRUE(observed->pcm.empty()) << "Setup reached the production PCM router";
        EXPECT_EQ(observed->writes, 0U);
        reply(channel, p::encode_av_sink_ready().value());
    }
    ASSERT_TRUE(receive(99, p::encode(p::AvSinkSetup{p::AvSinkCodec::h264}).value()));
    reply(99, p::encode(p::VideoFocusNotification{p::VideoFocusMode::projected, true}).value());
    ASSERT_TRUE(receive(99, p::encode(p::AvSinkStart{-7, 0}).value()));
    auto config = std::vector<std::byte>{std::byte{0}, std::byte{1}, std::byte{0},
        std::byte{0}, std::byte{0}, std::byte{1}, std::byte{0x67}, std::byte{0x42}};
    ASSERT_TRUE(receive(99, config));
    reply(99, p::encode_av_sink_ack(-7).value());
    const auto empty = pair.value().consumer.receive(Milliseconds{0});
    ASSERT_FALSE(empty);
    EXPECT_EQ(empty.error().code(), aa::ErrorCode::timeout);
    std::vector<std::byte> data(10, std::byte{0x55});
    data[0] = data[1] = std::byte{0};
    data.insert(data.end(), wiredtest::kVideoIdr.begin(), wiredtest::kVideoIdr.end());
    clock.set(aa::core::Nanoseconds{987654});
    ASSERT_TRUE(receive(99, data));
    reply(99, p::encode_av_sink_ack(-7).value());
    ASSERT_TRUE(pair.value().consumer.receive(Milliseconds{500}));
    const auto &unit = pair.value().consumer.access_unit();
    EXPECT_EQ(unit.timestamp_ns, 987654U);
    std::vector<std::uint8_t> expected;
    for (auto it = config.begin() + 2; it != config.end(); ++it) {
        expected.push_back(std::to_integer<std::uint8_t>(*it));
    }
    for (const auto byte : wiredtest::kVideoIdr) { expected.push_back(std::to_integer<std::uint8_t>(byte)); }
    EXPECT_EQ(unit.payload, expected);
    EXPECT_TRUE(unit.idr);
    ASSERT_TRUE(receive(100, p::encode(p::AvSinkStart{0, 0}).value()));
    data.resize(10);
    const auto pcm = wiredtest::pcm_unit();
    data.insert(data.end(), pcm.begin(), pcm.end());
    ASSERT_TRUE(receive(100, data));
    reply(100, p::encode_av_sink_ack(0).value());
    EXPECT_EQ(observed->pcm, pcm);
    EXPECT_EQ(observed->writes, 1U);
    ASSERT_TRUE(receive(0, p::encode(p::PingRequest{42, {}}).value()));
    EXPECT_EQ(session.pings_answered(), 1U);
}

TEST(WiredReceiver, ProductionAudioInputsRouteTo48kStereoAndKeepCompletionCallback) {
    for (const auto &expected : receiver_audio_expectations) {
        SCOPED_TRACE(expected.service);
        auto output = std::make_unique<ReceiverProfileSink>();
        const auto *observed = output.get();
        auto router = aa::audio::detail::open_router_with_sink_for_test(
            pcm_router_config(), std::move(output));
        ASSERT_TRUE(router.has_value());
        const std::size_t frames = expected.input.sample_rate_hz / 50U;
        const std::vector<std::byte> pcm(frames * expected.input.channels * sizeof(std::int16_t),
                                         std::byte{0x10});
        RouterSink sink{router.value()};
        testing::internal::CaptureStdout();
        const auto result = sink.on_message({{expected.channel, 1, {}}, pcm});
        const auto log = testing::internal::GetCapturedStdout();
        ASSERT_TRUE(result.has_value());
        EXPECT_EQ(observed->format, (aa::audio::AudioFormat{48000, 2, 16}));
        // Resampling may retain a filter tail; require the 3x rate expansion,
        // allowing that bounded tail rather than assuming one exact chunk size.
        if (expected.input.channels == 1) {
            EXPECT_GT(observed->pcm.size(), frames * 2U * 2U * sizeof(std::int16_t));
            EXPECT_LE(observed->pcm.size(), frames * 3U * 2U * sizeof(std::int16_t));
        } else {
            EXPECT_EQ(observed->pcm.size(), pcm.size());
        }
        ASSERT_FALSE(observed->pcm.empty());
        ASSERT_EQ(observed->pcm.size() % 4U, 0U);
        EXPECT_TRUE(std::any_of(observed->pcm.begin(), observed->pcm.end(),
                               [](std::byte value) { return value != std::byte{0}; }));
        for (std::size_t offset = 0; offset < observed->pcm.size(); offset += 4U) {
            EXPECT_EQ(observed->pcm[offset], observed->pcm[offset + 2U]);
            EXPECT_EQ(observed->pcm[offset + 1U], observed->pcm[offset + 3U]);
        }
        EXPECT_NE(log.find("\"event\":\"audio_write_completed\""), std::string::npos);
        EXPECT_NE(log.find("\"scheduled_end_ns\":"), std::string::npos);
    }
}

std::atomic<int> runtime_connects{0};
std::atomic<int> runtime_ciphers{0};
bool runtime_recovers = false;
bool runtime_quiet = false;
std::atomic<bool> runtime_waiting{false};
std::atomic<bool> runtime_closed{false};
std::mutex runtime_owner_mutex;
std::thread::id runtime_io_owner;
aa::trust::PhoneIdentity runtime_phone;
bool runtime_real_admission = false;
bool runtime_fail_before_admission = false;
std::shared_ptr<aa::test::usb_aoa::LinkState> admission_state;

class RuntimeTransport final : public aa::transport::Transport {
  public:
    explicit RuntimeTransport(bool drops) : drops_(drops) {
        frames_.push_back(wiredtest::control_frame(
            aa::protocol::encode(aa::protocol::VersionResponse{1, 6, 0}).value()));
        frames_.push_back(wiredtest::control_frame(
            aa::protocol::encode(aa::protocol::EncapsulatedSsl{{std::byte{1}}}).value()));
        frames_.push_back(wiredtest::open_request_frame(99));
    }
    aa::transport::Kind kind() const noexcept override { return aa::transport::Kind::usb_aoa; }
    aa::core::Result<void> open(aa::core::CancellationToken token) override {
        if (shutdown_observation) {
            shutdown_observation->io();
        }
        token_ = token;
        return {};
    }
    aa::core::Result<void> send(std::span<const std::byte> bytes) override {
        if (shutdown_observation) {
            shutdown_observation->io();
            // Pings use plain framing. Match the complete valid response, not
            // merely an arbitrary send during startup.
            const auto reply = wiredtest::control_frame(
                aa::protocol::encode(aa::protocol::PingResponse{1, {}}).value());
            if (std::equal(bytes.begin(), bytes.end(), reply.begin(), reply.end())) {
                const std::lock_guard lock(shutdown_observation->state);
                ++shutdown_observation->replies;
            }
        }
        return {};
    }
    void set_io_checkpoint(std::function<void()> checkpoint) override {
        checkpoint_ = std::move(checkpoint);
    }
    aa::core::Result<std::vector<std::byte>> receive() override {
        if (shutdown_observation) {
            shutdown_observation->io();
        }
        {
            const std::lock_guard lock(runtime_owner_mutex);
            runtime_io_owner = std::this_thread::get_id();
        }
        if (checkpoint_) {
            checkpoint_();
        }
        if (next_ < frames_.size()) {
            return frames_[next_++];
        }
        if (shutdown_observation) {
            std::unique_lock lock(shutdown_observation->state);
            if (shutdown_observation->pings == 3) {
                shutdown_observation->receive_held = true;
                shutdown_observation->record("receive-held");
                (void)shutdown_observation->wait(lock, [&] {
                    return shutdown_observation->release_receive;
                });
                ++shutdown_observation->final_receives;
                shutdown_observation->record("final-receive");
            }
            ++shutdown_observation->pings;
            return wiredtest::control_frame(
                aa::protocol::encode(aa::protocol::PingRequest{1, {}}).value());
        }
        if (drops_ && !runtime_quiet) {
            return aa::Error{aa::ErrorCode::transport_closed};
        }
        std::mutex mutex;
        std::condition_variable_any cv;
        std::unique_lock lock(mutex);
        runtime_waiting.store(true);
        while (!token_.stop_requested()) {
            lock.unlock();
            if (checkpoint_) {
                checkpoint_();
            }
            lock.lock();
            cv.wait_for(lock, token_, std::chrono::milliseconds{20}, [] { return false; });
        }
        close();
        return aa::Error{aa::ErrorCode::cancelled};
    }
    void close() noexcept override {
        if (shutdown_observation) {
            shutdown_observation->cleanup_close();
        }
        runtime_closed.store(true);
    }
  private:
    bool drops_;
    std::size_t next_{};
    aa::core::CancellationToken token_;
    std::function<void()> checkpoint_;
    std::vector<std::vector<std::byte>> frames_;
};
}

// Link-time seams keep the real CLI/thread loop under test without USB or audio hardware.
aa::core::Result<aa::app::WiredLink>
real_open_link(const aa::transport::UsbDeviceSelector &, const aa::transport::AoaAccessoryInfo &,
               const aa::trust::TrustStore &, const aa::core::Deadline &,
               aa::trust::WiredIdentity *)
    asm("__real__ZN2aa3app15open_wired_linkERKNS_9transport17UsbDeviceSelectorERKNS1_16AoaAccessoryInfoERKNS_5trust10TrustStoreERKNS_4core8DeadlineEPNS8_13WiredIdentityE");
aa::core::Result<aa::app::WiredLink>
runtime_open_link(const aa::transport::UsbDeviceSelector &, const aa::transport::AoaAccessoryInfo &,
                  const aa::trust::TrustStore &, const aa::core::Deadline &,
                  aa::trust::WiredIdentity *)
    asm("__wrap__ZN2aa3app15open_wired_linkERKNS_9transport17UsbDeviceSelectorERKNS1_16AoaAccessoryInfoERKNS_5trust10TrustStoreERKNS_4core8DeadlineEPNS8_13WiredIdentityE");
aa::core::Result<aa::app::WiredLink>
runtime_open_link(const aa::transport::UsbDeviceSelector &selector,
                  const aa::transport::AoaAccessoryInfo &info,
                  const aa::trust::TrustStore &store, const aa::core::Deadline &deadline,
                  aa::trust::WiredIdentity *attempted_identity) {
    const int attempt = ++runtime_connects;
    if (runtime_real_admission) {
        return real_open_link(selector, info, store, deadline, attempted_identity);
    }
    if (attempt == 1 || (runtime_recovers && attempt >= 4)) {
        return aa::app::WiredLink{std::make_unique<RuntimeTransport>(attempt == 1), runtime_phone};
    }
    return aa::Error{aa::ErrorCode::transport_unavailable};
}

aa::core::Result<std::unique_ptr<aa::transport::usb_adapter::AoaLinkFactory>>
real_link_factory() asm("__real__ZN2aa9transport11usb_adapter24make_libusb_link_factoryEv");
aa::core::Result<std::unique_ptr<aa::transport::usb_adapter::AoaLinkFactory>>
runtime_link_factory() asm("__wrap__ZN2aa9transport11usb_adapter24make_libusb_link_factoryEv");
aa::core::Result<std::unique_ptr<aa::transport::usb_adapter::AoaLinkFactory>>
runtime_link_factory() {
    if (!runtime_real_admission) {
        return real_link_factory();
    }
    auto factory = std::make_unique<aa::test::usb_aoa::FakeLinkFactory>(admission_state);
    factory->phone.serial = "SECOND-PRIVATE-USB";
    factory->accessory.serial = "SECOND-PRIVATE-AOA";
    factory->fail_switch = runtime_fail_before_admission;
    return std::unique_ptr<aa::transport::usb_adapter::AoaLinkFactory>{std::move(factory)};
}

std::unique_ptr<aa::transport::HandshakingCipher>
runtime_cipher(const std::function<bool()> &)
    asm("__wrap__ZN2aa9transport28make_client_handshake_cipherERKSt8functionIFbvEE");
std::unique_ptr<aa::transport::HandshakingCipher>
runtime_cipher(const std::function<bool()> &) {
    ++runtime_ciphers;
    return std::make_unique<wiredtest::FakeHandshakeCipher>();
}

aa::audio::detail::SinkOpenOutcome
runtime_sink(const aa::audio::detail::SinkOpenRequest &)
    asm("__wrap__ZN2aa5audio6detail17open_default_sinkERKNS1_15SinkOpenRequestE");
aa::audio::detail::SinkOpenOutcome
runtime_sink(const aa::audio::detail::SinkOpenRequest &) {
    return {std::make_unique<wiredtest::RecordingObservedSink>(
                std::make_shared<wiredtest::AudioObs>()), {}};
}

namespace {
struct AdmissionResult {
    int status{};
    bool watchdog_fired = false;
    int connects = -1;
    int ciphers = -1;
    int switches = -1;
    int claims = -1;
    std::string expected_label;
    std::string output;
};

AdmissionResult run_admission_child(bool pre_admission_failure) {
    if (const char *child_root = std::getenv("AA_TEST_ADMISSION_ROOT")) {
        const std::filesystem::path root{child_root};
        runtime_real_admission = true;
        runtime_fail_before_admission = std::string{std::getenv("AA_TEST_ADMISSION_FAILURE")} == "1";
        admission_state = std::make_shared<aa::test::usb_aoa::LinkState>();
        runtime_connects.store(0);
        runtime_ciphers.store(0);
        shutdown_observation = nullptr;
        g_running.store(true);
        const aa::trust::WiredIdentity second{
            "04e8", "6860", "SECOND-PRIVATE-USB", "SAMSUNG", "SAMSUNG_Android",
            "SECOND-PRIVATE-AOA", "1-2.2"};
        const auto label = aa::trust::redacted_label(aa::trust::TransportIdentity{second});
        std::vector<std::string> args{"receiver", "--port", "1-2.2", "--no-bus", "--store",
            (root / "approved.json").string(), "--video-socket", (root / "video").string(),
            "--input-socket", (root / "input").string(), "--duration-ms", "100"};
        std::vector<char *> argv;
        argv.reserve(args.size());
        for (auto &arg : args) {
            argv.push_back(arg.data());
        }
        const int code = wired_receiver_main(static_cast<int>(argv.size()), argv.data());
        {
            std::ofstream metrics(root / "metrics");
            metrics << runtime_connects.load() << ' ' << runtime_ciphers.load() << ' '
                    << admission_state->switches() << ' ' << admission_state->claims() << ' ' << label;
        }
        std::fflush(stdout);
        ::_exit(code);
    }
    AdmissionResult result;
    char directory[] = "/tmp/wired-admission-XXXXXX";
    const char *created = ::mkdtemp(directory);
    EXPECT_NE(created, nullptr);
    if (!created) {
        result.status = -1;
        return result;
    }
    const std::filesystem::path root{created};
    const auto store_path = root / "approved.json";
    aa::trust::ApprovedPhoneStore initial{store_path};
    EXPECT_TRUE(initial.load().has_value());
    const aa::trust::WiredIdentity first{
        "04e8", "6860", "FIRST-PRIVATE-USB", "SAMSUNG", "SAMSUNG_Android",
        "FIRST-PRIVATE-AOA", "1-2.1"};
    EXPECT_TRUE(initial.approve(aa::trust::TransportIdentity{first}).has_value());
    std::string root_env = "AA_TEST_ADMISSION_ROOT=" + root.string();
    std::string failure_env = pre_admission_failure ? "AA_TEST_ADMISSION_FAILURE=1"
                                                   : "AA_TEST_ADMISSION_FAILURE=0";
    std::vector<char *> environment;
    for (char **entry = ::environ; *entry != nullptr; ++entry) {
        environment.push_back(*entry);
    }
    environment.push_back(root_env.data());
    environment.push_back(failure_env.data());
    environment.push_back(nullptr);
    std::string executable = "/proc/self/exe";
    std::string filter = pre_admission_failure
        ? "--gtest_filter=WiredReceiver.PreAdmissionFailureEmitsNoAttemptAttribution"
        : "--gtest_filter=WiredReceiver.RejectedAttemptEmitsCapturedPseudonymAndPort";
    std::string repeat = "--gtest_repeat=1";
    char *child_argv[]{executable.data(), filter.data(), repeat.data(), nullptr};
    const int capture_fd = ::open((root / "stdout").c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    EXPECT_GE(capture_fd, 0);
    std::fflush(nullptr);
    const pid_t child = ::fork();
    EXPECT_GE(child, 0);
    if (child == 0) {
        if (capture_fd < 0 || ::dup2(capture_fd, STDOUT_FILENO) < 0) {
            ::_exit(90);
        }
        ::close(capture_fd);
        ::execve(executable.c_str(), child_argv, environment.data());
        ::_exit(91);
    }
    if (capture_fd >= 0) {
        ::close(capture_fd);
    }
    if (child > 0) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
        for (;;) {
            const auto waited = ::waitpid(child, &result.status, WNOHANG);
            if (waited == child) {
                break;
            }
            if (waited < 0 || std::chrono::steady_clock::now() >= deadline) {
                result.watchdog_fired = true;
                (void)::kill(child, SIGKILL);
                (void)::waitpid(child, &result.status, 0);
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{10});
        }
        std::ifstream capture(root / "stdout");
        result.output.assign(std::istreambuf_iterator<char>{capture}, {});
        std::ifstream metrics(root / "metrics");
        metrics >> result.connects >> result.ciphers >> result.switches >> result.claims
                >> result.expected_label;
    }
    std::filesystem::remove_all(root);
    return result;
}

void expect_admission_exit(const AdmissionResult &result, int claims) {
    ASSERT_FALSE(result.watchdog_fired) << "ADMISSION_WATCHDOG_FIRED";
    ASSERT_TRUE(WIFEXITED(result.status)) << result.status;
    EXPECT_EQ(WEXITSTATUS(result.status), 3) << result.output;
    EXPECT_EQ(result.connects, 1);
    EXPECT_EQ(result.switches, 1);
    EXPECT_EQ(result.claims, claims);
    EXPECT_EQ(result.ciphers, 0) << "rejected attempt reached TLS construction";
    EXPECT_EQ(result.output.find("\"event\":\"ready\""), std::string::npos);
    EXPECT_EQ(result.output.find("\"event\":\"active\""), std::string::npos);
    EXPECT_EQ(result.output.find("\"to_state\":\"active\""), std::string::npos);
}

std::vector<std::string> link_errors(const std::string &output) {
    std::vector<std::string> errors;
    std::istringstream lines(output);
    for (std::string line; std::getline(lines, line);) {
        if (line.find("\"detail\":\"link\"") != std::string::npos) {
            errors.push_back(std::move(line));
        }
    }
    return errors;
}

TEST(WiredReceiver, RejectedAttemptEmitsCapturedPseudonymAndPort) {
    const aa::trust::WiredIdentity second{
        "04e8", "6860", "SECOND-PRIVATE-USB", "SAMSUNG", "SAMSUNG_Android",
        "SECOND-PRIVATE-AOA", "1-2.2"};
    const auto result = run_admission_child(false);
    const auto &label = result.expected_label;
    ASSERT_FALSE(label.empty());
    expect_admission_exit(result, 1);
    const auto errors = link_errors(result.output);
    ASSERT_EQ(errors.size(), 1U) << result.output;
    EXPECT_NE(errors[0].find("\"phone_id\":\"" + label + "\""), std::string::npos)
        << "CAPTURED_PSEUDONYM_MISSING: " << errors[0];
    EXPECT_NE(errors[0].find("\"port\":\"1-2.2\""), std::string::npos)
        << "SECOND_PORT_MISSING: " << errors[0];
    EXPECT_EQ(errors[0], "{\"event\":\"err\",\"detail\":\"link\",\"code\":\"transport_unauthorized_peer\","
                         "\"phone_id\":\"" + label + "\",\"port\":\"1-2.2\"}");
    const auto key = aa::trust::validated_key(aa::trust::TransportIdentity{second});
    ASSERT_TRUE(key.has_value());
    for (const auto &raw : {second.usb_serial, second.aoa_serial, second.aoa_manufacturer,
                            second.aoa_model, key.value(), std::string{"FIRST-PRIVATE"}}) {
        EXPECT_EQ(result.output.find(raw), std::string::npos) << "raw identity leaked: " << raw;
    }
}

TEST(WiredReceiver, PreAdmissionFailureEmitsNoAttemptAttribution) {
    const auto result = run_admission_child(true);
    expect_admission_exit(result, 0);
    const auto errors = link_errors(result.output);
    ASSERT_EQ(errors.size(), 1U) << result.output;
    EXPECT_EQ(errors[0], "{\"event\":\"err\",\"detail\":\"link\",\"code\":\"transport_unavailable\"}");
    EXPECT_EQ(errors[0].find("phone_id"), std::string::npos);
    EXPECT_EQ(errors[0].find("\"port\""), std::string::npos);
}

class RejectAudio final : public aa::audio::detail::ObservedAudioSink {
  public:
    aa::core::Result<void> write(const aa::audio::MixedChunk &) override {
        return aa::Error{aa::ErrorCode::transport_io};
    }
    aa::core::Result<void> drain(aa::core::Milliseconds) override { return {}; }
    aa::audio::SinkProbeStats probe_stats() const override { return {}; }
    aa::audio::RenderBarrierStats render_stats() const override { return {}; }
};

TEST(WiredReceiver, FailedAudioPumpDoesNotEmitDelivery) {
    auto router = aa::audio::detail::open_router_with_sink_for_test(
        wiredtest::pcm_router_config(), std::make_unique<RejectAudio>());
    ASSERT_TRUE(router.has_value());
    RouterSink sink{router.value()};
    const auto pcm = wiredtest::pcm_unit();
    aa::protocol::MessageView message{
        {aa::protocol::ChannelRole::media_audio, 1, {}}, pcm};
    testing::internal::CaptureStdout();
    const auto result = sink.on_message(message);
    const auto output = testing::internal::GetCapturedStdout();
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(output.find("audio_chunks"), std::string::npos)
        << "failed pump emitted delivery evidence: " << output;
}

struct RuntimeResult {
    int code;
    std::string output;
    std::chrono::steady_clock::duration elapsed;
};

RuntimeResult run_runtime(bool recovers) {
    char directory[] = "/tmp/wired-runtime-XXXXXX";
    const std::filesystem::path root{::mkdtemp(directory)};
    const auto path = root / "approved.json";
    aa::trust::ApprovedPhoneStore store{path};
    EXPECT_TRUE(store.load().has_value());
    const aa::trust::WiredIdentity identity{
        "04e8", "6860", "SYNTHETIC", "TEST", "TEST", "SYNTHETIC", "1-2.1"};
    EXPECT_TRUE(store.approve(aa::trust::TransportIdentity{identity}).has_value());
    runtime_phone = aa::app::wired_phone_identity(identity).value();
    runtime_recovers = recovers;
    runtime_quiet = false;
    runtime_connects.store(0);
    runtime_ciphers.store(0);
    g_running.store(true);
    std::vector<std::string> args{"receiver", "--port", "1-2.1", "--no-bus", "--store",
                                  path.string(), "--video-socket", (root / "video").string(),
                                  "--input-socket", (root / "input").string(), "--duration-ms",
                                  recovers ? "1000" : "6500"};
    std::vector<char *> argv;
    argv.reserve(args.size());
    for (auto &arg : args) {
        argv.push_back(arg.data());
    }
    testing::internal::CaptureStdout();
    const auto began = std::chrono::steady_clock::now();
    const int code = wired_receiver_main(static_cast<int>(argv.size()), argv.data());
    const auto elapsed = std::chrono::steady_clock::now() - began;
    auto output = testing::internal::GetCapturedStdout();
    std::filesystem::remove_all(root);
    return {code, std::move(output), elapsed};
}

TEST(WiredReceiver, TransientLinkLossRetriesUntilFreshSessionRecovers) {
    const auto result = run_runtime(true);
    EXPECT_EQ(result.code, 0) << result.output;
    EXPECT_GE(runtime_connects.load(), 4) << "reconnect stopped after one immediate attempt";
    EXPECT_EQ(runtime_ciphers.load(), 2);
    const auto active = result.output.find("\"to_state\":\"active\"");
    ASSERT_NE(active, std::string::npos) << result.output;
    EXPECT_NE(result.output.find("\"to_state\":\"active\"", active + 1), std::string::npos)
        << result.output;
}

TEST(WiredReceiver, PermanentLinkLossExitsNonzeroWithinWiredRecoveryBudget) {
    const auto result = run_runtime(false);
    EXPECT_EQ(result.code, 1) << "dead session returned clean success: " << result.output;
    EXPECT_GE(runtime_connects.load(), 3);
    EXPECT_EQ(runtime_ciphers.load(), 1);
    EXPECT_LT(result.elapsed, std::chrono::milliseconds{5500});
}

TEST(WiredReceiver, ActiveFramesShutdownOrdersStopAndReset) {
    namespace faults = aa::trust::test::faults;
    for (const int stimulus : {0, SIGINT, SIGTERM}) {
        SCOPED_TRACE(stimulus == 0 ? "duration" : stimulus == SIGINT ? "SIGINT" : "SIGTERM");
        char directory[] = "/tmp/wired-shutdown-runtime-XXXXXX";
        const std::filesystem::path root{::mkdtemp(directory)};
        const auto path = root / "approved.json";
        aa::trust::ApprovedPhoneStore initial{path};
        ASSERT_TRUE(initial.load().has_value());
        const aa::trust::WiredIdentity identity{
            "04e8", "6860", "SYNTHETIC", "TEST", "TEST", "SYNTHETIC", "1-2.1"};
        ASSERT_TRUE(initial.approve(aa::trust::TransportIdentity{identity}).has_value());
        const auto read_store = [&] {
            std::ifstream file(path);
            return std::string(std::istreambuf_iterator<char>{file}, {});
        };
        const auto store_before = read_store();
        runtime_phone = aa::app::wired_phone_identity(identity).value();
        runtime_quiet = false;
        runtime_recovers = false;
        runtime_connects.store(0);
        runtime_ciphers.store(0);
        runtime_closed.store(false);
        g_running.store(true);
        ShutdownObservation observed;
        shutdown_observation = &observed;
        std::atomic<int> mutations{0};
        faults::arm(faults::Fault::none);
        faults::set_observer([&] { ++mutations; });
        std::vector<std::string> args{"receiver", "--port", "1-2.1", "--store", path.string(),
            "--video-socket", (root / "video").string(), "--input-socket", (root / "input").string(),
            "--duration-ms", stimulus == 0 ? "1000" : "0"};
        std::vector<char *> argv;
        argv.reserve(args.size());
        for (auto &arg : args) {
            argv.push_back(arg.data());
        }
        testing::internal::CaptureStdout();
        const auto began = ShutdownObservation::Clock::now();
        int code = -1;
        std::thread receiver([&] {
            code = wired_receiver_main(static_cast<int>(argv.size()), argv.data());
        });
        std::atomic<bool> watchdog_fired{false};
        std::jthread watchdog([&](const std::stop_token &stop) {
            std::mutex mutex;
            std::condition_variable_any cv;
            std::unique_lock lock(mutex);
            cv.wait_for(lock, stop, std::chrono::seconds{5}, [] { return false; });
            if (!stop.stop_requested()) {
                watchdog_fired.store(true);
                g_running.store(false);
                const std::lock_guard state_lock(observed.state);
                observed.release_all();
            }
        });
        bool traffic = false;
        bool publication = false;
        bool cleanup = false;
        bool challenge = false;
        int signal_result = -1;
        {
            std::unique_lock lock(observed.state);
            traffic = observed.wait(lock, [&] { return observed.receive_held; });
            if (stimulus != 0 && traffic) {
                signal_result = ::pthread_kill(receiver.native_handle(), stimulus);
            }
            publication = observed.wait(lock, [&] { return observed.published; });
            observed.release_receive = true;
            observed.cv.notify_all();
            cleanup = observed.wait(lock, [&] { return observed.cleanup_held; });
            observed.release_main = true;
            observed.cv.notify_all();
            challenge = observed.wait(lock, [&] {
                return observed.main_attempt || observed.unprotected_stop;
            });
            observed.release_cleanup = true;
            observed.cv.notify_all();
        }
        receiver.join();
        const auto ended = ShutdownObservation::Clock::now();
        watchdog.request_stop();
        watchdog.join();
        const auto output = testing::internal::GetCapturedStdout();
        faults::set_observer({});
        faults::disarm();
        shutdown_observation = nullptr;
        EXPECT_TRUE(traffic);
        EXPECT_TRUE(publication);
        EXPECT_TRUE(cleanup);
        EXPECT_TRUE(challenge);
        EXPECT_FALSE(observed.gate_failed);
        EXPECT_FALSE(watchdog_fired.load());
        if (stimulus == 0) {
            EXPECT_GE(observed.published_at - began, std::chrono::milliseconds{1000});
        } else {
            EXPECT_EQ(signal_result, 0);
        }
        EXPECT_EQ(observed.pings, 4);
        EXPECT_EQ(observed.replies, 4);
        EXPECT_EQ(observed.final_receives, 1);
        EXPECT_EQ(observed.resets, 1);
        EXPECT_TRUE(observed.reset_ok);
        EXPECT_EQ(observed.stops, 1);
        EXPECT_EQ(observed.joins, 1);
        EXPECT_NE(observed.reset_owner, observed.dispatcher);
        EXPECT_FALSE(observed.unprotected_stop) << "MAIN_STOP_WITHOUT_SESSION_MUTEX";
        if (observed.unprotected_stop) {
            EXPECT_TRUE(observed.unprotected_during_cleanup);
        } else {
            EXPECT_TRUE(observed.stop_owned) << "MAIN_STOP_WITHOUT_SESSION_MUTEX";
            const auto position = [&](const char *name, std::thread::id thread,
                                      std::size_t after = 0) {
                for (auto i = after; i < observed.trace.size(); ++i) {
                    if (observed.trace[i].name == name && observed.trace[i].thread == thread) {
                        return i;
                    }
                }
                return observed.trace.size();
            };
            const auto reset = position("reset", observed.reset_owner);
            const auto release = position("release", observed.reset_owner, reset + 1);
            const auto acquire = position("acquire", observed.dispatcher, release + 1);
            const auto stop = position("stop", observed.dispatcher, acquire + 1);
            const auto unlock = position("release", observed.dispatcher, stop + 1);
            const auto join = position("join", observed.dispatcher, unlock + 1);
            EXPECT_LT(reset, release);
            EXPECT_LT(release, acquire);
            EXPECT_LT(acquire, stop);
            EXPECT_LT(stop, unlock);
            EXPECT_LT(unlock, join);
            EXPECT_LT(join, observed.trace.size());
        }
        EXPECT_FALSE(observed.join_owned);
        EXPECT_LT(ended - observed.published_at, std::chrono::seconds{3});
        EXPECT_EQ(code, 0) << output;
        EXPECT_EQ(runtime_connects.load(), 1);
        EXPECT_EQ(runtime_ciphers.load(), 1);
        EXPECT_TRUE(runtime_closed.load());
        EXPECT_EQ(observed.post_close_io, 0);
        EXPECT_EQ(mutations.load(), 0);
        EXPECT_EQ(read_store(), store_before);
        aa::trust::ApprovedPhoneStore persisted{path};
        EXPECT_TRUE(persisted.load().has_value());
        EXPECT_EQ(persisted.lookup(runtime_phone), aa::trust::Decision::approved);
        const auto active = output.find("\"to_state\":\"active\"");
        const auto stopping = output.find("\"to_state\":\"stopping\"", active);
        const auto disconnected = output.find("\"to_state\":\"disconnected\"", stopping);
        EXPECT_NE(active, std::string::npos) << output;
        EXPECT_NE(stopping, std::string::npos) << output;
        EXPECT_NE(disconnected, std::string::npos) << output;
        EXPECT_LT(active, stopping);
        EXPECT_LT(stopping, disconnected);
        EXPECT_EQ(output.find("\"to_state\":\"reconnecting\""), std::string::npos) << output;
        EXPECT_EQ(output.find("\"to_state\":\"failed\""), std::string::npos) << output;
        EXPECT_EQ(output.find("\"event\":\"err\""), std::string::npos) << output;
        std::printf("G1 stimulus=%s pings=%d replies=%d final_receive=%d resets=%d "
                    "stop_owned=%d join_owned=%d gate_failed=%d watchdog=%d\n",
                    stimulus == 0 ? "duration" : stimulus == SIGINT ? "SIGINT" : "SIGTERM",
                    observed.pings, observed.replies, observed.final_receives, observed.resets,
                    observed.stop_owned, observed.join_owned, observed.gate_failed,
                    watchdog_fired.load());
        std::filesystem::remove_all(root);
    }
}

TEST(WiredReceiver, ForgetPhoneUsesOwnerQueueAndDisconnectsQuietSession) {
    namespace bus = aa::ipc::dbus_adapter;
    namespace faults = aa::trust::test::faults;
    char directory[] = "/tmp/wired-revoke-runtime-XXXXXX";
    const std::filesystem::path root{::mkdtemp(directory)};
    const auto path = root / "approved.json";
    aa::trust::ApprovedPhoneStore initial{path};
    ASSERT_TRUE(initial.load().has_value());
    const aa::trust::WiredIdentity identity{
        "04e8", "6860", "SYNTHETIC", "TEST", "TEST", "SYNTHETIC", "1-2.1"};
    const auto approved = initial.approve(aa::trust::TransportIdentity{identity});
    ASSERT_TRUE(approved.has_value());
    runtime_phone = aa::app::wired_phone_identity(identity).value();
    runtime_quiet = true;
    runtime_recovers = false;
    runtime_connects.store(0);
    runtime_ciphers.store(0);
    runtime_waiting.store(false);
    runtime_closed.store(false);
    g_running.store(true);
    std::vector<std::string> args{"receiver", "--port", "1-2.1", "--store", path.string(),
        "--video-socket", (root / "video").string(), "--input-socket", (root / "input").string(),
        "--duration-ms", "5000"};
    std::vector<char*> argv;
    argv.reserve(args.size());
    for (auto& arg : args) {
        argv.push_back(arg.data());
    }
    std::vector<std::thread::id> mutations;
    faults::arm(faults::Fault::none);
    faults::set_observer([&] { mutations.push_back(std::this_thread::get_id()); });
    testing::internal::CaptureStdout();
    int code = -1;
    std::thread::id dispatcher;
    std::thread receiver([&] {
        dispatcher = std::this_thread::get_id();
        code = wired_receiver_main(static_cast<int>(argv.size()), argv.data());
    });
    std::atomic<bool> watchdog_fired{false};
    std::jthread watchdog([&](const std::stop_token& stop) {
        std::mutex mutex;
        std::condition_variable_any cv;
        std::unique_lock lock(mutex);
        cv.wait_for(lock, stop, std::chrono::seconds{3}, [] { return false; });
        if (!stop.stop_requested()) {
            watchdog_fired.store(true);
            g_running.store(false);
        }
    });
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds{2};
    while (!runtime_waiting.load() && std::chrono::steady_clock::now() < until) {
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    const bool was_quiet = runtime_waiting.load();
    auto client = bus::BusConnection::connect_session();
    aa::core::Result<bus::ParsedMessage> reply{aa::Error{aa::ErrorCode::internal}};
    bool registered = false;
    bool closed_at_reply = false;
    if (client && was_quiet) {
        const aa::ipc::BusName consumer{"org.custom.AndroidAutoReceiverTest.Consumer"};
        const aa::ipc::BusName service{"org.custom.AndroidAutoReceiver"};
        auto named = client.value().request_name(consumer);
        auto owner = client.value().get_name_owner(service);
        if (named && owner) {
            bus::MessageWriter registration;
            registration.destination(service.value).path("/org/custom/AndroidAutoReceiver")
                .interface_name("org.custom.AndroidAutoReceiver1").member("RegisterConsumer")
                .body_signature("s").put_string(consumer.value);
            auto serial = client.value().send_message(bus::MessageType::method_call, registration);
            if (serial) {
                auto result = client.value().await_reply(serial.value(), owner.value());
                registered = result && result.value().type == bus::MessageType::method_return;
            }
            if (registered) {
                bus::MessageWriter forget;
                forget.destination(service.value).path("/org/custom/AndroidAutoReceiver")
                    .interface_name("org.custom.AndroidAutoReceiver1").member("ForgetPhone")
                    .body_signature("t").put_u64(approved.value().value);
                auto sent = client.value().send_message(bus::MessageType::method_call, forget);
                if (sent) {
                    reply = client.value().await_reply(sent.value(), owner.value());
                    closed_at_reply = runtime_closed.load();
                }
            }
        }
    }
    if (!registered || !reply) {
        g_running.store(false);
    }
    receiver.join();
    watchdog.request_stop();
    watchdog.join();
    const auto output = testing::internal::GetCapturedStdout();
    faults::set_observer({});
    faults::disarm();
    EXPECT_FALSE(watchdog_fired.load()) << output;
    EXPECT_TRUE(was_quiet);
    EXPECT_TRUE(registered);
    ASSERT_TRUE(reply.has_value()) << output;
    EXPECT_EQ(reply.value().type, bus::MessageType::method_return);
    EXPECT_TRUE(closed_at_reply);
    EXPECT_EQ(code, 1) << output;
    EXPECT_EQ(runtime_connects.load(), 1);
    EXPECT_EQ(runtime_ciphers.load(), 1);
    EXPECT_NE(output.find("\"to_state\":\"failed\""), std::string::npos) << output;
    EXPECT_EQ(output.find("\"detail\":\"poll\""), std::string::npos) << output;
    ASSERT_FALSE(mutations.empty());
    {
        const std::lock_guard lock(runtime_owner_mutex);
        for (const auto thread : mutations) {
            EXPECT_EQ(thread, runtime_io_owner);
            EXPECT_NE(thread, dispatcher);
        }
    }
    aa::trust::ApprovedPhoneStore persisted{path};
    ASSERT_TRUE(persisted.load().has_value());
    EXPECT_EQ(persisted.lookup(runtime_phone), aa::trust::Decision::unknown);
    std::filesystem::remove_all(root);
    runtime_quiet = false;
}
} // namespace
