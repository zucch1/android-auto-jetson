// SPDX-License-Identifier: GPL-3.0-or-later
#include "../../tools/wired-receiver/VideoOutlet.hpp"
#include "harness.hpp"
#include "../session/Fakes.hpp"
#include "../video_socket/pair.hpp"

#include <gtest/gtest.h>
#include <algorithm>
#include <cerrno>
#include <fcntl.h>
#include <sys/socket.h>

namespace {
namespace p = aa::protocol;
namespace tr = aa::transport;
using aa::core::Milliseconds;
using aa::wired_receiver::VideoOutlet;

TEST(WiredReceiver, ConsumerDisconnectKeepsMediaAckAndReplacementDelivery) {
    // Given an authenticated session delivering through the actual outlet/socket.
    aa::session::test::MapTrustStore trust;
    const aa::trust::PhoneIdentity phone{"synthetic-video-peer-test"};
    trust.set(phone, aa::trust::Decision::approved);
    aa::session::test::ManualClock clock;
    wiredtest::CaptureSink capture;
    aa::diagnostics::Diagnostics diagnostics{capture};
    wiredtest::ScriptedTransport transport;
    std::vector<std::string> logs;
    VideoOutlet outlet{[&](const std::string& line) { logs.push_back(line); }};
    auto pair = videotest::make_pair({});
    ASSERT_TRUE(pair);
    const int producer_fd = pair.value().producer.native_handle();
    outlet.install(std::move(pair.value().producer));
    aa::app::WiredSession session{{trust, clock, diagnostics}};
    ASSERT_TRUE(session.configure(wiredtest::golden_services()));
    ASSERT_TRUE(session.register_av_sink(p::ChannelRole::video, outlet));
    session.attach_transport(&transport);
    auto owned = std::make_unique<wiredtest::FakeHandshakeCipher>();
    auto* cipher = owned.get();
    session.attach_cipher(std::move(owned));
    ASSERT_TRUE(session.apply(aa::session::Event::start_discovery));
    ASSERT_TRUE(session.phone_discovered(phone));
    ASSERT_TRUE(session.apply(aa::session::Event::transport_ready));
    wiredtest::walk_auth(session, transport);
    ASSERT_TRUE(session.authenticated());
    ASSERT_TRUE(session.announce_services());
    const auto receive = [&](std::vector<std::byte> payload) -> aa::core::Result<bool> {
        auto frames = tr::encode_message({99, tr::MessageKind::specific, std::move(payload)},
                                         tr::Encryption::encrypted, cipher);
        if (!frames) { return frames.error(); }
        for (auto& frame : frames.value()) { transport.push_inbound(std::move(frame)); }
        return session.poll_transport();
    };
    ASSERT_TRUE(receive(p::encode(p::AvSinkSetup{p::AvSinkCodec::h264}).value()));
    ASSERT_TRUE(receive(p::encode(p::AvSinkStart{-7, 0}).value()));
    auto data = std::vector<std::byte>(10, std::byte{0});
    data.insert(data.end(), wiredtest::kVideoIdr.begin(), wiredtest::kVideoIdr.end());
    ASSERT_TRUE(receive(data));
    ASSERT_TRUE(pair.value().consumer.receive(Milliseconds{500}));
    EXPECT_EQ(pair.value().consumer.access_unit().frame, 1U);
    ASSERT_TRUE(session.apply(aa::session::Event::negotiation_succeeded));
    ASSERT_EQ(session.state(), aa::session::State::active);
    const auto channel_opens = session.channel_opens();
    const auto auth_completes = session.auth_completes();

    // When the local consumer dies, not the phone link.
    pair.value().consumer.close();
    const auto before = transport.sent_count();
    const auto disconnected = receive(data);

    // Then the failed unit is ACKed, the dead producer is retired, and no relink occurs.
    ASSERT_TRUE(disconnected) << aa::to_string(disconnected.error().code());
    EXPECT_EQ(transport.sent_count(), before + 1);
    const auto frame = tr::decode_frame(transport.last_sent());
    ASSERT_TRUE(frame);
    EXPECT_EQ(frame.value().header.encryption, tr::Encryption::encrypted);
    const auto ack = cipher->open(frame.value().payload);
    ASSERT_TRUE(ack);
    EXPECT_EQ(ack.value(), p::encode_av_sink_ack(-7).value());
    errno = 0;
    EXPECT_EQ(::fcntl(producer_fd, F_GETFD), -1);
    EXPECT_EQ(errno, EBADF);
    ASSERT_TRUE(receive(data)); // No consumer: still ACK, no attempt on dead producer.
    EXPECT_EQ(transport.sent_count(), before + 2);

    auto replacement = videotest::make_pair({});
    ASSERT_TRUE(replacement);
    outlet.install(std::move(replacement.value().producer));
    auto non_idr = data;
    non_idr[14] = std::byte{0x41};
    ASSERT_TRUE(receive(non_idr));
    const auto waiting = replacement.value().consumer.receive(Milliseconds{0});
    ASSERT_FALSE(waiting);
    EXPECT_EQ(waiting.error().code(), aa::ErrorCode::timeout);
    for (std::uint64_t delivered = 2; delivered <= 31; ++delivered) {
        clock.set(aa::core::Nanoseconds{static_cast<std::int64_t>(delivered * 100)});
        ASSERT_TRUE(receive(delivered == 2 ? data : non_idr));
        ASSERT_TRUE(replacement.value().consumer.receive(Milliseconds{500}));
        const auto& unit = replacement.value().consumer.access_unit();
        EXPECT_EQ(unit.frame, delivered + 2); // Failed send + waiting-IDR consumed frame IDs.
        EXPECT_EQ(unit.timestamp_ns, delivered * 100);
        EXPECT_EQ(unit.idr, delivered == 2);
    }
    EXPECT_EQ(transport.sent_count(), before + 33);
    EXPECT_TRUE(session.authenticated());
    EXPECT_EQ(session.state(), aa::session::State::active);
    EXPECT_TRUE(cipher->is_active());
    EXPECT_EQ(session.auth_completes(), auth_completes);
    EXPECT_EQ(session.channel_opens(), channel_opens);
    EXPECT_EQ(transport.close_calls(), 0);
    EXPECT_FALSE(session.failure());
    EXPECT_FALSE(capture.contains("reconnecting"));
    EXPECT_NE(std::find(logs.begin(), logs.end(), "{\"event\":\"video_sent\",\"units\":31}"), logs.end());
    outlet.close(); // Flush cumulative drops, including disconnect, absence and waiting IDR.
    EXPECT_EQ(logs.back(), "{\"event\":\"decode_stats\",\"frame_drops\":3}");
}

TEST(WiredReceiver, VideoOutletUnrelatedSocketIoStillPropagates) {
    // Given a connected peer but a locally disabled producer write side.
    auto pair = videotest::make_pair({});
    ASSERT_TRUE(pair);
    ASSERT_EQ(::shutdown(pair.value().producer.native_handle(), SHUT_WR), 0);
    VideoOutlet outlet{[](const std::string&) {}};
    outlet.install(std::move(pair.value().producer));
    // When a real socket send fails without a peer hangup.
    const auto result = outlet.on_message({{p::ChannelRole::video, 1, {}}, wiredtest::kVideoIdr});
    // Then it is not treated as consumer loss.
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code(), aa::ErrorCode::transport_io);
}

TEST(WiredReceiver, MalformedVideoStillPropagatesWithoutAck) {
    // Given the actual AV boundary, with an outlet but malformed media framing.
    VideoOutlet outlet{[](const std::string&) {}};
    std::size_t replies{};
    aa::channels::AvSink sink{wiredtest::golden_services().front(), outlet,
        [&](const std::vector<std::byte>&) -> aa::core::Result<void> { ++replies; return {}; }};
    const auto setup = p::encode(p::AvSinkSetup{p::AvSinkCodec::h264}).value();
    const auto start = p::encode(p::AvSinkStart{1, 0}).value();
    ASSERT_TRUE(sink.on_message({{p::ChannelRole::video, 1, {}}, setup}));
    ASSERT_TRUE(sink.on_message({{p::ChannelRole::video, 2, {}}, start}));
    const auto before = replies;
    const std::vector<std::byte> malformed{std::byte{0}, std::byte{0}};
    // When the data header is truncated.
    const auto result = sink.on_message({{p::ChannelRole::video, 3, {}}, malformed});
    // Then protocol errors still reach the caller, without granting media credit.
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code(), aa::ErrorCode::protocol_malformed_message);
    EXPECT_EQ(replies, before);
}
} // namespace
