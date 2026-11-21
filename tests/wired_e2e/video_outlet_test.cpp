// SPDX-License-Identifier: GPL-3.0-or-later
#include "../../tools/wired-receiver/VideoOutlet.hpp"
#include "harness.hpp"
#include "../session/Fakes.hpp"
#include "../video_socket/pair.hpp"

#include <gtest/gtest.h>
#include <aa/ipc/VideoSocket.hpp>
#include <algorithm>
#include <array>
#include <map>
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

TEST(WiredReceiver, VideoOutletFinalDecodeStatsReachLifecycleSinkAfterAPeriodicCopy) {
    // Given the periodic decode_stats for the current count already went to the
    // lossy media sink (which a stalled stdout may have dropped).
    std::vector<std::string> lifecycle;
    std::vector<std::pair<std::string, std::string>> media;
    VideoOutlet outlet{[&](const std::string& line) { lifecycle.push_back(line); },
                       [&](std::string_view kind, const std::string& line) {
                           media.emplace_back(std::string{kind}, line);
                       }};
    ASSERT_TRUE(outlet.on_message({{p::ChannelRole::video, 1, {}}, wiredtest::kVideoIdr}));
    ASSERT_EQ(media.size(), 2U);
    ASSERT_EQ(media[1].second, "{\"event\":\"decode_stats\",\"frame_drops\":1}");
    // When the outlet closes (twice: explicit close, then the destructor's).
    outlet.close();
    outlet.close();
    // Then the final cumulative count reaches the lifecycle sink exactly once.
    ASSERT_EQ(lifecycle.size(), 1U);
    EXPECT_EQ(lifecycle.back(), "{\"event\":\"decode_stats\",\"frame_drops\":1}");
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

TEST(WiredReceiver, VideoOutletRequestsIdrOnBackpressureRespectsRateLimitAndTracksRecovery) {
    aa::session::test::MapTrustStore trust;
    const aa::trust::PhoneIdentity phone{"idr-recovery-phone"};
    trust.set(phone, aa::trust::Decision::approved);
    aa::session::test::ManualClock clock;
    wiredtest::CaptureSink capture;
    aa::diagnostics::Diagnostics diagnostics{capture};
    wiredtest::ScriptedTransport transport;
    auto steady_now = std::chrono::steady_clock::time_point{std::chrono::seconds{10}};

    std::vector<std::string> logs;
    VideoOutlet outlet{[&](const std::string& line) { logs.push_back(line); }};
    auto pair = videotest::make_pair({});
    ASSERT_TRUE(pair);
    const int producer_fd = pair.value().producer.native_handle();
    const int consumer_fd = pair.value().consumer.native_handle();
    outlet.install(std::move(pair.value().producer));

    aa::app::WiredSession session{{trust, clock, diagnostics}};
    ASSERT_TRUE(session.configure(wiredtest::golden_services()));
    ASSERT_TRUE(session.register_av_sink(p::ChannelRole::video, outlet));
    outlet.set_idr_recovery({
        [&session] { return session.video_projecting(); },
        [&session] { return session.request_video_idr(); },
        [&steady_now] { return steady_now; },
    });

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
        aa::core::Result<bool> polled{false};
        for (auto& frame : frames.value()) {
            transport.push_inbound(std::move(frame));
            polled = session.poll_transport();
            if (!polled) { return polled.error(); }
        }
        return polled;
    };
    ASSERT_TRUE(receive(p::encode(p::AvSinkSetup{p::AvSinkCodec::h264}).value()));
    ASSERT_TRUE(receive(p::encode(p::AvSinkStart{1, 0}).value()));
    ASSERT_TRUE(session.apply(aa::session::Event::negotiation_succeeded));
    ASSERT_TRUE(session.video_projecting());

    // Initial IDR establishes profile and triggers socket buffer sizing.
    auto idr_payload = std::vector<std::byte>(10, std::byte{0});
    idr_payload.insert(idr_payload.end(), wiredtest::kVideoIdr.begin(), wiredtest::kVideoIdr.end());
    ASSERT_TRUE(receive(idr_payload));
    ASSERT_TRUE(pair.value().consumer.receive(Milliseconds{500}));
    EXPECT_NE(std::find_if(logs.begin(), logs.end(), [](const std::string& line) {
        return line.find("\"event\":\"video_socket_buffer\"") != std::string::npos &&
               line.find("\"side\":\"producer\"") != std::string::npos;
    }), logs.end());

    // Shrink socket buffers so non-IDR frames deterministically trigger EAGAIN (not EMSGSIZE).
    const int tiny_buf = 32768;
    ASSERT_EQ(::setsockopt(producer_fd, SOL_SOCKET, SO_SNDBUF, &tiny_buf, sizeof(tiny_buf)), 0);
    ASSERT_EQ(::setsockopt(consumer_fd, SOL_SOCKET, SO_RCVBUF, &tiny_buf, sizeof(tiny_buf)), 0);

    auto big_non_idr = std::vector<std::byte>(10, std::byte{0});
    big_non_idr.push_back(std::byte{0x00});
    big_non_idr.push_back(std::byte{0x00});
    big_non_idr.push_back(std::byte{0x01});
    big_non_idr.push_back(std::byte{0x41});
    big_non_idr.resize(16384, std::byte{0x55});

    auto count_logs = [&](const std::string& needle) {
        return static_cast<std::size_t>(std::count_if(logs.begin(), logs.end(), [&](const std::string& s) {
            return s.find(needle) != std::string::npos;
        }));
    };

    const auto sent_before_bp = transport.sent_count();
    for (int i = 0; i < 128 && count_logs("\"event\":\"idr_requested\"") == 0; ++i) {
        const auto res = receive(big_non_idr);
        ASSERT_TRUE(res) << aa::to_string(res.error().code()) << ": " << res.error().message();
    }
    ASSERT_EQ(count_logs("\"event\":\"idr_requested\""), 1u);
    EXPECT_NE(std::find(logs.begin(), logs.end(),
                        "{\"event\":\"idr_requested\",\"source\":\"producer\",\"reason\":\"backpressure\"}"),
              logs.end());

    // Confirm the wire frame sent to the phone is VideoFocusNotification(projected).
    const auto expected_focus =
        p::encode(p::VideoFocusNotification{p::VideoFocusMode::projected, true}).value();
    bool saw_focus_notification = false;
    for (std::size_t i = sent_before_bp; i < transport.sent_count(); ++i) {
        const auto frame = tr::decode_frame(transport.sent_frames()[i]);
        ASSERT_TRUE(frame);
        if (frame.value().header.channel == 99 &&
            frame.value().header.encryption == tr::Encryption::encrypted) {
            const auto opened = cipher->open(frame.value().payload);
            ASSERT_TRUE(opened);
            if (opened.value() == expected_focus) {
                saw_focus_notification = true;
            }
        }
    }
    EXPECT_TRUE(saw_focus_notification);

    // Inside the 250 ms cooldown window: waiting_idr drops do NOT resend.
    steady_now += std::chrono::milliseconds{100};
    ASSERT_TRUE(receive(big_non_idr));
    EXPECT_EQ(count_logs("\"event\":\"idr_requested\""), 1u);

    // At +250 ms: waiting_idr drop triggers a second IDR request.
    steady_now += std::chrono::milliseconds{150};
    ASSERT_TRUE(receive(big_non_idr));
    EXPECT_EQ(count_logs("\"event\":\"idr_requested\""), 2u);
    EXPECT_NE(std::find(logs.begin(), logs.end(),
                        "{\"event\":\"idr_requested\",\"source\":\"producer\",\"reason\":\"waiting_idr\"}"),
              logs.end());

    // Drain consumer so the recovery IDR succeeds and clears waiting_idr.
    for (;;) {
        if (!pair.value().consumer.receive(Milliseconds{0})) {
            break;
        }
    }
    steady_now += std::chrono::milliseconds{150}; // Total 400 ms since begin_wait.
    ASSERT_TRUE(receive(idr_payload));
    ASSERT_TRUE(pair.value().consumer.receive(Milliseconds{500}));
    EXPECT_EQ(count_logs("\"event\":\"idr_recovered\""), 1u);
    EXPECT_NE(std::find(logs.begin(), logs.end(),
                        "{\"event\":\"idr_recovered\",\"source\":\"producer\",\"latency_ms\":400,\"episode_requests\":2,\"total_requests\":2}"),
              logs.end());
}

TEST(WiredReceiver, VideoOutletRequestsIdrWhenLateFirstConsumerMissesInitialIdr) {
    auto steady_now = std::chrono::steady_clock::time_point{std::chrono::seconds{50}};
    std::vector<std::string> logs;
    std::vector<bool> request_modes;
    VideoOutlet outlet{[&](const std::string& line) { logs.push_back(line); }};
    outlet.set_idr_recovery({
        [] { return true; },
        {},
        [&steady_now] { return steady_now; },
        [&request_modes](bool focus_transition) -> aa::core::Result<void> {
            request_modes.push_back(focus_transition);
            return {};
        },
        aa::ipc::IdrRecoveryStrategy::auto_escalate,
    });

    // Phone starts and emits initial IDR while no consumer is installed yet.
    std::vector<std::byte> idr_bytes(10, std::byte{0});
    idr_bytes.insert(idr_bytes.end(), wiredtest::kVideoIdr.begin(), wiredtest::kVideoIdr.end());
    ASSERT_TRUE(outlet.on_message({{p::ChannelRole::video, 1, {100}}, idr_bytes}));

    // First consumer installs later; units_ == 0 and had_backpressure_ == false.
    auto pair = videotest::make_pair({});
    ASSERT_TRUE(pair);
    outlet.install(std::move(pair.value().producer));

    // Phone continues with P-frames only; consumer would stay black unless IDR recovery fires.
    std::vector<std::byte> p_frame(16, std::byte{0x41});
    p_frame[0] = std::byte{0x00};
    p_frame[1] = std::byte{0x00};
    p_frame[2] = std::byte{0x01};
    p_frame[3] = std::byte{0x41};
    ASSERT_TRUE(outlet.on_message({{p::ChannelRole::video, 2, {200}}, p_frame}));

    ASSERT_EQ(request_modes.size(), 1u);
    EXPECT_FALSE(request_modes.front()); // First step is cheap nudge.
    EXPECT_NE(std::find(logs.begin(), logs.end(),
                        "{\"event\":\"idr_requested\",\"source\":\"producer\",\"reason\":\"waiting_idr\"}"),
              logs.end());

    // Subsequent recovery IDR arrives and finishes episode 1.
    steady_now += std::chrono::milliseconds{120};
    ASSERT_TRUE(outlet.on_message({{p::ChannelRole::video, 3, {300}}, idr_bytes}));
    ASSERT_TRUE(pair.value().consumer.receive(Milliseconds{500}));
    EXPECT_NE(std::find(logs.begin(), logs.end(),
                        "{\"event\":\"idr_recovered\",\"source\":\"producer\",\"latency_ms\":120,\"episode_requests\":1,\"total_requests\":1}"),
              logs.end());
}

TEST(WiredReceiver, IdrRecoveryCoordinatorHoldsCooldownAcrossEpisodesAndInterleavedDbus) {
    auto steady_now = std::chrono::steady_clock::time_point{std::chrono::seconds{10}};
    std::size_t requests = 0;
    std::vector<std::string> logs;
    VideoOutlet outlet{[&](const std::string& line) { logs.push_back(line); }};
    outlet.set_idr_recovery({
        [] { return true; },
        [&requests]() -> aa::core::Result<void> { ++requests; return {}; },
        [&steady_now] { return steady_now; },
    });

    auto pair = videotest::make_pair({});
    ASSERT_TRUE(pair);
    outlet.install(std::move(pair.value().producer));

    std::vector<std::byte> idr_bytes(10, std::byte{0});
    idr_bytes.insert(idr_bytes.end(), wiredtest::kVideoIdr.begin(), wiredtest::kVideoIdr.end());

    // Episode 1: D-Bus request triggers at t=10.000s, IDR recovers at t=10.050s.
    {
        const auto res = outlet.request_idr_external("consumer_ipc");
        ASSERT_TRUE(res);
        EXPECT_TRUE(res.value());
    }
    EXPECT_EQ(requests, 1u);
    steady_now += std::chrono::milliseconds{50};
    ASSERT_TRUE(outlet.on_message({{p::ChannelRole::video, 1, {100}}, idr_bytes}));
    ASSERT_TRUE(pair.value().consumer.receive(Milliseconds{500}));
    EXPECT_NE(std::find(logs.begin(), logs.end(),
                        "{\"event\":\"idr_recovered\",\"source\":\"producer\",\"latency_ms\":50,\"episode_requests\":1,\"total_requests\":1}"),
              logs.end());

    // Episode 2: Interleaved D-Bus RequestIdrRecovery at t=10.100s (100 ms since last request < 250 ms) is suppressed.
    steady_now += std::chrono::milliseconds{50};
    {
        const auto res = outlet.request_idr_external("consumer_ipc");
        ASSERT_TRUE(res);
        EXPECT_FALSE(res.value());
    }
    EXPECT_EQ(requests, 1u);

    // At t=10.250s (250 ms since last request), the new wait succeeds.
    steady_now += std::chrono::milliseconds{150};
    {
        const auto res = outlet.request_idr_external("consumer_ipc");
        ASSERT_TRUE(res);
        EXPECT_TRUE(res.value());
    }
    EXPECT_EQ(requests, 2u);

    steady_now += std::chrono::milliseconds{40};
    ASSERT_TRUE(outlet.on_message({{p::ChannelRole::video, 2, {200}}, idr_bytes}));
    ASSERT_TRUE(pair.value().consumer.receive(Milliseconds{500}));
    EXPECT_NE(std::find(logs.begin(), logs.end(),
                        "{\"event\":\"idr_recovered\",\"source\":\"producer\",\"latency_ms\":190,\"episode_requests\":1,\"total_requests\":2}"),
              logs.end());
}

