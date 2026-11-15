// SPDX-License-Identifier: GPL-3.0-or-later
#include "../../tools/wired-receiver/VideoOutlet.hpp"
#include "harness.hpp"
#include "../session/Fakes.hpp"
#include "../video_socket/pair.hpp"

#include <gtest/gtest.h>
#include <aa/ipc/VideoSocket.hpp>
#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>

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

TEST(WiredReceiver, VideoOutletRoutesPeriodicRecordsToMediaLogOnly) {
    // Given an outlet with a separate media (rate-limited) log, and no consumer.
    std::vector<std::string> lifecycle;
    std::vector<std::pair<std::string, std::string>> media;
    VideoOutlet outlet{[&](const std::string& line) { lifecycle.push_back(line); },
                       [&](std::string_view kind, const std::string& line) {
                           media.emplace_back(std::string{kind}, line);
                       }};
    // When the phone's video is dropped (audit L13: phone-driven periodic records).
    ASSERT_TRUE(outlet.on_message({{p::ChannelRole::video, 1, {}}, wiredtest::kVideoIdr}));
    // Then they reach the media log by kind and the lifecycle log stays silent.
    EXPECT_TRUE(lifecycle.empty());
    ASSERT_EQ(media.size(), 2U);
    EXPECT_EQ(media[0].first, "video_dropped");
    EXPECT_EQ(media[1].first, "decode_stats");
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

int connect_raw_video_client(const std::string &path) {
    const int fd = ::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        return -1;
    }
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
    if (::connect(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0) {
        ::close(fd);
        return -1;
    }
    return fd;
}

void retire_video_runtime_dir(const std::string &runtime_dir) {
    const auto path = aa::ipc::video_socket_path_in(runtime_dir);
    videotest::remove_startup_lock(path.value);
    (void)::rmdir((runtime_dir + "/android-auto-receiver").c_str());
    (void)::rmdir(runtime_dir.c_str());
}

TEST(WiredReceiver, AcceptConsumerAdmitsLimitsRecordDelayedWithinBound) {
    const std::string runtime_dir = videotest::private_runtime_dir();
    ASSERT_FALSE(runtime_dir.empty());
    const auto path = aa::ipc::video_socket_path_in(runtime_dir);
    auto server = aa::ipc::VideoServer::listen(path, {{}, aa::ipc::current_uid()});
    ASSERT_TRUE(server.has_value()) << aa::to_string(server.error().code());
    const int fd = connect_raw_video_client(path.value);
    ASSERT_GE(fd, 0);
    aa::core::Result<aa::ipc::VideoProducer> accepted{aa::Error{aa::ErrorCode::internal}};
    std::thread acceptor([&] { accepted = server.value().accept_consumer(Milliseconds{1500}); });
    std::this_thread::sleep_for(std::chrono::milliseconds{150});
    std::array<std::uint8_t, aa::ipc::kLimitsRecordBytes> record{};
    aa::ipc::encode_limits_record(record, {});
    ASSERT_EQ(::send(fd, record.data(), record.size(), MSG_NOSIGNAL),
              static_cast<ssize_t>(record.size()));
    std::array<std::uint8_t, aa::ipc::kLimitsRecordBytes> peer{};
    (void)::recv(fd, peer.data(), peer.size(), 0);
    acceptor.join();
    ASSERT_TRUE(accepted.has_value())
        << "delayed limits record inside the positive bound was not admitted: "
        << aa::to_string(accepted.error().code());
    accepted.value().close();
    ::close(fd);
    server.value().close();
    retire_video_runtime_dir(runtime_dir);
}

TEST(WiredReceiver, AcceptConsumerTimesOutWhenLimitsRecordAbsent) {
    const std::string runtime_dir = videotest::private_runtime_dir();
    ASSERT_FALSE(runtime_dir.empty());
    const auto path = aa::ipc::video_socket_path_in(runtime_dir);
    auto server = aa::ipc::VideoServer::listen(path, {{}, aa::ipc::current_uid()});
    ASSERT_TRUE(server.has_value()) << aa::to_string(server.error().code());
    const int fd = connect_raw_video_client(path.value);
    ASSERT_GE(fd, 0);
    aa::core::Result<aa::ipc::VideoProducer> accepted{aa::Error{aa::ErrorCode::internal}};
    std::thread acceptor([&] { accepted = server.value().accept_consumer(Milliseconds{300}); });
    acceptor.join();
    ::close(fd);
    ASSERT_FALSE(accepted.has_value()) << "absent limits record still produced a producer";
    EXPECT_EQ(accepted.error().code(), aa::ErrorCode::timeout);
    server.value().close();
    retire_video_runtime_dir(runtime_dir);
}
} // namespace

