// SPDX-License-Identifier: GPL-3.0-or-later
//
// Task 30 wired end-to-end happy path (host-side, no hardware): a fake wired
// phone session reaches active projection, and every media path is exercised
// through the REAL downstream module - video crosses the task-22 AF_UNIX
// channel to a real consumer, audio routes through the real task-25 router,
// input flows to the phone over the session transport (task 26), trust admits
// the approved identity (task 27) and diagnostics emit redacted telemetry
// (task 19).

#include "harness.hpp"

#include "../session/Fakes.hpp"
#include "../video_socket/pair.hpp"

#include <aap_protobuf/service/inputsource/message/InputReport.pb.h>
#include <aap_protobuf/service/inputsource/message/KeyEvent.pb.h>

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <vector>

namespace {

namespace app = aa::app;
namespace diag = aa::diagnostics;
namespace ipc = aa::ipc;
namespace proto = aa::protocol;
namespace ss = aa::session;
namespace tr = aa::transport;

using wiredtest::CaptureSink;
using wiredtest::ScriptedTransport;

const aa::trust::PhoneIdentity kApproved{"phone-approved"};

// Forwards a routed video message across the task-22 AF_UNIX channel by handing
// it to the real producer. The observation block outlives the sink.
class ProducerVideoSink final : public proto::MessageSink {
  public:
    struct Obs final {
        std::uint64_t units{};
        std::uint64_t frame{};
    };

    ProducerVideoSink(ipc::VideoProducer &producer, std::shared_ptr<Obs> obs)
        : producer_(producer), obs_(std::move(obs)) {}

    aa::core::Result<void> on_message(const proto::MessageView &message) override {
        std::vector<std::uint8_t> payload;
        payload.reserve(message.payload.size());
        for (const std::byte byte : message.payload) {
            payload.push_back(std::to_integer<std::uint8_t>(byte));
        }
        ++obs_->units;
        ++obs_->frame;
        auto sent = producer_.send_access_unit(ipc::VideoSendRequest{
            obs_->frame, static_cast<std::uint64_t>(message.header.timestamp.count), true,
            payload});
        if (!sent) {
            return sent.error();
        }
        return {};
    }