TEST(WiredReceiver, VideoOutletEscalatesOrderAndEnforcesTransitionCooldown) {
    aa::session::test::MapTrustStore trust;
    const aa::trust::PhoneIdentity phone{"idr-escalation-phone"};
    trust.set(phone, aa::trust::Decision::approved);
    aa::session::test::ManualClock clock;
    wiredtest::CaptureSink capture;
    aa::diagnostics::Diagnostics diagnostics{capture};
    wiredtest::ScriptedTransport transport;
    auto steady_now = std::chrono::steady_clock::time_point{std::chrono::seconds{100}};

    std::vector<std::string> logs;
    VideoOutlet outlet{[&](const std::string& line) { logs.push_back(line); }};
    auto pair = videotest::make_pair({});
    ASSERT_TRUE(pair);
    outlet.install(std::move(pair.value().producer));

    aa::app::WiredSession session{{trust, clock, diagnostics}};
    ASSERT_TRUE(session.configure(wiredtest::golden_services()));
    ASSERT_TRUE(session.register_av_sink(p::ChannelRole::video, outlet));
    outlet.set_idr_recovery({
        [&session] { return session.video_projecting(); },
        [&session] { return session.request_video_idr(); },
        [&steady_now] { return steady_now; },
        [&session](bool focus_transition) { return session.request_video_idr(focus_transition); },
        aa::ipc::IdrRecoveryStrategy::auto_escalate,
    });

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
        aa::core::Result<bool> polled{false};
        for (auto& frame : frames.value()) {
            transport.push_inbound(std::move(frame));
            polled = session.poll_transport();
            if (!polled) { return polled.error(); }
        }
        return polled;
    };
    ASSERT_TRUE(receive(p::encode(p::AvSinkSetup{p::AvSinkCodec::h264}).value()));
    ASSERT_TRUE(receive(p::encode(p::AvSinkStart{1, 0}).value()));
    ASSERT_TRUE(session.apply(aa::session::Event::negotiation_succeeded));
    ASSERT_TRUE(session.video_projecting());

    const auto decode_focus_notifications_since = [&](std::size_t start_idx) {
        std::vector<p::VideoFocusMode> modes;
        const auto n_proj = p::encode(p::VideoFocusNotification{p::VideoFocusMode::projected, true}).value();
        const auto n_nat = p::encode(p::VideoFocusNotification{p::VideoFocusMode::native, true}).value();
        const auto n_no_inp = p::encode(p::VideoFocusNotification{p::VideoFocusMode::projected_no_input_focus, true}).value();
        for (std::size_t i = start_idx; i < transport.sent_count(); ++i) {
            const auto frame = tr::decode_frame(transport.sent_frames()[i]);
            if (frame && frame.value().header.channel == 99 &&
                frame.value().header.encryption == tr::Encryption::encrypted) {
                const auto opened = cipher->open(frame.value().payload);
                if (opened && opened.value() == n_proj) { modes.push_back(p::VideoFocusMode::projected); }
                else if (opened && opened.value() == n_nat) { modes.push_back(p::VideoFocusMode::native); }
                else if (opened && opened.value() == n_no_inp) { modes.push_back(p::VideoFocusMode::projected_no_input_focus); }
            }
        }
        return modes;
    };

    const std::size_t mark0 = transport.sent_count();
    {
        const auto r1 = outlet.request_idr_external("consumer_ipc"); // t=0: nudge #1
        ASSERT_TRUE(r1); EXPECT_TRUE(r1.value());
        steady_now += aa::ipc::kIdrRequestMinInterval;
        const auto r2 = outlet.request_idr_external("consumer_ipc"); // t=250ms: nudge #2
        ASSERT_TRUE(r2); EXPECT_TRUE(r2.value());
        steady_now += aa::ipc::kIdrRequestMinInterval;
        const auto r3 = outlet.request_idr_external("consumer_ipc"); // t=500ms: nudge #3
        ASSERT_TRUE(r3); EXPECT_TRUE(r3.value());
    }
    EXPECT_EQ(decode_focus_notifications_since(mark0),
              (std::vector<p::VideoFocusMode>{p::VideoFocusMode::projected,
                                              p::VideoFocusMode::projected,
                                              p::VideoFocusMode::projected}));
    EXPECT_EQ(std::count_if(logs.begin(), logs.end(), [](const std::string& s) {
        return s.find("\"event\":\"idr_escalated\"") != std::string::npos;
    }), 0);

    // t=750ms (max nudges reached): escalates to native -> projected focus transition.
    steady_now += aa::ipc::kIdrRequestMinInterval;
    const std::size_t mark_esc = transport.sent_count();
    {
        const auto res = outlet.request_idr_external("consumer_ipc");
        ASSERT_TRUE(res);
        EXPECT_TRUE(res.value());
    }
    EXPECT_EQ(decode_focus_notifications_since(mark_esc),
              (std::vector<p::VideoFocusMode>{p::VideoFocusMode::native,
                                              p::VideoFocusMode::projected}));
    EXPECT_NE(std::find(logs.begin(), logs.end(),
                        "{\"event\":\"idr_escalated\",\"source\":\"producer\",\"step\":\"focus_transition\",\"reason\":\"consumer_ipc\",\"episode_requests\":4,\"total_requests\":4}"),
              logs.end());

    // Inside kIdrTransitionCooldown (5000 ms), subsequent requests fall back to cheap nudge without repeating transition.
    steady_now += std::chrono::milliseconds{1000};
    const std::size_t mark_cool = transport.sent_count();
    {
        const auto res = outlet.request_idr_external("consumer_ipc");
        ASSERT_TRUE(res);
        EXPECT_TRUE(res.value());
    }
    EXPECT_EQ(decode_focus_notifications_since(mark_cool),
              (std::vector<p::VideoFocusMode>{p::VideoFocusMode::projected}));
    EXPECT_EQ(std::count_if(logs.begin(), logs.end(), [](const std::string& s) {
        return s.find("\"event\":\"idr_escalated\"") != std::string::npos;
    }), 1);

    // After kIdrTransitionCooldown elapses since t=750ms (+4000ms here -> +5000ms total), transition runs again.
    steady_now += std::chrono::milliseconds{4000};
    const std::size_t mark_esc2 = transport.sent_count();
    {
        const auto res = outlet.request_idr_external("consumer_ipc");
        ASSERT_TRUE(res);
        EXPECT_TRUE(res.value());
    }
    EXPECT_EQ(decode_focus_notifications_since(mark_esc2),
              (std::vector<p::VideoFocusMode>{p::VideoFocusMode::native,
                                              p::VideoFocusMode::projected}));
    EXPECT_EQ(std::count_if(logs.begin(), logs.end(), [](const std::string& s) {
        return s.find("\"event\":\"idr_escalated\"") != std::string::npos;
    }), 2);
}

TEST(WiredReceiver, VideoOutletPartialFocusRestoreFailureFailsSessionAndPreservesSnapshot) {
    for (const auto initial_mode :
         {p::VideoFocusMode::projected, p::VideoFocusMode::projected_no_input_focus}) {
        SCOPED_TRACE(initial_mode == p::VideoFocusMode::projected ? "projected" : "projected_no_input_focus");
        aa::session::test::MapTrustStore trust;
        const aa::trust::PhoneIdentity phone{"idr-restore-fail-phone"};
        trust.set(phone, aa::trust::Decision::approved);
        aa::session::test::ManualClock clock;
        wiredtest::CaptureSink capture;
        aa::diagnostics::Diagnostics diagnostics{capture};
        wiredtest::ScriptedTransport transport;
        auto steady_now = std::chrono::steady_clock::time_point{std::chrono::seconds{200}};

        std::vector<std::string> logs;
        VideoOutlet outlet{[&](const std::string& line) { logs.push_back(line); }};
        auto pair = videotest::make_pair({});
        ASSERT_TRUE(pair);
        outlet.install(std::move(pair.value().producer));

        aa::app::WiredSession session{{trust, clock, diagnostics}};
        ASSERT_TRUE(session.configure(wiredtest::golden_services()));
        ASSERT_TRUE(session.register_av_sink(p::ChannelRole::video, outlet));
        outlet.set_idr_recovery({
            [&session] { return session.video_projecting(); },
            [&session] { return session.request_video_idr(); },
            [&steady_now] { return steady_now; },
            [&session](bool focus_transition) { return session.request_video_idr(focus_transition); },
            aa::ipc::IdrRecoveryStrategy::auto_escalate,
        });

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
            aa::core::Result<bool> polled{false};
            for (auto& frame : frames.value()) {
                transport.push_inbound(std::move(frame));
                polled = session.poll_transport();
                if (!polled) { return polled.error(); }
            }
            return polled;
        };
        ASSERT_TRUE(receive(p::encode(p::AvSinkSetup{p::AvSinkCodec::h264}).value()));
        ASSERT_TRUE(receive(p::encode(p::AvSinkStart{1, 0}).value()));
        if (initial_mode == p::VideoFocusMode::projected_no_input_focus) {
            ASSERT_TRUE(receive(p::encode(p::VideoFocusRequest{initial_mode}).value()));
        }
        ASSERT_TRUE(session.apply(aa::session::Event::negotiation_succeeded));
        ASSERT_TRUE(session.video_projecting());
        EXPECT_EQ(session.state(), aa::session::State::active);

        // Escalate through 3 nudges (t=0, 250ms, 500ms).
        for (int i = 0; i < 3; ++i) {
            const auto res = outlet.request_idr_external("consumer_ipc");
            ASSERT_TRUE(res);
            EXPECT_TRUE(res.value());
            steady_now += aa::ipc::kIdrRequestMinInterval;
        }

        // First-send failure when trying focus_transition leaves session active and rolls back snapshot.
        transport.fail_send_at(transport.sent_count());
        const auto first_fail = outlet.request_idr_external("consumer_ipc");
        ASSERT_FALSE(first_fail);
        EXPECT_EQ(first_fail.error().code(), aa::ErrorCode::transport_io);
        EXPECT_EQ(session.state(), aa::session::State::active);
        EXPECT_FALSE(session.failure());
        EXPECT_TRUE(session.video_projecting());
        EXPECT_FALSE(outlet.idr_snapshot().escalated);
        EXPECT_EQ(outlet.idr_snapshot().episode_requests, 3u);
        EXPECT_FALSE(outlet.idr_snapshot().last_transition_at.has_value());

        // Failure starts retry backoff; immediate retry is throttled, then after kIdrRequestMinInterval
        // the second-send failure after native succeeds fails session and preserves snapshot.
        const auto throttled = outlet.request_idr_external("consumer_ipc");
        ASSERT_TRUE(throttled);
        EXPECT_FALSE(throttled.value());
        steady_now += aa::ipc::kIdrRequestMinInterval;
        const std::size_t before_partial = transport.sent_count();
        transport.fail_send_at(before_partial + 1); // 1st send (native) succeeds; 2nd send (restore) fails.
        const auto partial_fail = outlet.request_idr_external("consumer_ipc");
        ASSERT_FALSE(partial_fail);
        EXPECT_EQ(partial_fail.error().code(), aa::ErrorCode::channel_focus_restore_failed);

        EXPECT_EQ(session.state(), aa::session::State::failed);
        ASSERT_TRUE(session.failure().has_value());
        EXPECT_EQ(session.failure()->code(), aa::ErrorCode::channel_focus_restore_failed);
        EXPECT_TRUE(session.token().stop_requested());
        EXPECT_GE(transport.close_calls(), 1);
        EXPECT_FALSE(transport.is_open());
        EXPECT_FALSE(session.video_projecting());
        EXPECT_EQ(transport.sent_count(), before_partial + 1);
        EXPECT_TRUE(capture.contains("\"to_state\":\"failed\""));
        EXPECT_TRUE(capture.contains("\"error_code\":406"));
        EXPECT_NE(std::find(logs.begin(), logs.end(),
                            aa::ipc::idr_transition_failed_json(
                                "producer", "restore_focus", "consumer_ipc", 4, 4,
                                "channel_focus_restore_failed")),
                  logs.end());

        const auto snap = outlet.idr_snapshot();
        EXPECT_TRUE(snap.escalated);
        EXPECT_EQ(snap.episode_requests, 4u);
        EXPECT_EQ(snap.total_requests, 4u);
        EXPECT_EQ(snap.last_request_at, steady_now);
        EXPECT_EQ(snap.last_transition_at, steady_now);

        // Retrying later never replays native.
        transport.fail_send_at(std::nullopt);
        const auto sent_after = transport.sent_count();
        steady_now += aa::ipc::kIdrTransitionCooldown;
        const auto after = outlet.request_idr_external("consumer_ipc");
        ASSERT_TRUE(after);
        EXPECT_FALSE(after.value());
        EXPECT_EQ(transport.sent_count(), sent_after);
    }
}

TEST(WiredReceiver, VideoOutletOnMessagePropagatesPartialFocusRestoreFailure) {
    aa::session::test::MapTrustStore trust;
    const aa::trust::PhoneIdentity phone{"idr-msg-fail-phone"};
    trust.set(phone, aa::trust::Decision::approved);
    aa::session::test::ManualClock clock;
    wiredtest::CaptureSink capture;
    aa::diagnostics::Diagnostics diagnostics{capture};
    wiredtest::ScriptedTransport transport;
    auto steady_now = std::chrono::steady_clock::time_point{std::chrono::seconds{300}};

    std::vector<std::string> logs;
    VideoOutlet outlet{[&](const std::string& line) { logs.push_back(line); }};
    auto pair = videotest::make_pair({});
    ASSERT_TRUE(pair);
    const int producer_fd = pair.value().producer.native_handle();
    const int consumer_fd = pair.value().consumer.native_handle();
    outlet.install(std::move(pair.value().producer));

    aa::app::WiredSession session{{trust, clock, diagnostics}};
    ASSERT_TRUE(session.configure(wiredtest::golden_services()));
    ASSERT_TRUE(session.register_av_sink(p::ChannelRole::video, outlet));
    outlet.set_idr_recovery({
        [&session] { return session.video_projecting(); },
        [&session] { return session.request_video_idr(); },
        [&steady_now] { return steady_now; },
        [&session](bool focus_transition) { return session.request_video_idr(focus_transition); },
        aa::ipc::IdrRecoveryStrategy::auto_escalate,
    });

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
        aa::core::Result<bool> polled{false};
        for (auto& frame : frames.value()) {
            transport.push_inbound(std::move(frame));
            polled = session.poll_transport();
            if (!polled) { return polled.error(); }
        }
        return polled;
    };
    ASSERT_TRUE(receive(p::encode(p::AvSinkSetup{p::AvSinkCodec::h264}).value()));
    ASSERT_TRUE(receive(p::encode(p::AvSinkStart{1, 0}).value()));
    ASSERT_TRUE(session.apply(aa::session::Event::negotiation_succeeded));
    ASSERT_TRUE(session.video_projecting());

    // Initial IDR establishes profile and runs configure_buffer_locked before shrinking buffers.
    auto idr_payload = std::vector<std::byte>(10, std::byte{0});
    idr_payload.insert(idr_payload.end(), wiredtest::kVideoIdr.begin(), wiredtest::kVideoIdr.end());
    ASSERT_TRUE(receive(idr_payload));
    ASSERT_TRUE(pair.value().consumer.receive(Milliseconds{500}));

    const int tiny_buf = 32768;
    ASSERT_EQ(::setsockopt(producer_fd, SOL_SOCKET, SO_SNDBUF, &tiny_buf, sizeof(tiny_buf)), 0);
    ASSERT_EQ(::setsockopt(consumer_fd, SOL_SOCKET, SO_RCVBUF, &tiny_buf, sizeof(tiny_buf)), 0);

    // Saturate the tiny UNIX socket buffer with non-IDR packets so nudge #1..#3 fire via
    // on_message(), guaranteeing the buffer is already full before the escalated step.
    std::vector<std::byte> big_non_idr(10 + 16384, std::byte{0x41});
    big_non_idr[0] = big_non_idr[1] = std::byte{0};
    big_non_idr[10] = std::byte{0x00};
    big_non_idr[11] = std::byte{0x00};
    big_non_idr[12] = std::byte{0x01};
    big_non_idr[13] = std::byte{0x41};
    bool saturated = false;
    for (int i = 0; i < 64; ++i) {
        ASSERT_TRUE(receive(big_non_idr));
        if (outlet.idr_snapshot().episode_requests >= 1u) {
            saturated = true;
            break;
        }
    }
    ASSERT_TRUE(saturated);
    for (int nudge = 2; nudge <= 3; ++nudge) {
        steady_now += aa::ipc::kIdrRequestMinInterval;
        ASSERT_TRUE(receive(big_non_idr));
        EXPECT_EQ(outlet.idr_snapshot().episode_requests, static_cast<std::size_t>(nudge));
    }

    // Next drop escalates to focus_transition inside on_message(); fail the restore send
    // (2nd send after native) and verify channel_focus_restore_failed fails the session.
    steady_now += aa::ipc::kIdrRequestMinInterval;
    const std::size_t before_esc = transport.sent_count();
    transport.fail_send_at(before_esc + 1);
    const auto polled = receive(big_non_idr);
    ASSERT_FALSE(polled);
    EXPECT_EQ(polled.error().code(), aa::ErrorCode::channel_focus_restore_failed);
    EXPECT_EQ(session.state(), aa::session::State::failed);
    ASSERT_TRUE(session.failure().has_value());
    EXPECT_EQ(session.failure()->code(), aa::ErrorCode::channel_focus_restore_failed);
    EXPECT_EQ(transport.sent_count(), before_esc + 1);
    EXPECT_NE(std::find(logs.begin(), logs.end(),
                        aa::ipc::idr_transition_failed_json(
                            "producer", "restore_focus", "waiting_idr", 4, 4,
                            "channel_focus_restore_failed")),
              logs.end());
}

