// SPDX-License-Identifier: GPL-3.0-or-later
#include "harness.hpp"
#include "../session/Fakes.hpp"
#include <aa/channels/MicrophoneSink.hpp>
#include <aa/protocol/Microphone.hpp>

#include <aap_protobuf/service/media/source/message/MicrophoneResponse.pb.h>
#include <gtest/gtest.h>

namespace {
namespace proto = aa::protocol;
namespace tr = aa::transport;
namespace ss = aa::session;

TEST(WiredMicrophone, RefusedCaptureRepliesEncryptedAndPingSurvivesThenLinkResetClosesRoute) {
    ss::test::MapTrustStore trust;
    ss::test::ManualClock clock;
    wiredtest::CaptureSink capture;
    aa::diagnostics::Diagnostics diagnostics{capture};
    wiredtest::ScriptedTransport transport;
    aa::app::WiredSession session{{trust, clock, diagnostics}};
    aa::channels::MicrophoneSink sink{[&](std::vector<std::byte> bytes) {
        return session.send_channel_payload(proto::ChannelRole::microphone, std::move(bytes));
    }};
    ASSERT_TRUE(session.configure({{proto::ServiceKey{106}, proto::ChannelRole::microphone,
                                   proto::MicrophoneConfiguration{{16000, 16, 1}}}}));
    ASSERT_TRUE(session.register_sink(proto::ChannelRole::microphone, sink));
    const aa::trust::PhoneIdentity phone{"microphone-test"};
    trust.set(phone, aa::trust::Decision::approved);
    session.attach_transport(&transport);
    auto owned = std::make_unique<wiredtest::FakeHandshakeCipher>();
    auto* cipher = owned.get();
    session.attach_cipher(std::move(owned));
    ASSERT_TRUE(session.apply(ss::Event::start_discovery));
    ASSERT_TRUE(session.phone_discovered(phone));
    ASSERT_TRUE(session.apply(ss::Event::transport_ready));
    EXPECT_FALSE(session.send_channel_payload(proto::ChannelRole::microphone,
                                              proto::encode_microphone_ready().value()));
    wiredtest::walk_auth(session, transport);
    ASSERT_TRUE(session.authenticated());
    auto receive = [&](std::uint8_t channel, std::vector<std::byte> bytes) {
        auto frames = tr::encode_message({channel, tr::MessageKind::specific, std::move(bytes)},
                                          tr::Encryption::encrypted, cipher);
        EXPECT_TRUE(frames);
        for (auto& frame : frames.value()) { transport.push_inbound(std::move(frame)); }
        return session.poll_transport();
    };
    ASSERT_TRUE(session.announce_services());
    ASSERT_TRUE(receive(0, proto::encode(proto::ChannelOpenRequest{1, proto::ServiceKey{106}}).value()));
    ASSERT_EQ(session.channel_opens(), 1u);
    ASSERT_TRUE(receive(106, proto::encode(proto::MicrophoneSetup{}).value()));
    const auto before = transport.sent_count();
    ASSERT_TRUE(receive(106, proto::encode(proto::MicrophoneRequest{true}).value()));
    ASSERT_EQ(transport.sent_count(), before + 1);
    const auto response_frame = tr::decode_frame(transport.last_sent());
    ASSERT_TRUE(response_frame);
    EXPECT_EQ(response_frame.value().header.channel, 106);
    EXPECT_EQ(response_frame.value().header.encryption, tr::Encryption::encrypted);
    const auto response = cipher->open(response_frame.value().payload);
    ASSERT_TRUE(response);
    ASSERT_GE(response.value().size(), 2u);
    EXPECT_EQ(response.value()[0], std::byte{0x80});
    EXPECT_EQ(response.value()[1], std::byte{6});
    aap_protobuf::service::media::source::message::MicrophoneResponse wire;
    ASSERT_TRUE(wire.ParseFromArray(response.value().data() + 2,
                                   static_cast<int>(response.value().size() - 2)));
    EXPECT_EQ(wire.status(), -250);
    EXPECT_FALSE(wire.has_session_id());
    ASSERT_TRUE(receive(0, proto::encode(proto::PingRequest{12345, false, {}}).value()));
    ASSERT_EQ(transport.sent_count(), before + 2);
    const auto ping_frame = tr::decode_frame(transport.last_sent());
    ASSERT_TRUE(ping_frame);
    EXPECT_EQ(ping_frame.value().header.channel, 0);
    const auto ping_payload = ping_frame.value().header.encryption == tr::Encryption::encrypted
        ? cipher->open(ping_frame.value().payload)
        : aa::core::Result<std::vector<std::byte>>{ping_frame.value().payload};
    ASSERT_TRUE(ping_payload);
    const auto ping = proto::decode_ping_response(ping_payload.value());
    ASSERT_TRUE(ping);
    EXPECT_EQ(ping.value().timestamp, 12345);
    EXPECT_EQ(session.pings_answered(), 1u);
    EXPECT_FALSE(session.failure());
    EXPECT_EQ(session.state(), ss::State::negotiating);
    session.attach_transport(&transport);
    EXPECT_FALSE(session.authenticated());
    EXPECT_FALSE(session.send_channel_payload(proto::ChannelRole::microphone,
                                              proto::encode_microphone_ready().value()));
}
} // namespace