TEST(WiredReceiver, CodecSpecificRandomAccessDetection) {
    using aa::wired_receiver::is_idr_au;
    EXPECT_TRUE(is_idr_au({0,0,1,0x65,1},p::VideoCodec::h264));
    EXPECT_TRUE(is_idr_au({0,0,1,0x26,1},p::VideoCodec::h265));
    EXPECT_TRUE(is_idr_au({0x82,0x49,0x83,0x42},p::VideoCodec::vp9));
    EXPECT_FALSE(is_idr_au({0x86,0x49,0x83,0x42},p::VideoCodec::vp9));
    EXPECT_FALSE(is_idr_au({0x82,0x49},p::VideoCodec::vp9));
    bool reduced = false;
    EXPECT_TRUE(is_idr_au({0x0a,1,0,0x32,1,0},p::VideoCodec::av1,&reduced));
    EXPECT_FALSE(is_idr_au({0x32,1,0x20},p::VideoCodec::av1,&reduced));
    EXPECT_FALSE(is_idr_au({0x32,1,0x80},p::VideoCodec::av1,&reduced));
    EXPECT_FALSE(is_idr_au({0x32,127,0},p::VideoCodec::av1,&reduced));
    EXPECT_TRUE(is_idr_au({0x0a,1,8,0x32,1,0x80},p::VideoCodec::av1,&reduced));
}

#include <aa/diagnostics/SafeLogSink.hpp>

TEST(WiredReceiver, VideoOutletLogsAtNegotiatedSixtyFpsCadence) {
    // Given: an outlet without a consumer receiving 60 fps metadata.
    std::vector<std::string> drop_logs;
    VideoOutlet drop_outlet{[&](const std::string& line) { drop_logs.push_back(line); }};
    p::MessageHeader header60{p::ChannelRole::video, 1, {100}};
    header60.video_profile = p::VideoProfile{1920, 1080, 60};
    for (int i = 0; i < 62; ++i) {
        ASSERT_TRUE(drop_outlet.on_message({header60, wiredtest::kVideoIdr}));
    }
    EXPECT_NE(std::find(drop_logs.begin(), drop_logs.end(),
                        "{\"event\":\"video_dropped\",\"reason\":\"no_consumer\",\"count\":1}"),
              drop_logs.end());
    EXPECT_EQ(std::find(drop_logs.begin(), drop_logs.end(),
                        "{\"event\":\"video_dropped\",\"reason\":\"no_consumer\",\"count\":31}"),
              drop_logs.end());
    EXPECT_NE(std::find(drop_logs.begin(), drop_logs.end(),
                        "{\"event\":\"video_dropped\",\"reason\":\"no_consumer\",\"count\":61}"),
              drop_logs.end());

    // And: an outlet with a consumer sending 60 fps frames emits video_sent every 60 frames.
    auto pair = videotest::make_pair({});
    ASSERT_TRUE(pair);
    std::vector<std::string> sent_logs;
    VideoOutlet send_outlet{[&](const std::string& line) { sent_logs.push_back(line); }};
    send_outlet.install(std::move(pair.value().producer));
    for (int i = 1; i <= 62; ++i) {
        header60.sequence = static_cast<std::uint64_t>(i);
        header60.timestamp = aa::core::Nanoseconds{i * 100};
        ASSERT_TRUE(send_outlet.on_message({header60, wiredtest::kVideoIdr}));
        ASSERT_TRUE(pair.value().consumer.receive(Milliseconds{500}));
    }
    EXPECT_NE(std::find(sent_logs.begin(), sent_logs.end(), "{\"event\":\"video_sent\",\"units\":1}"),
              sent_logs.end());
    EXPECT_EQ(std::find(sent_logs.begin(), sent_logs.end(), "{\"event\":\"video_sent\",\"units\":31}"),
              sent_logs.end());
    EXPECT_NE(std::find(sent_logs.begin(), sent_logs.end(), "{\"event\":\"video_sent\",\"units\":61}"),
              sent_logs.end());
}