TEST(WiredReceiver, IdrRecoveryStrategyParserAndModes) {
    using aa::ipc::IdrRecoveryCoordinator;
    using aa::ipc::IdrRecoveryStep;
    using aa::ipc::IdrRecoveryStrategy;
    using aa::ipc::parse_idr_recovery_strategy;

    IdrRecoveryStrategy parsed{};
    EXPECT_TRUE(parse_idr_recovery_strategy("auto", parsed));
    EXPECT_EQ(parsed, IdrRecoveryStrategy::auto_escalate);
    EXPECT_TRUE(parse_idr_recovery_strategy("nudge", parsed));
    EXPECT_EQ(parsed, IdrRecoveryStrategy::nudge);
    EXPECT_TRUE(parse_idr_recovery_strategy("transition", parsed));
    EXPECT_EQ(parsed, IdrRecoveryStrategy::transition);
    EXPECT_FALSE(parse_idr_recovery_strategy("invalid", parsed));

    const auto t0 = std::chrono::steady_clock::time_point{std::chrono::seconds{1}};
    IdrRecoveryCoordinator nudge_only{IdrRecoveryCoordinator::Config{
        aa::ipc::kIdrRequestMinInterval, aa::ipc::kIdrEscalationDelay,
        aa::ipc::kIdrEscalationMaxNudges, aa::ipc::kIdrTransitionCooldown,
        IdrRecoveryStrategy::nudge}};
    nudge_only.begin_wait(t0);
    EXPECT_EQ(nudge_only.try_request_step(t0, true), IdrRecoveryStep::nudge);
    EXPECT_EQ(nudge_only.try_request_step(t0 + std::chrono::seconds{10}, true),
              IdrRecoveryStep::nudge);

    IdrRecoveryCoordinator trans_only{IdrRecoveryCoordinator::Config{
        aa::ipc::kIdrRequestMinInterval, aa::ipc::kIdrEscalationDelay,
        aa::ipc::kIdrEscalationMaxNudges, aa::ipc::kIdrTransitionCooldown,
        IdrRecoveryStrategy::transition}};
    trans_only.begin_wait(t0);
    EXPECT_EQ(trans_only.try_request_step(t0, true), IdrRecoveryStep::focus_transition);
    EXPECT_EQ(trans_only.try_request_step(t0 + std::chrono::seconds{1}, true),
              IdrRecoveryStep::none);
    EXPECT_EQ(trans_only.try_request_step(t0 + aa::ipc::kIdrTransitionCooldown, true),
              IdrRecoveryStep::focus_transition);
}

TEST(WiredReceiver, IdrRecoveryCoordinatorHoldsCooldownThroughIdrDeliveredAndReset) {
    using aa::ipc::IdrRecoveryCoordinator;
    using aa::ipc::IdrRecoveryStep;
    using aa::ipc::IdrRecoveryStrategy;

    const auto t0 = std::chrono::steady_clock::time_point{std::chrono::seconds{50}};
    IdrRecoveryCoordinator coord{IdrRecoveryCoordinator::Config{
        aa::ipc::kIdrRequestMinInterval,
        aa::ipc::kIdrEscalationDelay,
        aa::ipc::kIdrEscalationMaxNudges,
        aa::ipc::kIdrTransitionCooldown,
        IdrRecoveryStrategy::auto_escalate}};

    // Episode 1 escalates to focus_transition at t_trans = t0 + 750ms.
    coord.begin_wait(t0);
    for (int i = 0; i < 3; ++i) {
        EXPECT_EQ(coord.try_request_step(t0 + i * aa::ipc::kIdrRequestMinInterval, true),
                  IdrRecoveryStep::nudge);
    }
    const auto t_trans1 = t0 + 3 * aa::ipc::kIdrRequestMinInterval;
    EXPECT_EQ(coord.try_request_step(t_trans1, true), IdrRecoveryStep::focus_transition);

    // IDR is delivered during cooldown; Episode 2 starts and escalates before cooldown expires.
    const auto recovered = coord.on_idr_delivered(t_trans1 + std::chrono::milliseconds{200});
    EXPECT_TRUE(recovered.recovered);
    EXPECT_EQ(recovered.episode_requests, 4u);
    EXPECT_EQ(recovered.total_requests, 4u);
    const auto t_ep2 = t_trans1 + std::chrono::milliseconds{500};
    coord.begin_wait(t_ep2);
    for (int i = 0; i < 3; ++i) {
        EXPECT_EQ(coord.try_request_step(t_ep2 + i * aa::ipc::kIdrRequestMinInterval, true),
                  IdrRecoveryStep::nudge);
    }
    EXPECT_EQ(coord.try_request_step(
                  t_trans1 + aa::ipc::kIdrTransitionCooldown - aa::ipc::kIdrRequestMinInterval, true),
              IdrRecoveryStep::nudge);
    const auto t_trans2 = t_trans1 + aa::ipc::kIdrTransitionCooldown;
    EXPECT_EQ(coord.try_request_step(t_trans2, true), IdrRecoveryStep::focus_transition);

    // reset() during kIdrTransitionCooldown clears episode state without lifting cooldown.
    coord.reset();
    const auto t_ep3 = t_trans2 + aa::ipc::kIdrRequestMinInterval;
    coord.begin_wait(t_ep3);
    for (int i = 0; i < 3; ++i) {
        EXPECT_EQ(coord.try_request_step(t_ep3 + i * aa::ipc::kIdrRequestMinInterval, true),
                  IdrRecoveryStep::nudge);
    }
    EXPECT_EQ(coord.try_request_step(
                  t_trans2 + aa::ipc::kIdrTransitionCooldown - aa::ipc::kIdrRequestMinInterval, true),
              IdrRecoveryStep::nudge);
    EXPECT_EQ(coord.try_request_step(t_trans2 + aa::ipc::kIdrTransitionCooldown, true),
              IdrRecoveryStep::focus_transition);
}

TEST(WiredReceiver, IdrRecoveryCoordinatorBacksOffFailedRequestsWithoutCountingThem) {
    using aa::ipc::IdrRecoveryCoordinator;
    using aa::ipc::IdrRecoveryStep;
    using aa::ipc::IdrRecoveryStrategy;

    const auto t0 = std::chrono::steady_clock::time_point{std::chrono::seconds{60}};
    IdrRecoveryCoordinator coord{IdrRecoveryCoordinator::Config{
        aa::ipc::kIdrRequestMinInterval,
        aa::ipc::kIdrEscalationDelay,
        aa::ipc::kIdrEscalationMaxNudges,
        aa::ipc::kIdrTransitionCooldown,
        IdrRecoveryStrategy::auto_escalate}};

    coord.begin_wait(t0);
    ASSERT_EQ(coord.try_request_step(t0, true), IdrRecoveryStep::nudge);
    coord.note_request_failed(t0);

    // Counters and timestamps stay rolled back, while backoff blocks retries inside min_interval.
    EXPECT_EQ(coord.episode_requests(), 0u);
    EXPECT_EQ(coord.total_requests(), 0u);
    EXPECT_FALSE(coord.last_request_at().has_value());
    EXPECT_FALSE(coord.last_transition_at().has_value());
    EXPECT_FALSE(coord.escalated());
    EXPECT_EQ(coord.consecutive_failures(), 1u);
    EXPECT_EQ(coord.try_request_step(t0 + std::chrono::milliseconds{249}, true),
              IdrRecoveryStep::none);

    // After 250 ms, next attempt runs; a second failure doubles backoff to 500 ms (capped at 2000 ms).
    const auto t1 = t0 + aa::ipc::kIdrRequestMinInterval;
    ASSERT_EQ(coord.try_request_step(t1, true), IdrRecoveryStep::nudge);
    coord.note_request_failed(t1);
    EXPECT_EQ(coord.episode_requests(), 0u);
    EXPECT_EQ(coord.total_requests(), 0u);
    EXPECT_EQ(coord.consecutive_failures(), 2u);
    EXPECT_EQ(coord.try_request_step(t1 + std::chrono::milliseconds{499}, true),
              IdrRecoveryStep::none);

    // A successful request at t2 clears the failure backoff (next failure starts at 250 ms again).
    const auto t2 = t1 + std::chrono::milliseconds{500};
    ASSERT_EQ(coord.try_request_step(t2, true), IdrRecoveryStep::nudge);
    EXPECT_EQ(coord.consecutive_failures(), 0u);
    EXPECT_FALSE(coord.retry_after().has_value());
    EXPECT_EQ(coord.episode_requests(), 1u);
    EXPECT_EQ(coord.total_requests(), 1u);

    const auto t3 = t2 + aa::ipc::kIdrRequestMinInterval;
    ASSERT_EQ(coord.try_request_step(t3, true), IdrRecoveryStep::nudge);
    coord.note_request_failed(t3);
    EXPECT_EQ(coord.consecutive_failures(), 1u);
    EXPECT_EQ(coord.episode_requests(), 1u);
    EXPECT_EQ(coord.total_requests(), 1u);

    // on_idr_delivered() clears failure backoff immediately for a subsequent episode.
    const auto delivered = coord.on_idr_delivered(t3 + std::chrono::milliseconds{10});
    EXPECT_TRUE(delivered.recovered);
    EXPECT_EQ(delivered.episode_requests, 1u);
    EXPECT_EQ(delivered.total_requests, 1u);
    EXPECT_EQ(coord.consecutive_failures(), 0u);
    EXPECT_FALSE(coord.retry_after().has_value());
    // t3 + 20 ms is 270 ms after t2 (>= min_interval), and only succeeds because
    // on_idr_delivered cleared the t3 failure backoff (which would have held until t3 + 250 ms).
    const auto t4 = t3 + std::chrono::milliseconds{20};
    coord.begin_wait(t4);
    EXPECT_EQ(coord.try_request_step(t4, true), IdrRecoveryStep::nudge);

    // reset() also clears failure backoff even before retry_after expires.
    const auto t5 = t4 + aa::ipc::kIdrRequestMinInterval;
    ASSERT_EQ(coord.try_request_step(t5, true), IdrRecoveryStep::nudge);
    coord.note_request_failed(t5);
    EXPECT_EQ(coord.consecutive_failures(), 1u);
    EXPECT_EQ(coord.try_request_step(t5 + std::chrono::milliseconds{10}, true),
              IdrRecoveryStep::none);
    coord.reset();
    EXPECT_EQ(coord.consecutive_failures(), 0u);
    EXPECT_FALSE(coord.retry_after().has_value());
    coord.begin_wait(t5 + std::chrono::milliseconds{10});
    EXPECT_EQ(coord.try_request_step(t5 + std::chrono::milliseconds{10}, true),
              IdrRecoveryStep::nudge);

    // Repeated failures cap exponential backoff at kIdrFailureBackoffCap (2000 ms) without escalating.
    coord.reset();
    auto t_cap = t5 + std::chrono::milliseconds{10} + aa::ipc::kIdrRequestMinInterval;
    coord.begin_wait(t_cap);
    using std::chrono::milliseconds;
    const milliseconds expected_delays[] = {milliseconds{250}, milliseconds{500}, milliseconds{1000},
                                            milliseconds{2000}, milliseconds{2000}, milliseconds{2000}};
    static_assert(aa::ipc::kIdrFailureBackoffCap == milliseconds{2000});
    for (const auto expected : expected_delays) {
        ASSERT_EQ(coord.try_request_step(t_cap, true), IdrRecoveryStep::nudge);
        coord.note_request_failed(t_cap);
        ASSERT_TRUE(coord.retry_after().has_value());
        EXPECT_EQ(std::chrono::duration_cast<milliseconds>(*coord.retry_after() - t_cap), expected);
        EXPECT_EQ(coord.episode_requests(), 0u);
        EXPECT_EQ(coord.try_request_step(*coord.retry_after() - milliseconds{1}, true),
                  IdrRecoveryStep::none);
        t_cap = *coord.retry_after();
    }
    EXPECT_EQ(coord.total_requests(), 3u);
}