  private:
    ipc::VideoProducer &producer_;
    std::shared_ptr<Obs> obs_;
};

TEST(WiredE2eHappy, ApprovedPhoneReachesActiveProjection) {
    ss::test::MapTrustStore trust;
    trust.set(kApproved, aa::trust::Decision::approved);
    ss::test::ManualClock clock;
    CaptureSink capture;
    diag::Diagnostics diagnostics{capture};

    // Video consumer: the real task-22 AF_UNIX channel (server+producer+consumer).
    auto video_pair = videotest::make_pair(ipc::NegotiatedLimits{});
    ASSERT_TRUE(video_pair.has_value());
    auto video_obs = std::make_shared<ProducerVideoSink::Obs>();
    ProducerVideoSink video_sink{video_pair.value().producer, video_obs};

    // Audio consumer: the real task-25 router around a recording sink.
    auto audio_obs = std::make_shared<wiredtest::AudioObs>();
    auto router = aa::audio::detail::open_router_with_sink_for_test(
        wiredtest::pcm_router_config(),
        std::make_unique<wiredtest::RecordingObservedSink>(audio_obs));
    ASSERT_TRUE(router.has_value());
    wiredtest::RouterAudioSink audio_sink{router.value()};

    ScriptedTransport transport;
    app::WiredSession session{{trust, clock, diagnostics}, app::WiredSessionConfig{}};
    ASSERT_TRUE(session.configure(wiredtest::golden_services()).has_value());
    ASSERT_TRUE(session.register_sink(proto::ChannelRole::video, video_sink).has_value());
    ASSERT_TRUE(session.register_sink(proto::ChannelRole::media_audio, audio_sink).has_value());
    auto cipher = std::make_unique<wiredtest::FakeHandshakeCipher>();
    auto *cipher_raw = cipher.get();
    session.attach_transport(&transport);
    session.attach_cipher(std::move(cipher));

    // Approved identity -> active session (admission is the TrustStore's call).
    ASSERT_TRUE(session.apply(ss::Event::start_discovery).has_value());
    ASSERT_TRUE(session.phone_discovered(kApproved).has_value());
    ASSERT_TRUE(session.apply(ss::Event::transport_ready).has_value());
    EXPECT_TRUE(transport.is_open());

    // AAP authentication: version -> SSL handshake -> auth_complete (finding F1).
    wiredtest::walk_auth(session, transport);
    ASSERT_TRUE(session.authenticated());

    // Discovery: the phone asks, the session publishes its advertised services.
    transport.push_inbound(wiredtest::discovery_request_frame());
    auto discovery = session.poll_transport();
    ASSERT_TRUE(discovery.has_value());
    EXPECT_TRUE(discovery.value());
    ASSERT_GE(transport.sent_count(), 1U);

    // Channel opens for video / media audio / input.
    for (const std::int32_t service : {99, 100, 101}) {
        transport.push_inbound(wiredtest::open_request_frame(service));
        auto opened = session.poll_transport();
        ASSERT_TRUE(opened.has_value());
        EXPECT_TRUE(opened.value());
    }

    ASSERT_TRUE(session.apply(ss::Event::negotiation_succeeded).has_value());
    EXPECT_EQ(session.state(), ss::State::active);

    // Video: the phone's frame traverses the channel to the consumer.
    transport.push_inbound(wiredtest::service_frame(99, wiredtest::kVideoIdr));
    auto video = session.poll_transport();
    ASSERT_TRUE(video.has_value());
    EXPECT_TRUE(video.value());
    EXPECT_EQ(video_obs->units, 1U);
    auto received = video_pair.value().consumer.receive(aa::core::Milliseconds{2000});
    ASSERT_TRUE(received.has_value());
    EXPECT_EQ(received.value(), ipc::ReceiveStatus::complete);
    EXPECT_EQ(video_pair.value().consumer.access_unit().payload.size(),
              wiredtest::kVideoIdr.size());

    // Audio: the phone's audio frame routes through the real router.
    transport.push_inbound(wiredtest::service_frame(100, wiredtest::pcm_unit()));
    auto audio = session.poll_transport();
    ASSERT_TRUE(audio.has_value());
    EXPECT_TRUE(audio.value());
    EXPECT_GE(audio_obs->writes, 1U);

    // Input: display events flow to the phone through the session transport.
    const auto before_input = transport.sent_count();
    ASSERT_TRUE(
        session.submit_input(ipc::InputEvent{ipc::TouchEvent{10, 20, ipc::TouchAction::down}})
            .has_value());
    ASSERT_TRUE(session.submit_input(ipc::InputEvent{ipc::KeyEvent{66, true}}).has_value());
    EXPECT_EQ(transport.sent_count(), before_input + 2);
    EXPECT_EQ(session.input_sends(), 2U);

    auto input_frame = tr::decode_frame(transport.last_sent());
    ASSERT_TRUE(input_frame.has_value());
    EXPECT_EQ(input_frame.value().header.channel, 101);
    // Post-auth input carries the negotiated cipher (finding F1): the frame is
    // encrypted and its payload is the sealed AAP InputReport (finding F2) - the
    // message id 0x8001 + body, not the local 24-byte IPC record. Last event: key 66.
    EXPECT_EQ(input_frame.value().header.encryption, tr::Encryption::encrypted);
    auto opened = cipher_raw->open(input_frame.value().payload);
    ASSERT_TRUE(opened.has_value());
    const auto &payload = opened.value();
    ASSERT_GE(payload.size(), 2U);
    EXPECT_EQ(payload[0], std::byte{0x80});
    EXPECT_EQ(payload[1], std::byte{0x01});
    aap_protobuf::service::inputsource::message::InputReport report;
    ASSERT_TRUE(report.ParseFromArray(payload.data() + 2, static_cast<int>(payload.size() - 2)));
    ASSERT_TRUE(report.has_key_event());
    ASSERT_EQ(report.key_event().keys_size(), 1);
    EXPECT_EQ(report.key_event().keys(0).keycode(), 66U);

    // Capability gate: an unadvertised keycode is refused and nothing is sent.
    auto gated = session.submit_input(ipc::InputEvent{ipc::KeyEvent{9999, true}});
    ASSERT_FALSE(gated.has_value());
    EXPECT_EQ(gated.error().code(), aa::ErrorCode::channel_unsupported_capability);
    EXPECT_EQ(transport.sent_count(), before_input + 2);

    // Diagnostics emitted along the path, redacted (no raw identity).
    EXPECT_GE(session.diagnostics_emitted(), 4U);
    EXPECT_TRUE(capture.contains("\"to_state\":\"active\""));
    EXPECT_FALSE(capture.contains("phone-approved"));
}

} // namespace