TEST(WiredReceiver, SessionNegotiatingEachCodecLogsWithoutRedactionOrRejection) {
    struct LogCapture final : aa::core::LogSink {
        std::string message;
        std::vector<aa::core::LogField> fields;
        void write(const aa::core::LogEvent& event) override {
            message = event.message;
            fields = event.fields;
        }
    };
    for (const auto codec : p::kVideoCodecs) {
        SCOPED_TRACE(p::codec_name(codec));
        aa::session::test::MapTrustStore trust;
        const aa::trust::PhoneIdentity phone{"codec-log-test-phone"};
        trust.set(phone, aa::trust::Decision::approved);
        aa::session::test::ManualClock clock;
        wiredtest::CaptureSink capture;
        aa::diagnostics::Diagnostics diagnostics{capture};
        LogCapture log_capture;
        aa::diagnostics::SafeLogSink safe_log{log_capture};
        wiredtest::ScriptedTransport transport;
        std::vector<std::string> outlet_logs;
        VideoOutlet outlet{[&](const std::string& line) { outlet_logs.push_back(line); }};
        auto pair = videotest::make_pair({});
        ASSERT_TRUE(pair);
        outlet.install(std::move(pair.value().producer));

        auto services = wiredtest::golden_services();
        std::get<p::VideoConfiguration>(services.front().configuration).profiles = {
            {1280, 720, 30, codec}};
        aa::app::WiredSession session{{trust, clock, diagnostics}};
        ASSERT_TRUE(session.configure(services));
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
        ASSERT_TRUE(receive(p::encode(p::AvSinkSetup{static_cast<p::AvSinkCodec>(codec)}).value()));
        ASSERT_TRUE(receive(p::encode(p::AvSinkStart{1, 0}).value()));
        const std::vector<std::byte> idr = [&] {
            std::vector<std::byte> bytes(10, std::byte{0});
            switch (codec) {
            case p::VideoCodec::h264:
                bytes.insert(bytes.end(), wiredtest::kVideoIdr.begin(), wiredtest::kVideoIdr.end());
                break;
            case p::VideoCodec::h265:
                bytes.insert(bytes.end(), {std::byte{0}, std::byte{0}, std::byte{1}, std::byte{0x26}, std::byte{1}});
                break;
            case p::VideoCodec::vp9:
                bytes.insert(bytes.end(), {std::byte{0x82}, std::byte{0x49}, std::byte{0x83}, std::byte{0x42}});
                break;
            case p::VideoCodec::av1:
                bytes.insert(bytes.end(), {std::byte{0x0a}, std::byte{1}, std::byte{0}, std::byte{0x32}, std::byte{1}, std::byte{0}});
                break;
            }
            return bytes;
        }();
        ASSERT_TRUE(receive(idr));
        ASSERT_TRUE(pair.value().consumer.receive(Milliseconds{500}));
        const auto negotiated = pair.value().consumer.access_unit().profile;
        ASSERT_TRUE(negotiated);
        EXPECT_EQ(negotiated->codec, codec);
        const std::string codec_str{p::codec_name(negotiated->codec)};

        aa::diagnostics::Event event{};
        event.kind = aa::diagnostics::EventKind::capability_negotiated;
        event.fields.push_back(aa::diagnostics::EventField::text("codec", codec_str));
        diagnostics.emit(event);
        EXPECT_TRUE(capture.contains(std::string{"\"codec\":\""} + codec_str + "\""));
        EXPECT_FALSE(capture.contains("[redacted]"));

        safe_log.write({aa::core::LogLevel::info, "capability_negotiated", {{"codec", codec_str}}});
        EXPECT_EQ(log_capture.message, "capability_negotiated");
        ASSERT_EQ(log_capture.fields.size(), 1u);
        EXPECT_EQ(log_capture.fields[0].value, codec_str);
        EXPECT_NE(std::find(outlet_logs.begin(), outlet_logs.end(),
                            "{\"event\":\"video_profile\",\"width\":1280,\"height\":720,\"fps\":30,\"codec\":\""
                                + codec_str + "\"}"),
                  outlet_logs.end());
    }
}