TEST(WiredReceiver, VideoOutletSuppressesIdrRequestsWhenNotProjecting) {
    auto steady_now = std::chrono::steady_clock::time_point{std::chrono::seconds{5}};
    bool projecting = false;
    std::size_t requests = 0;
    std::vector<std::string> logs;
    VideoOutlet outlet{[&](const std::string& line) { logs.push_back(line); }};
    outlet.set_idr_recovery({
        [&projecting] { return projecting; },
        [&requests]() -> aa::core::Result<void> { ++requests; return {}; },
        [&steady_now] { return steady_now; },
    });

    {
        const auto res = outlet.request_idr_external("consumer_ipc");
        ASSERT_TRUE(res);
        EXPECT_FALSE(res.value());
    }
    EXPECT_EQ(requests, 0u);
    EXPECT_TRUE(logs.empty());

    auto pair = videotest::make_pair({});
    ASSERT_TRUE(pair);
    const int tiny_buf = 8192;
    ASSERT_EQ(::setsockopt(pair.value().producer.native_handle(), SOL_SOCKET, SO_SNDBUF,
                           &tiny_buf, sizeof(tiny_buf)), 0);
    ASSERT_EQ(::setsockopt(pair.value().consumer.native_handle(), SOL_SOCKET, SO_RCVBUF,
                           &tiny_buf, sizeof(tiny_buf)), 0);
    outlet.install(std::move(pair.value().producer));

    std::vector<std::byte> non_idr(16384, std::byte{0x41});
    non_idr[0] = std::byte{0x00};
    non_idr[1] = std::byte{0x00};
    non_idr[2] = std::byte{0x01};
    non_idr[3] = std::byte{0x41};
    for (int i = 0; i < 64; ++i) {
        ASSERT_TRUE(outlet.on_message({{p::ChannelRole::video, static_cast<std::uint64_t>(i + 1), {100}}, non_idr}));
    }
    EXPECT_EQ(requests, 0u);

    projecting = true;
    {
        const auto res = outlet.request_idr_external("consumer_ipc");
        ASSERT_TRUE(res);
        EXPECT_TRUE(res.value());
    }
    EXPECT_EQ(requests, 1u);
    {
        const auto res = outlet.request_idr_external("consumer_ipc");
        ASSERT_TRUE(res);
        EXPECT_FALSE(res.value());
    }
    EXPECT_EQ(requests, 1u);

    steady_now += aa::ipc::kIdrRequestMinInterval;
    {
        const auto res = outlet.request_idr_external("consumer_ipc");
        ASSERT_TRUE(res);
        EXPECT_TRUE(res.value());
    }
    EXPECT_EQ(requests, 2u);
}

TEST(WiredReceiver, VideoOutletThrottlesFailedFocusNotificationAcrossDroppedAus) {
    auto steady_now = std::chrono::steady_clock::time_point{std::chrono::seconds{15}};
    std::size_t failed_hook_calls = 0;
    std::vector<std::string> logs;
    VideoOutlet outlet{[&](const std::string& line) { logs.push_back(line); }};
    outlet.set_idr_recovery({
        [] { return true; },
        [&failed_hook_calls]() -> aa::core::Result<void> {
            ++failed_hook_calls;
            return aa::Error{aa::ErrorCode::transport_io};
        },
        [&steady_now] { return steady_now; },
    });

    auto pair = videotest::make_pair({});
    ASSERT_TRUE(pair);
    const int tiny_buf = 32768;
    ASSERT_EQ(::setsockopt(pair.value().producer.native_handle(), SOL_SOCKET, SO_SNDBUF,
                           &tiny_buf, sizeof(tiny_buf)), 0);
    ASSERT_EQ(::setsockopt(pair.value().consumer.native_handle(), SOL_SOCKET, SO_RCVBUF,
                           &tiny_buf, sizeof(tiny_buf)), 0);
    outlet.install(std::move(pair.value().producer));

    // Send an initial IDR so VideoProducer leaves initial waiting_idr state before saturating.
    std::uint64_t ts = 100;
    ASSERT_TRUE(outlet.on_message({{p::ChannelRole::video, ++ts, {100}}, wiredtest::kVideoIdr}));
    ASSERT_TRUE(pair.value().consumer.receive(Milliseconds{500}));

    std::vector<std::byte> non_idr(16384, std::byte{0x41});
    non_idr[0] = std::byte{0x00};
    non_idr[1] = std::byte{0x00};
    non_idr[2] = std::byte{0x01};
    non_idr[3] = std::byte{0x41};

    // Saturate the tiny socket buffer so VideoOutlet enters dropped_backpressure / dropped_waiting_idr.
    bool saw_first_failure = false;
    for (int i = 0; i < 64; ++i) {
        ASSERT_TRUE(outlet.on_message({{p::ChannelRole::video, ++ts, {100}}, non_idr}));
        if (failed_hook_calls == 1u) {
            saw_first_failure = true;
            break;
        }
    }
    ASSERT_TRUE(saw_first_failure);
    EXPECT_EQ(outlet.idr_snapshot().episode_requests, 0u);
    EXPECT_EQ(outlet.idr_snapshot().total_requests, 0u);

    // Many dropped AUs within the 250 ms failure backoff must not invoke the hook again.
    for (int i = 0; i < 24; ++i) {
        steady_now += std::chrono::milliseconds{10};
        ASSERT_TRUE(outlet.on_message({{p::ChannelRole::video, ++ts, {100}}, non_idr}));
    }
    EXPECT_EQ(failed_hook_calls, 1u);
    EXPECT_EQ(outlet.idr_snapshot().episode_requests, 0u);
    EXPECT_EQ(outlet.idr_snapshot().total_requests, 0u);

    // Once the 250 ms failure backoff elapses (+10 ms -> 250 ms total), one more attempt runs.
    steady_now += std::chrono::milliseconds{10};
    ASSERT_TRUE(outlet.on_message({{p::ChannelRole::video, ++ts, {100}}, non_idr}));
    EXPECT_EQ(failed_hook_calls, 2u);
    EXPECT_EQ(outlet.idr_snapshot().episode_requests, 0u);
    EXPECT_EQ(outlet.idr_snapshot().total_requests, 0u);
}

// #90 review 5: the producer logs its own wait transition, so a stall where
// every IDR request fails (no idr_requested ever) still opens an episode.
TEST(WiredReceiver, VideoOutletLogsTheProducerWaitEvenWhenEveryIdrRequestFails) {
    auto steady_now = std::chrono::steady_clock::time_point{std::chrono::seconds{70}};
    std::size_t failed = 0;
    std::vector<std::string> logs;
    VideoOutlet outlet{[&](const std::string& line) { logs.push_back(line); }};
    outlet.set_idr_recovery({
        [] { return true; },
        {},
        [&steady_now] { return steady_now; },
        [&failed](bool) -> aa::core::Result<void> {
            ++failed;
            return aa::Error{aa::ErrorCode::transport_io};
        },
        aa::ipc::IdrRecoveryStrategy::auto_escalate,
    });
    const auto count = [&](const std::string& line) {
        return static_cast<std::size_t>(std::count(logs.begin(), logs.end(), line));
    };
    const std::string started =
        aa::ipc::idr_wait_started_json("producer", "waiting_idr");

    // The initial IDR passes before any consumer; the late consumer then only
    // sees P-frames, so the producer waits, and the request fails.
    std::vector<std::byte> idr_bytes(10, std::byte{0});
    idr_bytes.insert(idr_bytes.end(), wiredtest::kVideoIdr.begin(), wiredtest::kVideoIdr.end());
    ASSERT_TRUE(outlet.on_message({{p::ChannelRole::video, 1, {100}}, idr_bytes}));
    auto pair = videotest::make_pair({});
    ASSERT_TRUE(pair);
    outlet.install(std::move(pair.value().producer));
    std::vector<std::byte> p_frame(16, std::byte{0x41});
    p_frame[0] = std::byte{0x00};
    p_frame[1] = std::byte{0x00};
    p_frame[2] = std::byte{0x01};
    ASSERT_TRUE(outlet.on_message({{p::ChannelRole::video, 2, {200}}, p_frame}));
    ASSERT_EQ(failed, 1u);
    EXPECT_EQ(count(started), 1u);

    // Retries inside the same wait (and an external request) fail too: still
    // one transition record, and never a request record.
    for (int i = 0; i < 4; ++i) {
        steady_now += std::chrono::milliseconds{300};
        ASSERT_TRUE(outlet.on_message({{p::ChannelRole::video, 3u + static_cast<std::uint64_t>(i),
                                        {300}}, p_frame}));
    }
    steady_now += std::chrono::milliseconds{300};
    (void)outlet.request_idr_external("consumer_ipc");
    EXPECT_GT(failed, 1u);
    EXPECT_EQ(count(started), 1u);
    EXPECT_EQ(std::count_if(logs.begin(), logs.end(), [](const std::string& line) {
        return line.find("\"event\":\"idr_wait_started\"") != std::string::npos;
    }), 1);
    EXPECT_EQ(std::count_if(logs.begin(), logs.end(), [](const std::string& line) {
        return line.find("\"event\":\"idr_requested\"") != std::string::npos;
    }), 0);

    // Once recovered, the next wait (an external request) logs its own transition.
    ASSERT_TRUE(outlet.on_message({{p::ChannelRole::video, 20, {400}}, idr_bytes}));
    ASSERT_TRUE(pair.value().consumer.receive(Milliseconds{500}));
    steady_now += std::chrono::milliseconds{2000};
    (void)outlet.request_idr_external("consumer_ipc");
    EXPECT_EQ(count(aa::ipc::idr_wait_started_json("producer", "consumer_ipc")), 1u);
}

TEST(WiredReceiver, VideoSelectionWatchdogEmitsOnceAfterTimeoutAndResets) {
    using aa::ipc::kVideoSelectionWarningTimeout;
    using aa::ipc::VideoSelectionWatchdog;
    VideoSelectionWatchdog watchdog;
    auto t0 = std::chrono::steady_clock::time_point{std::chrono::seconds{1}};

    EXPECT_FALSE(watchdog.check(false, false, t0));
    EXPECT_FALSE(watchdog.check(true, false, t0));
    EXPECT_FALSE(watchdog.check(true, false, t0 + kVideoSelectionWarningTimeout - std::chrono::milliseconds{1}));
    EXPECT_TRUE(watchdog.check(true, false, t0 + kVideoSelectionWarningTimeout));
    EXPECT_TRUE(watchdog.warned());
    EXPECT_FALSE(watchdog.check(true, false, t0 + kVideoSelectionWarningTimeout + std::chrono::seconds{2}));

    // Selecting a profile resets the watchdog so a future session/relink starts clean.
    EXPECT_FALSE(watchdog.check(true, true, t0 + std::chrono::seconds{10}));
    EXPECT_FALSE(watchdog.warned());
    EXPECT_FALSE(watchdog.check(true, false, t0 + std::chrono::seconds{11}));
    EXPECT_TRUE(watchdog.check(true, false, t0 + std::chrono::seconds{11} + kVideoSelectionWarningTimeout));
}

namespace {

std::vector<std::byte> make_h265_au(bool idr, std::size_t payload_bytes, std::byte fill) {
    std::vector<std::byte> bytes(std::max<std::size_t>(payload_bytes, 6U), fill);
    bytes[0] = std::byte{0x00};
    bytes[1] = std::byte{0x00};
    bytes[2] = std::byte{0x01};
    // H.265 NAL header: IRAP IDR_W_RADL (type 19 -> 0x26) vs TRAIL_R P-frame (type 1 -> 0x02).
    bytes[3] = static_cast<std::byte>(idr ? 0x26U : 0x02U);
    bytes[4] = std::byte{0x01};
    return bytes;
}

std::vector<std::uint8_t> to_u8(const std::vector<std::byte>& src) {
    std::vector<std::uint8_t> out;
    out.reserve(src.size());
    for (const auto b : src) {
        out.push_back(std::to_integer<std::uint8_t>(b));
    }
    return out;
}

std::optional<aa::ipc::VideoAccessUnit> receive_complete_au(aa::ipc::VideoConsumer& consumer) {
    for (int i = 0; i < 64; ++i) {
        const auto step = consumer.receive(Milliseconds{500});
        if (!step) {
            return std::nullopt;
        }
        if (step.value() == aa::ipc::ReceiveStatus::complete) {
            return consumer.access_unit();
        }
    }
    return std::nullopt;
}

void drain_available_aus(aa::ipc::VideoConsumer& consumer,
                         std::vector<aa::ipc::VideoAccessUnit>& out) {
    for (;;) {
        const auto step = consumer.receive(Milliseconds{0});
        if (!step) {
            break;
        }
        if (step.value() == aa::ipc::ReceiveStatus::complete) {
            out.push_back(consumer.access_unit());
        }
    }
}

std::vector<std::byte> make_unique_h265_au(bool idr, std::size_t payload_bytes, std::uint8_t tag) {
    auto bytes = make_h265_au(idr, payload_bytes, std::byte{tag});
    for (std::size_t i = 5; i < bytes.size(); ++i) {
        bytes[i] = static_cast<std::byte>((tag + i * 17U) & 0xFFU);
    }
    return bytes;
}

} // namespace

TEST(WiredReceiver, VideoOutletHoldsTransientBackpressureAcrossLargeIdrAndSmallAusInOrder) {
    auto steady_now = std::chrono::steady_clock::time_point{std::chrono::seconds{100}};
    std::size_t idr_requests = 0;
    std::vector<std::string> logs;
    VideoOutlet outlet{[&](const std::string& line) { logs.push_back(line); }};
    outlet.set_idr_recovery({
        [] { return true; },
        [&idr_requests]() -> aa::core::Result<void> { ++idr_requests; return {}; },
        [&steady_now] { return steady_now; },
    });

    auto pair = videotest::make_pair({});
    ASSERT_TRUE(pair);

    // Trigger EAGAIN once on the 1st datagram after the large ~214 KB IDR, then accept.
    const auto profile = p::VideoProfile{2560, 1440, 60, p::VideoCodec::h265};
    const auto large_idr = make_h265_au(true, 214 * 1024, std::byte{0x61});
    const auto small_p1 = make_h265_au(false, 20 * 1024, std::byte{0x21});
    const auto small_p2 = make_h265_au(false, 24 * 1024, std::byte{0x31});

    const std::size_t idr_datagrams =
        (large_idr.size() + aa::ipc::kMaxDatagramPayload - 1) / aa::ipc::kMaxDatagramPayload;
    std::size_t call_index = 0;
    bool block_once = false;
    pair.value().producer.set_datagram_send_for_test(
        [&](int fd, const void* buf, std::size_t len, int flags, int& out_errno) -> std::ptrdiff_t {
            ++call_index;
            if (call_index == idr_datagrams + 1 && !block_once) {
                block_once = true;
                out_errno = EAGAIN;
                return -1;
            }
            const auto sent = ::send(fd, buf, len, flags);
            out_errno = (sent < 0) ? errno : 0;
            return sent;
        });

    outlet.install(std::move(pair.value().producer));

    p::MessageHeader hdr{p::ChannelRole::video, 1, {1'000'000}};
    hdr.video_profile = profile;
    hdr.video_session = 1;
    hdr.video_generation = 1;
    ASSERT_TRUE(outlet.on_message({hdr, large_idr}));
    EXPECT_FALSE(outlet.has_pending());

    // First small P-frame immediately after the IDR sees one EAGAIN and is held.
    steady_now += std::chrono::milliseconds{4};
    hdr.sequence = 2;
    hdr.timestamp = aa::core::Nanoseconds{16'666'667};
    ASSERT_TRUE(outlet.on_message({hdr, small_p1}));
    EXPECT_TRUE(block_once);
    EXPECT_TRUE(outlet.has_pending());
    EXPECT_EQ(outlet.pending_units(), 1U);
    EXPECT_EQ(outlet.pending_bytes(), small_p1.size());
    EXPECT_EQ(idr_requests, 0U);

    // Next small P-frame arrives 16 ms later: fake socket accepts again, so
    // P1 flushes cleanly ahead of P2, preserving strict AU order.
    steady_now += std::chrono::milliseconds{16};
    hdr.sequence = 3;
    hdr.timestamp = aa::core::Nanoseconds{33'333'334};
    ASSERT_TRUE(outlet.on_message({hdr, small_p2}));
    EXPECT_FALSE(outlet.has_pending());
    EXPECT_EQ(outlet.pending_units(), 0U);
    EXPECT_EQ(outlet.pending_bytes(), 0U);
    EXPECT_EQ(idr_requests, 0U);

    // Zero drops and zero waiting_idr transitions; hold + release telemetry emitted.
    EXPECT_EQ(std::count_if(logs.begin(), logs.end(), [](const std::string& s) {
        return s.find("\"event\":\"video_dropped\"") != std::string::npos;
    }), 0);
    EXPECT_NE(std::find_if(logs.begin(), logs.end(), [](const std::string& s) {
        return s.find("\"event\":\"video_backpressure_held\"") != std::string::npos &&
               s.find("\"held_units\":1") != std::string::npos;
    }), logs.end());
    EXPECT_NE(std::find_if(logs.begin(), logs.end(), [](const std::string& s) {
        return s.find("\"event\":\"video_backpressure_released\"") != std::string::npos &&
               s.find("\"age_ms\":16") != std::string::npos;
    }), logs.end());

    const auto rx_idr = receive_complete_au(pair.value().consumer);
    ASSERT_TRUE(rx_idr.has_value());
    EXPECT_EQ(rx_idr->frame, 1U);
    EXPECT_EQ(rx_idr->payload, to_u8(large_idr));

    const auto rx_p1 = receive_complete_au(pair.value().consumer);
    ASSERT_TRUE(rx_p1.has_value());
    EXPECT_EQ(rx_p1->frame, 2U);
    EXPECT_EQ(rx_p1->payload, to_u8(small_p1));

    const auto rx_p2 = receive_complete_au(pair.value().consumer);
    ASSERT_TRUE(rx_p2.has_value());
    EXPECT_EQ(rx_p2->frame, 3U);
    EXPECT_EQ(rx_p2->payload, to_u8(small_p2));

    EXPECT_FALSE(pair.value().consumer.waiting_idr());
    EXPECT_EQ(pair.value().consumer.rejected(), 0U);
    EXPECT_EQ(pair.value().consumer.suppressed(), 0U);
}

TEST(WiredReceiver, VideoOutletRealSocketShortGrantHoldsAndReleasesWithoutDrop) {
    auto steady_now = std::chrono::steady_clock::time_point{std::chrono::seconds{150}};
    std::size_t idr_requests = 0;
    std::vector<std::string> logs;
    VideoOutlet outlet{[&](const std::string& line) { logs.push_back(line); }};
    outlet.set_idr_recovery({
        [] { return true; },
        [&idr_requests]() -> aa::core::Result<void> { ++idr_requests; return {}; },
        [&steady_now] { return steady_now; },
    });

    auto pair = videotest::make_pair({});
    ASSERT_TRUE(pair);
    const int producer_fd = pair.value().producer.native_handle();
    const int consumer_fd = pair.value().consumer.native_handle();
    outlet.install(std::move(pair.value().producer));

    struct ExpectedFrame final {
        std::uint64_t frame{};
        bool idr{};
        std::vector<std::uint8_t> payload{};
    };
    std::vector<ExpectedFrame> expected;
    std::vector<aa::ipc::VideoAccessUnit> received;

    // Frame 1: small IDR establishes profile (configure_buffer_locked) and clears startup gate.
    const auto profile = p::VideoProfile{2560, 1440, 60, p::VideoCodec::h265};
    const auto initial_idr = make_unique_h265_au(true, 4 * 1024, 0x11U);
    p::MessageHeader hdr{p::ChannelRole::video, 1, {100}};
    hdr.video_profile = profile;
    hdr.video_session = 1;
    hdr.video_generation = 1;
    ASSERT_TRUE(outlet.on_message({hdr, initial_idr}));
    expected.push_back({1U, true, to_u8(initial_idr)});
    drain_available_aus(pair.value().consumer, received);
    ASSERT_EQ(received.size(), 1U);

    // Shrink socket buffers to simulate a Jetson wmem_max short grant, then send a large
    // recovery/refresh IDR (~128 KiB) that partially/mostly fills the short-granted socket,
    // followed by distinct P-frames until real kernel EAGAIN occurs.
    const int short_buf = 98304;
    ASSERT_EQ(::setsockopt(producer_fd, SOL_SOCKET, SO_SNDBUF, &short_buf, sizeof(short_buf)), 0);
    ASSERT_EQ(::setsockopt(consumer_fd, SOL_SOCKET, SO_RCVBUF, &short_buf, sizeof(short_buf)), 0);

    std::uint64_t seq = 2;
    const auto large_refresh_idr = make_unique_h265_au(true, 128 * 1024, 0x22U);
    hdr.sequence = seq;
    hdr.timestamp = aa::core::Nanoseconds{static_cast<std::int64_t>(seq) * 16'000'000};
    ASSERT_TRUE(outlet.on_message({hdr, large_refresh_idr}));
    expected.push_back({seq, true, to_u8(large_refresh_idr)});
    ++seq;

    for (unsigned i = 0; i < 32U && !outlet.has_pending(); ++i) {
        steady_now += std::chrono::milliseconds{4};
        const auto p_frame = make_unique_h265_au(false, 24 * 1024, static_cast<std::uint8_t>(0x30U + i));
        hdr.sequence = seq;
        hdr.timestamp = aa::core::Nanoseconds{static_cast<std::int64_t>(seq) * 16'000'000};
        ASSERT_TRUE(outlet.on_message({hdr, p_frame}));
        expected.push_back({seq, false, to_u8(p_frame)});
        ++seq;
    }
    ASSERT_TRUE(outlet.has_pending());
    // Queue one additional distinct P-frame while held to prove FIFO release across multiple units.
    {
        const auto extra_p = make_unique_h265_au(false, 16 * 1024, 0x7EU);
        hdr.sequence = seq;
        hdr.timestamp = aa::core::Nanoseconds{static_cast<std::int64_t>(seq) * 16'000'000};
        ASSERT_TRUE(outlet.on_message({hdr, extra_p}));
        expected.push_back({seq, false, to_u8(extra_p)});
        ++seq;
    }
    EXPECT_GE(outlet.pending_units(), 2U);
    EXPECT_EQ(idr_requests, 0U);

    // Scripted draining + injected clock service_pending() without any sleeps.
    for (int pump = 0; pump < 8; ++pump) {
        drain_available_aus(pair.value().consumer, received);
        if (!outlet.has_pending()) {
            break;
        }
        steady_now += std::chrono::milliseconds{2};
        ASSERT_TRUE(outlet.service_pending());
    }
    drain_available_aus(pair.value().consumer, received);

    EXPECT_FALSE(outlet.has_pending());
    EXPECT_EQ(outlet.pending_units(), 0U);
    EXPECT_EQ(outlet.pending_bytes(), 0U);
    EXPECT_EQ(idr_requests, 0U);
    EXPECT_EQ(std::count_if(logs.begin(), logs.end(), [](const std::string& s) {
        return s.find("\"event\":\"video_dropped\"") != std::string::npos;
    }), 0);
    EXPECT_NE(std::find_if(logs.begin(), logs.end(), [](const std::string& s) {
        return s.find("\"event\":\"video_backpressure_held\"") != std::string::npos;
    }), logs.end());
    EXPECT_NE(std::find_if(logs.begin(), logs.end(), [](const std::string& s) {
        return s.find("\"event\":\"video_backpressure_released\"") != std::string::npos;
    }), logs.end());

    ASSERT_EQ(received.size(), expected.size());
    for (std::size_t i = 0; i < expected.size(); ++i) {
        EXPECT_EQ(received[i].frame, expected[i].frame) << "frame mismatch at index " << i;
        EXPECT_EQ(received[i].idr, expected[i].idr) << "idr mismatch at index " << i;
        EXPECT_EQ(received[i].payload, expected[i].payload) << "payload mismatch at index " << i;
    }
    EXPECT_FALSE(pair.value().consumer.waiting_idr());
    EXPECT_EQ(pair.value().consumer.rejected(), 0U);
    EXPECT_EQ(pair.value().consumer.suppressed(), 0U);
    EXPECT_EQ(pair.value().consumer.resyncs(), 1U);
}

TEST(WiredReceiver, VideoOutletSustainedBackpressureOverflowEntersWaitingIdrAndRequestsIdr) {
    auto steady_now = std::chrono::steady_clock::time_point{std::chrono::seconds{200}};
    std::size_t idr_requests = 0;
    std::vector<std::string> logs;
    VideoOutlet outlet{[&](const std::string& line) { logs.push_back(line); }};
    outlet.set_idr_recovery({
        [] { return true; },
        [&idr_requests]() -> aa::core::Result<void> { ++idr_requests; return {}; },
        [&steady_now] { return steady_now; },
    });

    auto pair = videotest::make_pair({});
    ASSERT_TRUE(pair);

    bool force_would_block = false;
    pair.value().producer.set_datagram_send_for_test(
        [&force_would_block](int fd, const void* buf, std::size_t len, int flags,
                             int& out_errno) -> std::ptrdiff_t {
            if (force_would_block) {
                out_errno = EAGAIN;
                return -1;
            }
            const auto sent = ::send(fd, buf, len, flags);
            out_errno = (sent < 0) ? errno : 0;
            return sent;
        });
    outlet.install(std::move(pair.value().producer));

    const auto profile = p::VideoProfile{1920, 1080, 60, p::VideoCodec::h265};
    const auto budget = aa::ipc::recommended_backpressure_budget(profile); // 486,000 B, 50 ms
    const auto idr = make_h265_au(true, 16 * 1024, std::byte{0x10});
    p::MessageHeader hdr{p::ChannelRole::video, 1, {100}};
    hdr.video_profile = profile;
    hdr.video_session = 1;
    hdr.video_generation = 1;
    ASSERT_TRUE(outlet.on_message({hdr, idr}));
    ASSERT_TRUE(receive_complete_au(pair.value().consumer).has_value());

    // Stall socket indefinitely and push 100 KB P-frames until byte budget overflows.
    force_would_block = true;
    const auto chunk = make_h265_au(false, 100 * 1024, std::byte{0x22});
    std::uint64_t seq = 2;
    for (int i = 0; i < 4; ++i) {
        hdr.sequence = seq++;
        hdr.timestamp = aa::core::Nanoseconds{static_cast<std::int64_t>(seq) * 16'000'000};
        ASSERT_TRUE(outlet.on_message({hdr, chunk}));
        EXPECT_LE(outlet.pending_bytes(), budget.max_bytes);
        EXPECT_EQ(idr_requests, 0U);
    }
    EXPECT_EQ(outlet.pending_units(), 4U);
    EXPECT_EQ(outlet.pending_bytes(), 400U * 1024U);

    // 5th 100 KB P-frame exceeds 486,000 B bound -> drop, backpressure IDR request, waiting_idr.
    hdr.sequence = seq++;
    hdr.timestamp = aa::core::Nanoseconds{static_cast<std::int64_t>(seq) * 16'000'000};
    ASSERT_TRUE(outlet.on_message({hdr, chunk}));
    EXPECT_FALSE(outlet.has_pending());
    EXPECT_EQ(idr_requests, 1U);
    EXPECT_NE(std::find_if(logs.begin(), logs.end(), [](const std::string& s) {
        return s.find("\"event\":\"video_dropped\",\"reason\":\"backpressure\"") != std::string::npos;
    }), logs.end());
    EXPECT_NE(std::find(logs.begin(), logs.end(),
                        "{\"event\":\"idr_requested\",\"source\":\"producer\",\"reason\":\"backpressure\"}"),
              logs.end());

    // Next P-frame while socket is now writable is suppressed as waiting_idr until fresh IDR arrives.
    force_would_block = false;
    hdr.sequence = seq++;
    ASSERT_TRUE(outlet.on_message({hdr, make_h265_au(false, 1024, std::byte{0x33})}));
    const auto none = pair.value().consumer.receive(Milliseconds{0});
    EXPECT_FALSE(none);

    steady_now += std::chrono::milliseconds{80};
    hdr.sequence = seq++;
    ASSERT_TRUE(outlet.on_message({hdr, idr}));
    const auto recovered = receive_complete_au(pair.value().consumer);
    ASSERT_TRUE(recovered.has_value());
    EXPECT_TRUE(recovered->idr);
}

TEST(WiredReceiver, VideoOutletResetAndConsumerReplacementClearHeldAccessUnits) {
    auto steady_now = std::chrono::steady_clock::time_point{std::chrono::seconds{300}};
    std::vector<std::string> logs;
    VideoOutlet outlet{[&](const std::string& line) { logs.push_back(line); }};
    outlet.set_idr_recovery({[] { return true; }, {}, [&steady_now] { return steady_now; }});

    auto pair1 = videotest::make_pair({});
    ASSERT_TRUE(pair1);
    bool force_would_block = false;
    pair1.value().producer.set_datagram_send_for_test(
        [&force_would_block](int fd, const void* buf, std::size_t len, int flags,
                             int& out_errno) -> std::ptrdiff_t {
            if (force_would_block) {
                out_errno = EAGAIN;
                return -1;
            }
            const auto sent = ::send(fd, buf, len, flags);
            out_errno = (sent < 0) ? errno : 0;
            return sent;
        });
    outlet.install(std::move(pair1.value().producer));

    const auto profile = p::VideoProfile{2560, 1440, 60, p::VideoCodec::h265};
    p::MessageHeader hdr{p::ChannelRole::video, 1, {100}};
    hdr.video_profile = profile;
    hdr.video_session = 1;
    hdr.video_generation = 1;
    ASSERT_TRUE(outlet.on_message({hdr, make_h265_au(true, 1024, std::byte{0x10})}));
    ASSERT_TRUE(receive_complete_au(pair1.value().consumer).has_value());

    // 1) Reset clears held AUs and must not deliver old held frames when stream resumes.
    force_would_block = true;
    hdr.sequence = 2;
    ASSERT_TRUE(outlet.on_message({hdr, make_h265_au(false, 2048, std::byte{0xaa})}));
    hdr.sequence = 3;
    ASSERT_TRUE(outlet.on_message({hdr, make_h265_au(false, 2048, std::byte{0xbb})}));
    EXPECT_EQ(outlet.pending_units(), 2U);
    EXPECT_EQ(outlet.pending_bytes(), 4096U);

    outlet.reset();
    EXPECT_FALSE(outlet.has_pending());
    EXPECT_EQ(outlet.pending_units(), 0U);
    EXPECT_EQ(outlet.pending_bytes(), 0U);
    EXPECT_EQ(std::count_if(logs.begin(), logs.end(), [](const std::string& s) {
        return s.find("\"event\":\"video_backpressure_released\"") != std::string::npos;
    }), 0);

    force_would_block = false;
    const auto fresh_idr_after_reset = make_h265_au(true, 1024, std::byte{0x55});
    hdr.sequence = 4;
    hdr.video_session = 2;
    ASSERT_TRUE(outlet.on_message({hdr, fresh_idr_after_reset}));
    const auto after_reset = receive_complete_au(pair1.value().consumer);
    ASSERT_TRUE(after_reset.has_value());
    EXPECT_EQ(after_reset->payload, to_u8(fresh_idr_after_reset));

    // 2) Consumer replacement clears held AUs queued for the prior consumer.
    force_would_block = true;
    hdr.sequence = 5;
    ASSERT_TRUE(outlet.on_message({hdr, make_h265_au(false, 2048, std::byte{0xcc})}));
    EXPECT_EQ(outlet.pending_units(), 1U);

    auto pair2 = videotest::make_pair({});
    ASSERT_TRUE(pair2);
    outlet.install(std::move(pair2.value().producer));
    EXPECT_FALSE(outlet.has_pending());
    EXPECT_EQ(outlet.pending_units(), 0U);
    EXPECT_EQ(outlet.pending_bytes(), 0U);

    const auto replacement_idr = make_h265_au(true, 1024, std::byte{0x77});
    hdr.sequence = 6;
    ASSERT_TRUE(outlet.on_message({hdr, replacement_idr}));
    const auto got2 = receive_complete_au(pair2.value().consumer);
    ASSERT_TRUE(got2.has_value());
    EXPECT_EQ(got2->payload, to_u8(replacement_idr));
}

TEST(WiredReceiver, VideoOutletBackpressureAgeBoundUsesInjectedClock) {
    auto steady_now = std::chrono::steady_clock::time_point{std::chrono::seconds{400}};
    std::size_t idr_requests = 0;
    std::vector<std::string> logs;
    VideoOutlet outlet{[&](const std::string& line) { logs.push_back(line); }};
    outlet.set_idr_recovery({
        [] { return true; },
        [&idr_requests]() -> aa::core::Result<void> { ++idr_requests; return {}; },
        [&steady_now] { return steady_now; },
    });

    auto pair = videotest::make_pair({});
    ASSERT_TRUE(pair);
    bool force_would_block = false;
    pair.value().producer.set_datagram_send_for_test(
        [&force_would_block](int fd, const void* buf, std::size_t len, int flags,
                             int& out_errno) -> std::ptrdiff_t {
            if (force_would_block) {
                out_errno = EAGAIN;
                return -1;
            }
            const auto sent = ::send(fd, buf, len, flags);
            out_errno = (sent < 0) ? errno : 0;
            return sent;
        });
    outlet.install(std::move(pair.value().producer));

    const auto profile = p::VideoProfile{2560, 1440, 60, p::VideoCodec::h265};
    const auto budget = aa::ipc::recommended_backpressure_budget(profile);
    ASSERT_EQ(budget.max_age, std::chrono::milliseconds{50});

    p::MessageHeader hdr{p::ChannelRole::video, 1, {100}};
    hdr.video_profile = profile;
    hdr.video_session = 1;
    hdr.video_generation = 1;
    ASSERT_TRUE(outlet.on_message({hdr, make_h265_au(true, 1024, std::byte{0x10})}));
    ASSERT_TRUE(receive_complete_au(pair.value().consumer).has_value());

    // Hold a single small P-frame well below max_bytes.
    force_would_block = true;
    hdr.sequence = 2;
    ASSERT_TRUE(outlet.on_message({hdr, make_h265_au(false, 4096, std::byte{0x20})}));
    ASSERT_EQ(outlet.pending_units(), 1U);
    EXPECT_EQ(idr_requests, 0U);

    // At max_age - 1 ms according to injected clock: must still be held.
    steady_now += budget.max_age - std::chrono::milliseconds{1};
    ASSERT_TRUE(outlet.service_pending());
    EXPECT_TRUE(outlet.has_pending());
    EXPECT_EQ(outlet.pending_units(), 1U);
    EXPECT_EQ(idr_requests, 0U);
    EXPECT_EQ(std::count_if(logs.begin(), logs.end(), [](const std::string& s) {
        return s.find("\"event\":\"video_dropped\"") != std::string::npos;
    }), 0);

    // Reaching max_age (+1 ms) expires the hold and triggers drop + IDR request.
    steady_now += std::chrono::milliseconds{1};
    ASSERT_TRUE(outlet.service_pending());
    EXPECT_FALSE(outlet.has_pending());
    EXPECT_EQ(outlet.pending_units(), 0U);
    EXPECT_EQ(outlet.pending_bytes(), 0U);
    EXPECT_EQ(idr_requests, 1U);
    EXPECT_NE(std::find_if(logs.begin(), logs.end(), [](const std::string& s) {
        return s.find("\"event\":\"video_dropped\",\"reason\":\"backpressure\"") != std::string::npos;
    }), logs.end());
}

TEST(WiredReceiver, VideoOutletResetAfterHeldAccessUnitRestoresWaitingIdr) {
    auto steady_now = std::chrono::steady_clock::time_point{std::chrono::seconds{500}};
    std::size_t idr_requests = 0;
    std::vector<std::string> logs;
    VideoOutlet outlet{[&](const std::string& line) { logs.push_back(line); }};
    outlet.set_idr_recovery({
        [] { return true; },
        [&idr_requests]() -> aa::core::Result<void> { ++idr_requests; return {}; },
        [&steady_now] { return steady_now; },
    });

    auto pair = videotest::make_pair({});
    ASSERT_TRUE(pair);

    std::size_t fail_from_datagram = 0; // 0 = allow all, 1 = fail datagram 0, 2 = fail datagram 2+
    std::size_t au_datagram_index = 0;
    pair.value().producer.set_datagram_send_for_test(
        [&](int fd, const void* buf, std::size_t len, int flags,
            int& out_errno) -> std::ptrdiff_t {
            ++au_datagram_index;
            if (fail_from_datagram != 0 && au_datagram_index >= fail_from_datagram) {
                out_errno = EAGAIN;
                return -1;
            }
            const auto sent = ::send(fd, buf, len, flags);
            out_errno = (sent < 0) ? errno : 0;
            return sent;
        });
    outlet.install(std::move(pair.value().producer));

    const auto profile = p::VideoProfile{2560, 1440, 60, p::VideoCodec::h265};
    p::MessageHeader hdr{p::ChannelRole::video, 1, {100}};
    hdr.video_profile = profile;
    hdr.video_session = 1;
    hdr.video_generation = 1;

    const auto initial_idr = make_unique_h265_au(true, 4 * 1024, 0x11U);
    ASSERT_TRUE(outlet.on_message({hdr, initial_idr}));
    const auto got_init = receive_complete_au(pair.value().consumer);
    ASSERT_TRUE(got_init.has_value());
    EXPECT_EQ(got_init->frame, 1U);

    // Phase A: EAGAIN on fragment 0 (partial.active == false) holds a P-frame, then reset().
    fail_from_datagram = 1;
    au_datagram_index = 0;
    hdr.sequence = 2;
    hdr.timestamp = aa::core::Nanoseconds{16'666'667};
    ASSERT_TRUE(outlet.on_message({hdr, make_unique_h265_au(false, 4 * 1024, 0x22U)}));
    EXPECT_TRUE(outlet.has_pending());
    EXPECT_EQ(outlet.pending_units(), 1U);

    outlet.reset();
    EXPECT_FALSE(outlet.has_pending());
    EXPECT_EQ(outlet.pending_units(), 0U);
    EXPECT_EQ(outlet.pending_bytes(), 0U);
    EXPECT_NE(std::find_if(logs.begin(), logs.end(), [](const std::string& s) {
        return s.find("\"event\":\"video_dropped\",\"reason\":\"reset\"") != std::string::npos;
    }), logs.end());
    EXPECT_EQ(std::count_if(logs.begin(), logs.end(), [](const std::string& s) {
        return s.find("\"event\":\"video_backpressure_released\"") != std::string::npos;
    }), 0);

    // Unblock socket and send a non-IDR first in the resumed session: must be suppressed + request IDR.
    fail_from_datagram = 0;
    std::vector<aa::ipc::VideoAccessUnit> suppressed_drain;
    hdr.video_profile = profile;
    hdr.video_session = 2;
    hdr.video_generation = 1;
    hdr.sequence = 3;
    hdr.timestamp = aa::core::Nanoseconds{33'333'334};
    ASSERT_TRUE(outlet.on_message({hdr, make_unique_h265_au(false, 4 * 1024, 0x33U)}));
    drain_available_aus(pair.value().consumer, suppressed_drain);
    EXPECT_TRUE(suppressed_drain.empty());
    EXPECT_EQ(idr_requests, 1U);
    EXPECT_NE(std::find(logs.begin(), logs.end(),
                        "{\"event\":\"idr_requested\",\"source\":\"producer\",\"reason\":\"waiting_idr\"}"),
              logs.end());

    // Subsequent fresh IDR delivers and clears waiting_idr.
    const auto fresh_idr_a = make_unique_h265_au(true, 8 * 1024, 0x44U);
    hdr.sequence = 4;
    hdr.timestamp = aa::core::Nanoseconds{50'000'001};
    ASSERT_TRUE(outlet.on_message({hdr, fresh_idr_a}));
    const auto got_a = receive_complete_au(pair.value().consumer);
    ASSERT_TRUE(got_a.has_value());
    EXPECT_TRUE(got_a->idr);
    EXPECT_EQ(got_a->payload, to_u8(fresh_idr_a));

    // Phase B: EAGAIN mid-AU (partial.active == true after datagram 1 was sent) -> reset().
    steady_now += std::chrono::seconds{5};
    fail_from_datagram = 2;
    au_datagram_index = 0;
    hdr.sequence = 5;
    hdr.timestamp = aa::core::Nanoseconds{66'666'668};
    ASSERT_TRUE(outlet.on_message({hdr, make_unique_h265_au(false, 100 * 1024, 0x55U)}));
    EXPECT_TRUE(outlet.has_pending());
    EXPECT_EQ(outlet.pending_units(), 1U);

    outlet.reset();
    EXPECT_FALSE(outlet.has_pending());
    EXPECT_EQ(outlet.pending_units(), 0U);
    EXPECT_EQ(outlet.pending_bytes(), 0U);
    EXPECT_EQ(std::count_if(logs.begin(), logs.end(), [](const std::string& s) {
        return s.find("\"event\":\"video_backpressure_released\"") != std::string::npos;
    }), 0);

    // Non-IDR in new session is still suppressed, partial fragment is ignored on recovery, and IDR wins.
    fail_from_datagram = 0;
    suppressed_drain.clear();
    drain_available_aus(pair.value().consumer, suppressed_drain);
    EXPECT_TRUE(suppressed_drain.empty());

    hdr.video_profile = profile;
    hdr.video_session = 3;
    hdr.sequence = 6;
    hdr.timestamp = aa::core::Nanoseconds{83'333'335};
    ASSERT_TRUE(outlet.on_message({hdr, make_unique_h265_au(false, 4 * 1024, 0x66U)}));
    drain_available_aus(pair.value().consumer, suppressed_drain);
    EXPECT_TRUE(suppressed_drain.empty());
    EXPECT_EQ(idr_requests, 2U);

    const auto fresh_idr_b = make_unique_h265_au(true, 8 * 1024, 0x77U);
    hdr.sequence = 7;
    hdr.timestamp = aa::core::Nanoseconds{100'000'002};
    ASSERT_TRUE(outlet.on_message({hdr, fresh_idr_b}));
    const auto got_b = receive_complete_au(pair.value().consumer);
    ASSERT_TRUE(got_b.has_value());
    EXPECT_TRUE(got_b->idr);
    EXPECT_EQ(got_b->payload, to_u8(fresh_idr_b));
    EXPECT_FALSE(pair.value().consumer.waiting_idr());

    outlet.close();
    EXPECT_EQ(std::count_if(logs.begin(), logs.end(), [](const std::string& s) {
        return s.find("\"event\":\"video_backpressure_released\"") != std::string::npos;
    }), 0);
    EXPECT_NE(std::find_if(logs.begin(), logs.end(), [](const std::string& s) {
        return s.find("\"event\":\"decode_stats\",\"frame_drops\":4") != std::string::npos;
    }), logs.end());
}

TEST(WiredReceiver, VideoOutletEpochChangeWhileHeldRestoresWaitingIdrAndDropsStaleFrames) {
    auto steady_now = std::chrono::steady_clock::time_point{std::chrono::seconds{600}};
    std::size_t idr_requests = 0;
    std::vector<std::string> logs;
    VideoOutlet outlet{[&](const std::string& line) { logs.push_back(line); }};
    outlet.set_idr_recovery({
        [] { return true; },
        [&idr_requests]() -> aa::core::Result<void> { ++idr_requests; return {}; },
        [&steady_now] { return steady_now; },
    });

    auto pair = videotest::make_pair({});
    ASSERT_TRUE(pair);

    std::size_t fail_from_datagram = 0;
    std::size_t au_datagram_index = 0;
    pair.value().producer.set_datagram_send_for_test(
        [&](int fd, const void* buf, std::size_t len, int flags,
            int& out_errno) -> std::ptrdiff_t {
            ++au_datagram_index;
            if (fail_from_datagram != 0 && au_datagram_index >= fail_from_datagram) {
                out_errno = EAGAIN;
                return -1;
            }
            const auto sent = ::send(fd, buf, len, flags);
            out_errno = (sent < 0) ? errno : 0;
            return sent;
        });
    outlet.install(std::move(pair.value().producer));

    const auto profile = p::VideoProfile{2560, 1440, 60, p::VideoCodec::h265};
    p::MessageHeader hdr{p::ChannelRole::video, 1, {100}};
    hdr.video_profile = profile;
    hdr.video_session = 1;
    hdr.video_generation = 1;

    ASSERT_TRUE(outlet.on_message({hdr, make_unique_h265_au(true, 4 * 1024, 0x11U)}));
    ASSERT_TRUE(receive_complete_au(pair.value().consumer).has_value());

    // Phase A: EAGAIN on fragment 0 (partial.active == false) holds stale frame from session 1.
    fail_from_datagram = 1;
    au_datagram_index = 0;
    hdr.sequence = 2;
    hdr.timestamp = aa::core::Nanoseconds{16'666'667};
    ASSERT_TRUE(outlet.on_message({hdr, make_unique_h265_au(false, 4 * 1024, 0x22U)}));
    EXPECT_TRUE(outlet.has_pending());
    EXPECT_EQ(outlet.pending_units(), 1U);

    // Socket recovers simultaneously with an epoch change carrying a non-IDR frame:
    // held frame must be dropped ("stream_reset") and new non-IDR suppressed + IDR requested.
    fail_from_datagram = 0;
    std::vector<aa::ipc::VideoAccessUnit> suppressed_drain;
    hdr.video_session = 2;
    hdr.sequence = 3;
    hdr.timestamp = aa::core::Nanoseconds{33'333'334};
    ASSERT_TRUE(outlet.on_message({hdr, make_unique_h265_au(false, 4 * 1024, 0x33U)}));
    EXPECT_FALSE(outlet.has_pending());
    EXPECT_EQ(outlet.pending_units(), 0U);
    drain_available_aus(pair.value().consumer, suppressed_drain);
    EXPECT_TRUE(suppressed_drain.empty());
    EXPECT_EQ(idr_requests, 1U);
    EXPECT_NE(std::find_if(logs.begin(), logs.end(), [](const std::string& s) {
        return s.find("\"event\":\"video_dropped\",\"reason\":\"stream_reset\"") != std::string::npos;
    }), logs.end());
    EXPECT_NE(std::find(logs.begin(), logs.end(),
                        "{\"event\":\"idr_requested\",\"source\":\"producer\",\"reason\":\"waiting_idr\"}"),
              logs.end());
    EXPECT_EQ(std::count_if(logs.begin(), logs.end(), [](const std::string& s) {
        return s.find("\"event\":\"video_backpressure_released\"") != std::string::npos;
    }), 0);

    // Fresh IDR in session 2 succeeds.
    const auto fresh_idr_a = make_unique_h265_au(true, 8 * 1024, 0x44U);
    hdr.sequence = 4;
    hdr.timestamp = aa::core::Nanoseconds{50'000'001};
    ASSERT_TRUE(outlet.on_message({hdr, fresh_idr_a}));
    const auto got_a = receive_complete_au(pair.value().consumer);
    ASSERT_TRUE(got_a.has_value());
    EXPECT_TRUE(got_a->idr);
    EXPECT_EQ(got_a->payload, to_u8(fresh_idr_a));

    // Phase B: EAGAIN mid-AU (partial.active == true) holds stale frame in session 2, then epoch 3 non-IDR.
    steady_now += std::chrono::seconds{5};
    fail_from_datagram = 2;
    au_datagram_index = 0;
    hdr.sequence = 5;
    hdr.timestamp = aa::core::Nanoseconds{66'666'668};
    ASSERT_TRUE(outlet.on_message({hdr, make_unique_h265_au(false, 100 * 1024, 0x55U)}));
    EXPECT_TRUE(outlet.has_pending());
    EXPECT_EQ(outlet.pending_units(), 1U);

    fail_from_datagram = 0;
    suppressed_drain.clear();
    hdr.video_session = 3;
    hdr.sequence = 6;
    hdr.timestamp = aa::core::Nanoseconds{83'333'335};
    ASSERT_TRUE(outlet.on_message({hdr, make_unique_h265_au(false, 4 * 1024, 0x66U)}));
    EXPECT_FALSE(outlet.has_pending());
    drain_available_aus(pair.value().consumer, suppressed_drain);
    EXPECT_TRUE(suppressed_drain.empty());
    EXPECT_EQ(idr_requests, 2U);

    // Fresh IDR in session 3 recovers cleanly.
    const auto fresh_idr_b = make_unique_h265_au(true, 8 * 1024, 0x77U);
    hdr.sequence = 7;
    hdr.timestamp = aa::core::Nanoseconds{100'000'002};
    ASSERT_TRUE(outlet.on_message({hdr, fresh_idr_b}));
    const auto got_b = receive_complete_au(pair.value().consumer);
    ASSERT_TRUE(got_b.has_value());
    EXPECT_TRUE(got_b->idr);
    EXPECT_EQ(got_b->payload, to_u8(fresh_idr_b));
    EXPECT_FALSE(pair.value().consumer.waiting_idr());

    outlet.close();
    EXPECT_EQ(std::count_if(logs.begin(), logs.end(), [](const std::string& s) {
        return s.find("\"event\":\"video_backpressure_released\"") != std::string::npos;
    }), 0);
    EXPECT_NE(std::find_if(logs.begin(), logs.end(), [](const std::string& s) {
        return s.find("\"event\":\"decode_stats\",\"frame_drops\":4") != std::string::npos;
    }), logs.end());
}

namespace {

// Wireless playout (credit window + playout, 2026-11-20): video is held for the audio playout depth and then
// forwarded with its arrival stamp, so the display's paint latency includes the
// same offset as audio_write_completed's presented latency.
TEST(WiredReceiver, PresentationDelayHoldsVideoForThePlayoutDepthWithItsArrivalStamp) {
    using namespace std::chrono_literals;
    std::vector<std::string> logs;
    VideoOutlet outlet{[&](const std::string& line) { logs.push_back(line); }};
    auto now = std::chrono::steady_clock::time_point{} + 10s;
    VideoOutlet::IdrRecoveryHooks hooks;
    hooks.clock = [&] { return now; };
    outlet.set_idr_recovery(std::move(hooks));
    auto pair = videotest::make_pair({});
    ASSERT_TRUE(pair);
    outlet.install(std::move(pair.value().producer));
    outlet.set_presentation_delay(120ms);
    const auto complete = [&](Milliseconds wait) {
        const auto received = pair.value().consumer.receive(wait);
        return received && received.value() == aa::ipc::ReceiveStatus::complete;
    };
    const p::MessageHeader header{p::ChannelRole::video, 1, aa::core::Nanoseconds{5'000'000'000}};
    ASSERT_TRUE(outlet.on_message({header, wiredtest::kVideoIdr}));
    EXPECT_TRUE(outlet.has_pending());
    EXPECT_EQ(outlet.delayed_units(), 1U);
    EXPECT_FALSE(complete(Milliseconds{20}));
    now += 119ms;
    ASSERT_TRUE(outlet.service_pending());
    EXPECT_FALSE(complete(Milliseconds{20}));
    now += 1ms;
    ASSERT_TRUE(outlet.service_pending());
    ASSERT_TRUE(complete(Milliseconds{500}));
    EXPECT_EQ(pair.value().consumer.access_unit().timestamp_ns, 5'000'000'000ULL);
    EXPECT_TRUE(pair.value().consumer.access_unit().idr);
    EXPECT_FALSE(outlet.has_pending());

    // Order is kept: a later frame never overtakes a held one.
    const p::MessageHeader second{p::ChannelRole::video, 2, aa::core::Nanoseconds{5'100'000'000}};
    const p::MessageHeader third{p::ChannelRole::video, 3, aa::core::Nanoseconds{5'110'000'000}};
    ASSERT_TRUE(outlet.on_message({second, wiredtest::kVideoIdr}));
    now += 10ms;
    ASSERT_TRUE(outlet.on_message({third, wiredtest::kVideoIdr}));
    EXPECT_EQ(outlet.delayed_units(), 2U);
    now += 110ms; // only the second frame is due
    ASSERT_TRUE(outlet.service_pending());
    ASSERT_TRUE(complete(Milliseconds{500}));
    EXPECT_EQ(pair.value().consumer.access_unit().timestamp_ns, 5'100'000'000ULL);
    EXPECT_EQ(outlet.delayed_units(), 1U);
    // A stream reset drops what is still held instead of sending it late.
    outlet.reset();
    EXPECT_FALSE(outlet.has_pending());
    now += 1s;
    ASSERT_TRUE(outlet.service_pending());
    EXPECT_FALSE(complete(Milliseconds{20}));
}

// Without a delay (wired) a frame is forwarded during on_message as before.
TEST(WiredReceiver, NoPresentationDelayForwardsAtOnce) {
    VideoOutlet outlet{[](const std::string&) {}};
    auto pair = videotest::make_pair({});
    ASSERT_TRUE(pair);
    outlet.install(std::move(pair.value().producer));
    const p::MessageHeader header{p::ChannelRole::video, 1, aa::core::Nanoseconds{7}};
    ASSERT_TRUE(outlet.on_message({header, wiredtest::kVideoIdr}));
    EXPECT_FALSE(outlet.has_pending());
    const auto received = pair.value().consumer.receive(Milliseconds{500});
    ASSERT_TRUE(received);
    EXPECT_EQ(received.value(), aa::ipc::ReceiveStatus::complete);
    EXPECT_EQ(pair.value().consumer.access_unit().timestamp_ns, 7U);
}

} // namespace

namespace {

// Neon P3: frames still held for the playout delay are older than a
// backpressure overflow's recovery IDR. They are dropped with the overflowed
// queue instead of dripping out after it.
TEST(WiredReceiver, PresentationDelayDropsHeldFramesOnBackpressureOverflow) {
    using namespace std::chrono_literals;
    auto now = std::chrono::steady_clock::time_point{} + 200s;
    std::size_t idr_requests = 0;
    std::vector<std::string> logs;
    VideoOutlet outlet{[&](const std::string& line) { logs.push_back(line); }};
    outlet.set_idr_recovery({
        [] { return true; },
        [&idr_requests]() -> aa::core::Result<void> { ++idr_requests; return {}; },
        [&now] { return now; },
    });
    auto pair = videotest::make_pair({});
    ASSERT_TRUE(pair);
    bool force_would_block = false;
    pair.value().producer.set_datagram_send_for_test(
        [&force_would_block](int fd, const void* buf, std::size_t len, int flags,
                             int& out_errno) -> std::ptrdiff_t {
            if (force_would_block) { out_errno = EAGAIN; return -1; }
            const auto sent = ::send(fd, buf, len, flags);
            out_errno = (sent < 0) ? errno : 0;
            return sent;
        });
    outlet.install(std::move(pair.value().producer));
    outlet.set_presentation_delay(120ms);
    const auto profile = p::VideoProfile{1920, 1080, 60, p::VideoCodec::h265};
    p::MessageHeader hdr{p::ChannelRole::video, 1, {100}};
    hdr.video_profile = profile;
    hdr.video_session = 1;
    hdr.video_generation = 1;
    ASSERT_TRUE(outlet.on_message({hdr, make_h265_au(true, 16 * 1024, std::byte{0x10})}));
    now += 120ms;
    ASSERT_TRUE(outlet.service_pending());
    ASSERT_TRUE(receive_complete_au(pair.value().consumer).has_value());

    // The socket stalls. Five 100 KB P-frames arrive together, then three more
    // 100 ms later; when the first five come due they overflow the byte budget.
    force_would_block = true;
    const auto chunk = make_h265_au(false, 100 * 1024, std::byte{0x22});
    for (int i = 0; i < 5; ++i) {
        ++hdr.sequence;
        ASSERT_TRUE(outlet.on_message({hdr, chunk}));
    }
    now += 100ms;
    for (int i = 0; i < 3; ++i) {
        ++hdr.sequence;
        ASSERT_TRUE(outlet.on_message({hdr, chunk}));
    }
    EXPECT_EQ(outlet.delayed_units(), 8U);
    now += 20ms;
    ASSERT_TRUE(outlet.service_pending());
    EXPECT_EQ(idr_requests, 1U);
    EXPECT_EQ(outlet.delayed_units(), 0U);
    EXPECT_FALSE(outlet.has_pending());
}

// Neon P3: a consumer that disconnects takes the held frames with it.
TEST(WiredReceiver, PresentationDelayDropsHeldFramesWhenTheConsumerDisconnects) {
    using namespace std::chrono_literals;
    auto now = std::chrono::steady_clock::time_point{} + 300s;
    VideoOutlet outlet{[](const std::string&) {}};
    VideoOutlet::IdrRecoveryHooks hooks;
    hooks.clock = [&] { return now; };
    outlet.set_idr_recovery(std::move(hooks));
    auto pair = videotest::make_pair({});
    ASSERT_TRUE(pair);
    outlet.install(std::move(pair.value().producer));
    outlet.set_presentation_delay(120ms);
    const p::MessageHeader first{p::ChannelRole::video, 1, aa::core::Nanoseconds{1}};
    const p::MessageHeader second{p::ChannelRole::video, 2, aa::core::Nanoseconds{2}};
    ASSERT_TRUE(outlet.on_message({first, wiredtest::kVideoIdr}));
    now += 50ms;
    ASSERT_TRUE(outlet.on_message({second, wiredtest::kVideoIdr}));
    pair.value().consumer.close();
    now += 70ms; // only the first frame is due: its send finds the peer gone
    ASSERT_TRUE(outlet.service_pending());
    EXPECT_EQ(outlet.delayed_units(), 0U);
    EXPECT_FALSE(outlet.has_pending());
}

} // namespace

namespace {

// N1: on the wireless receiver the video hold follows the audio playout offset
// reported for the frame's own timestamp, read per frame. A shorter hold
// never lets a later frame overtake a held one, and the applied hold falls by
// at most half the time that passes, so no frame is pinned behind another's
// due; a longer one holds the next frames longer at once.
TEST(WiredReceiver, PresentationDelaySourceIsReadPerFrameAndKeepsOrder) {
    using namespace std::chrono_literals;
    auto now = std::chrono::steady_clock::time_point{} + 400s;
    VideoOutlet outlet{[](const std::string&) {}};
    VideoOutlet::IdrRecoveryHooks hooks;
    hooks.clock = [&] { return now; };
    outlet.set_idr_recovery(std::move(hooks));
    auto pair = videotest::make_pair({});
    ASSERT_TRUE(pair);
    outlet.install(std::move(pair.value().producer));
    // The hold is the offset of the audio each frame's content pairs with:
    // frame n is keyed by its timestamp n.
    std::map<std::int64_t, std::chrono::nanoseconds> hold{{1, 120ms}, {2, 60ms}, {3, 200ms}};
    std::vector<std::int64_t> asked;
    outlet.set_presentation_delay_source([&](aa::core::Nanoseconds content) {
        asked.push_back(content.count);
        const auto found = hold.find(content.count);
        return found == hold.end() ? std::chrono::nanoseconds{120ms} : found->second;
    });
    EXPECT_EQ(outlet.presentation_delay_for(aa::core::Nanoseconds{7}), 120ms);
    const auto complete = [&](Milliseconds wait) {
        const auto received = pair.value().consumer.receive(wait);
        return received && received.value() == aa::ipc::ReceiveStatus::complete;
    };
    const auto frame = [](std::uint64_t n) {
        return p::MessageHeader{p::ChannelRole::video, n, aa::core::Nanoseconds{static_cast<std::int64_t>(n)}};
    };
    ASSERT_TRUE(outlet.on_message({frame(1), wiredtest::kVideoIdr})); // due +120
    EXPECT_EQ(asked.back(), 1);                                        // asked for its own timestamp
    now += 10ms;
    ASSERT_TRUE(outlet.on_message({frame(2), wiredtest::kVideoIdr})); // 60, slewed: 120 - 5 = 115, due +125
    now += 110ms;                                                     // t = 120
    ASSERT_TRUE(outlet.service_pending());
    ASSERT_TRUE(complete(Milliseconds{500}));
    EXPECT_EQ(pair.value().consumer.access_unit().timestamp_ns, 1U);
    EXPECT_FALSE(complete(Milliseconds{20}));
    now += 5ms;                                                       // t = 125
    ASSERT_TRUE(outlet.service_pending());
    ASSERT_TRUE(complete(Milliseconds{500}));
    EXPECT_EQ(pair.value().consumer.access_unit().timestamp_ns, 2U);
    ASSERT_TRUE(outlet.on_message({frame(3), wiredtest::kVideoIdr})); // 200: due t = 325
    now += 199ms;
    ASSERT_TRUE(outlet.service_pending());
    EXPECT_FALSE(complete(Milliseconds{20}));
    now += 1ms;
    ASSERT_TRUE(outlet.service_pending());
    ASSERT_TRUE(complete(Milliseconds{500}));
    EXPECT_EQ(pair.value().consumer.access_unit().timestamp_ns, 3U);
}

// Neon P1-1: when the offset drops (a burst peak ageing out, a re-anchor) the
// frames after the drop must not be pinned behind the last held frame's due -
// a ~drop-long freeze, then a burst, and an A/V excursion of (drop - frame
// period). With the applied hold slewing down at half rate every frame is
// released on its own, at least half a frame period after the previous one.
TEST(WiredReceiver, PresentationDelayDownwardStepNeverPinsFrames) {
    using namespace std::chrono_literals;
    auto now = std::chrono::steady_clock::time_point{} + 600s;
    VideoOutlet outlet{[](const std::string&) {}};
    VideoOutlet::IdrRecoveryHooks hooks;
    hooks.clock = [&] { return now; };
    outlet.set_idr_recovery(std::move(hooks));
    auto pair = videotest::make_pair({});
    ASSERT_TRUE(pair);
    outlet.install(std::move(pair.value().producer));
    std::chrono::nanoseconds hold = 295ms;
    outlet.set_presentation_delay_source([&](aa::core::Nanoseconds) { return hold; });
    std::vector<std::chrono::steady_clock::time_point> released;
    const auto drain = [&] {
        for (;;) {
            const auto received = pair.value().consumer.receive(Milliseconds{0});
            if (!received || received.value() != aa::ipc::ReceiveStatus::complete) { break; }
            released.push_back(now);
        }
    };
    // 30 fps for 3 s; the source drops 295 -> 120 ms after 1 s.
    const auto period = std::chrono::nanoseconds{33'333'333};
    auto next_frame = now;
    std::uint64_t n = 0;
    for (auto end = now + 3s; now < end; now += 1ms) {
        if (now >= next_frame) {
            if (n == 30) { hold = 120ms; }
            ASSERT_TRUE(outlet.on_message({p::MessageHeader{p::ChannelRole::video, ++n,
                aa::core::Nanoseconds{static_cast<std::int64_t>(n)}}, wiredtest::kVideoIdr}));
            next_frame += period;
        }
        ASSERT_TRUE(outlet.service_pending());
        drain();
    }
    ASSERT_GT(released.size(), 70U);
    for (std::size_t i = 1; i < released.size(); ++i) {
        EXPECT_GE(released[i] - released[i - 1], period / 2 - 2ms) << "frame " << i;
    }
}

// Neon P1-2 / DeepSeek R1: a hold above the cap is clamped, never silently:
// every frame released with a capped hold is counted and logged (at release,
// from the audio known then; Neon P3 round 4).
TEST(WiredReceiver, PresentationDelayCapIsCountedAndLogged) {
    using namespace std::chrono_literals;
    auto now = std::chrono::steady_clock::time_point{} + 800s;
    std::vector<std::string> media;
    VideoOutlet outlet{[](const std::string&) {}, [&](std::string_view, const std::string& line) {
        media.push_back(line);
    }};
    VideoOutlet::IdrRecoveryHooks hooks;
    hooks.clock = [&] { return now; };
    outlet.set_idr_recovery(std::move(hooks));
    auto pair = videotest::make_pair({});
    ASSERT_TRUE(pair);
    outlet.install(std::move(pair.value().producer));
    outlet.set_presentation_delay_source([](aa::core::Nanoseconds) { return std::chrono::nanoseconds{600ms}; });
    ASSERT_TRUE(outlet.on_message({p::MessageHeader{p::ChannelRole::video, 1, aa::core::Nanoseconds{1}},
                                   wiredtest::kVideoIdr}));
    EXPECT_EQ(outlet.capped_holds(), 0U);
    now += 399ms;
    ASSERT_TRUE(outlet.release_held());
    EXPECT_EQ(outlet.capped_holds(), 0U);
    now += 1ms;
    ASSERT_TRUE(outlet.release_held());
    EXPECT_EQ(outlet.capped_holds(), 1U);
    EXPECT_NE(std::find_if(media.begin(), media.end(), [](const std::string& line) {
        return line.find("{\"event\":\"video_hold_capped\",\"requested_ms\":600,\"cap_ms\":400,") != std::string::npos;
    }), media.end());
}

// F12 / N4: a replacement consumer does not receive frames held for the old one.
TEST(WiredReceiver, PresentationDelayDropsHeldFramesWhenTheConsumerIsReplaced) {
    using namespace std::chrono_literals;
    auto now = std::chrono::steady_clock::time_point{} + 500s;
    VideoOutlet outlet{[](const std::string&) {}};
    VideoOutlet::IdrRecoveryHooks hooks;
    hooks.clock = [&] { return now; };
    outlet.set_idr_recovery(std::move(hooks));
    auto first = videotest::make_pair({});
    ASSERT_TRUE(first);
    outlet.install(std::move(first.value().producer));
    outlet.set_presentation_delay(120ms);
    ASSERT_TRUE(outlet.on_message({p::MessageHeader{p::ChannelRole::video, 1, aa::core::Nanoseconds{1}},
                                   wiredtest::kVideoIdr}));
    EXPECT_EQ(outlet.delayed_units(), 1U);
    auto second = videotest::make_pair({});
    ASSERT_TRUE(second);
    outlet.install(std::move(second.value().producer));
    EXPECT_EQ(outlet.delayed_units(), 0U);
}

} // namespace

namespace {

// Window 12 at 349.5 s: a Wi-Fi stall blocked the session's outbound send for
// ~90 ms, the runtime skipped service_pending() (it may make session sends)
// and held frames left 89 ms late (+103 ms A/V). release_held() releases due
// frames with no session I/O; an IDR request a release triggers (here a
// backpressure overflow) waits for the next service_pending().
TEST(WiredReceiver, ReleaseHeldNeedsNoSessionAndDefersItsIdrRequest) {
    using namespace std::chrono_literals;
    auto now = std::chrono::steady_clock::time_point{} + 700s;
    std::size_t idr_requests = 0;
    VideoOutlet outlet{[](const std::string&) {}};
    outlet.set_idr_recovery({
        [] { return true; },
        [&idr_requests]() -> aa::core::Result<void> { ++idr_requests; return {}; },
        [&now] { return now; },
    });
    auto pair = videotest::make_pair({});
    ASSERT_TRUE(pair);
    bool force_would_block = false;
    pair.value().producer.set_datagram_send_for_test(
        [&force_would_block](int fd, const void* buf, std::size_t len, int flags,
                             int& out_errno) -> std::ptrdiff_t {
            if (force_would_block) { out_errno = EAGAIN; return -1; }
            const auto sent = ::send(fd, buf, len, flags);
            out_errno = (sent < 0) ? errno : 0;
            return sent;
        });
    outlet.install(std::move(pair.value().producer));
    outlet.set_presentation_delay(120ms);
    const auto profile = p::VideoProfile{1920, 1080, 60, p::VideoCodec::h265};
    p::MessageHeader hdr{p::ChannelRole::video, 1, {100}};
    hdr.video_profile = profile;
    hdr.video_session = 1;
    hdr.video_generation = 1;
    ASSERT_TRUE(outlet.on_message({hdr, make_h265_au(true, 16 * 1024, std::byte{0x10})}));
    now += 120ms;
    ASSERT_TRUE(outlet.release_held());
    ASSERT_TRUE(receive_complete_au(pair.value().consumer).has_value());
    force_would_block = true;
    const auto chunk = make_h265_au(false, 100 * 1024, std::byte{0x22});
    for (int i = 0; i < 5; ++i) {
        ++hdr.sequence;
        ASSERT_TRUE(outlet.on_message({hdr, chunk}));
    }
    now += 120ms;
    ASSERT_TRUE(outlet.release_held()); // overflows: the IDR request waits
    EXPECT_EQ(idr_requests, 0U);
    EXPECT_FALSE(outlet.has_pending());
    ASSERT_TRUE(outlet.service_pending());
    EXPECT_EQ(idr_requests, 1U);
}

} // namespace

namespace {

// Neon P1 #2 (round 4): release_held() defers the IDR request a waiting_idr
// drop makes. When the batch it releases also carries the keyframe, the
// episode closes there, and the deferred request is obsolete: dispatching it
// later reopened a phantom waiting_idr episode and sent a spurious request.
TEST(WiredReceiver, ReleaseHeldKeyframeInTheBatchCancelsTheDeferredIdrRequest) {
    using namespace std::chrono_literals;
    auto now = std::chrono::steady_clock::time_point{} + 900s;
    std::size_t idr_requests = 0;
    std::vector<std::string> logs;
    VideoOutlet outlet{[&](const std::string& line) { logs.push_back(line); }};
    outlet.set_idr_recovery({
        [] { return true; },
        [&idr_requests]() -> aa::core::Result<void> { ++idr_requests; return {}; },
        [&now] { return now; },
    });
    auto pair = videotest::make_pair({});
    ASSERT_TRUE(pair);
    outlet.install(std::move(pair.value().producer));
    outlet.set_presentation_delay(120ms);
    now += 2s; // past the IDR startup grace: a dropped non-IDR asks for one
    const auto profile = p::VideoProfile{1920, 1080, 60, p::VideoCodec::h265};
    p::MessageHeader hdr{p::ChannelRole::video, 1, {100}};
    hdr.video_profile = profile;
    hdr.video_session = 1;
    hdr.video_generation = 1;
    ASSERT_TRUE(outlet.on_message({hdr, make_h265_au(false, 1024, std::byte{0x22})})); // non-IDR
    ++hdr.sequence;
    ASSERT_TRUE(outlet.on_message({hdr, make_h265_au(true, 1024, std::byte{0x10})}));  // IDR
    now += 120ms;
    ASSERT_TRUE(outlet.release_held());
    ASSERT_TRUE(receive_complete_au(pair.value().consumer).has_value());
    const auto waits_before = std::count_if(logs.begin(), logs.end(), [](const std::string& line) {
        return line.find("\"event\":\"idr_wait_started\"") != std::string::npos;
    });
    ASSERT_TRUE(outlet.service_pending());
    EXPECT_EQ(idr_requests, 0U);
    const auto waits_after = std::count_if(logs.begin(), logs.end(), [](const std::string& line) {
        return line.find("\"event\":\"idr_wait_started\"") != std::string::npos;
    });
    EXPECT_EQ(waits_after, waits_before);
}

// Window 13: a frame that leaves more than 20 ms after its due is counted and
// logged (video_release_late), so the next window shows any release stall.
TEST(WiredReceiver, LateReleaseIsCountedAndLogged) {
    using namespace std::chrono_literals;
    auto now = std::chrono::steady_clock::time_point{} + 1000s;
    std::vector<std::string> media;
    VideoOutlet outlet{[](const std::string&) {}, [&](std::string_view, const std::string& line) {
        media.push_back(line);
    }};
    VideoOutlet::IdrRecoveryHooks hooks;
    hooks.clock = [&] { return now; };
    outlet.set_idr_recovery(std::move(hooks));
    auto pair = videotest::make_pair({});
    ASSERT_TRUE(pair);
    outlet.install(std::move(pair.value().producer));
    outlet.set_presentation_delay(120ms);
    ASSERT_TRUE(outlet.on_message({p::MessageHeader{p::ChannelRole::video, 1, aa::core::Nanoseconds{1}},
                                   wiredtest::kVideoIdr}));
    now += 125ms; // 5 ms late: fine
    ASSERT_TRUE(outlet.release_held());
    EXPECT_EQ(outlet.late_releases(), 0U);
    ASSERT_TRUE(outlet.on_message({p::MessageHeader{p::ChannelRole::video, 2, aa::core::Nanoseconds{2}},
                                   wiredtest::kVideoIdr}));
    now += 205ms; // 85 ms late, as in window 13
    ASSERT_TRUE(outlet.release_held());
    EXPECT_EQ(outlet.late_releases(), 1U);
    EXPECT_NE(std::find_if(media.begin(), media.end(), [](const std::string& line) {
        return line.find("{\"event\":\"video_release_late\",\"late_ms\":85,") != std::string::npos;
    }), media.end());
}

// Window 14: the owner's checkpoint went 35-90 ms without running every ~2 s,
// and ~270 held frames left more than 20 ms after their due. The wireless
// outlet now releases held frames on a timer of its own: with no
// release_held() call at all (an owner thread that never reaches a
// checkpoint), every frame still reaches the consumer within a few ms of its
// arrival + hold.
TEST(WiredReceiver, ReleaseTimerReleasesHeldFramesAtTheirDueWithoutACheckpoint) {
    using namespace std::chrono_literals;
    using Clock = std::chrono::steady_clock;
    VideoOutlet outlet{[](const std::string&) {}, [](std::string_view, const std::string&) {}};
    auto pair = videotest::make_pair({});
    ASSERT_TRUE(pair);
    outlet.install(std::move(pair.value().producer));
    constexpr auto kHold = 40ms;
    constexpr std::size_t kFrames = 40;
    outlet.set_presentation_delay(kHold);
    outlet.start_release_timer();
    std::vector<Clock::time_point> arrived(kFrames + 1);
    std::vector<Clock::time_point> received(kFrames + 1);
    std::thread consumer([&] {
        for (std::size_t n = 0; n < kFrames; ++n) {
            const auto au = receive_complete_au(pair.value().consumer);
            if (!au) { return; }
            if (au->frame <= kFrames) { received[au->frame] = Clock::now(); }
        }
    });
    for (std::uint64_t k = 1; k <= kFrames; ++k) {
        arrived[k] = Clock::now();
        ASSERT_TRUE(outlet.on_message({p::MessageHeader{p::ChannelRole::video, k, aa::core::Nanoseconds{
                                           static_cast<std::int64_t>(k)}}, wiredtest::kVideoIdr}));
        std::this_thread::sleep_for(k % 4 == 0 ? 37ms : 3ms); // grouped, as the A26 sends
    }
    consumer.join();
    std::vector<double> late_ms;
    for (std::size_t k = 1; k <= kFrames; ++k) {
        ASSERT_NE(received[k], Clock::time_point{}) << "frame " << k << " never reached the consumer";
        late_ms.push_back(std::chrono::duration<double, std::milli>(received[k] - (arrived[k] + kHold)).count());
    }
    std::sort(late_ms.begin(), late_ms.end());
    std::printf("release lateness ms: p50 %.2f p90 %.2f max %.2f\n", late_ms[late_ms.size() / 2],
                late_ms[late_ms.size() * 9 / 10], late_ms.back());
    EXPECT_GE(late_ms.front(), -0.5) << "a frame left before its due";
    EXPECT_LT(late_ms.back(), 5.0) << "a frame left more than 5 ms after its due";
    EXPECT_EQ(outlet.late_releases(), 0U);
    outlet.close();
}

} // namespace
