// SPDX-License-Identifier: GPL-3.0-or-later
#include <aa/protocol/AvSink.hpp>
#include <aa/transport/Frames.hpp>

#include <aap_protobuf/service/media/shared/message/Config.pb.h>
#include <aap_protobuf/service/media/source/message/Ack.pb.h>
#include <aap_protobuf/service/media/video/message/VideoFocusNotification.pb.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <limits>

namespace {
namespace p = aa::protocol;
using Bytes = std::vector<std::byte>;
Bytes bytes(std::initializer_list<unsigned int> values) {
    Bytes result;
    for (auto value : values) { result.push_back(static_cast<std::byte>(value)); }
    return result;
}
template <typename Wire> void read_reply(Wire& wire, const Bytes& payload, unsigned int id) {
    ASSERT_GE(payload.size(), 2U);
    EXPECT_EQ(payload[0], static_cast<std::byte>(id >> 8U));
    EXPECT_EQ(payload[1], static_cast<std::byte>(id & 255U));
    ASSERT_TRUE(wire.ParseFromArray(payload.data() + 2, static_cast<int>(payload.size() - 2)));
}

TEST(AvSink, SetupStartStopRoundTripAndSignedSessions) {
    for (auto codec : {p::AvSinkCodec::pcm, p::AvSinkCodec::h264}) {
        auto encoded = p::encode(p::AvSinkSetup{codec});
        ASSERT_TRUE(encoded);
        auto decoded = p::decode_av_sink_setup(encoded.value());
        ASSERT_TRUE(decoded);
        EXPECT_EQ(decoded.value().codec, codec);
    }
    for (auto session : {std::numeric_limits<std::int32_t>::min(), -1, 0, 1,
                         std::numeric_limits<std::int32_t>::max()}) {
        auto encoded = p::encode(p::AvSinkStart{session, UINT32_MAX});
        ASSERT_TRUE(encoded);
        auto decoded = p::decode_av_sink_start(encoded.value());
        ASSERT_TRUE(decoded);
        EXPECT_EQ(decoded.value().session_id, session);
        EXPECT_EQ(decoded.value().configuration_index, UINT32_MAX);
        auto ack = p::encode_av_sink_ack(session);
        ASSERT_TRUE(ack);
        aap_protobuf::service::media::source::message::Ack wire;
        read_reply(wire, ack.value(), 0x8004);
        EXPECT_EQ(wire.session_id(), session);
        EXPECT_TRUE(wire.has_ack());
        EXPECT_EQ(wire.ack(), 1U);
        EXPECT_EQ(wire.receive_timestamp_ns_size(), 0);
    }
    auto stop = p::encode(p::AvSinkStop{});
    ASSERT_TRUE(stop);
    EXPECT_EQ(stop.value(), bytes({0x80, 2}));
    EXPECT_TRUE(p::decode_av_sink_stop(stop.value()));
}

TEST(AvSink, ReadyHasExactlyIndexZeroOneCreditStatusTwo) {
    auto ready = p::encode_av_sink_ready();
    ASSERT_TRUE(ready);
    aap_protobuf::service::media::shared::message::Config wire;
    read_reply(wire, ready.value(), 0x8003);
    EXPECT_EQ(wire.status(), 2);
    EXPECT_TRUE(wire.has_max_unacked());
    EXPECT_EQ(wire.max_unacked(), 1U);
    ASSERT_EQ(wire.configuration_indices_size(), 1);
    EXPECT_EQ(wire.configuration_indices(0), 0U);
}

TEST(AvSink, FocusModesAndOptionalPresence) {
    auto absent = p::decode_video_focus_request(bytes({0x80, 7}));
    ASSERT_TRUE(absent);
    EXPECT_FALSE(absent.value().mode);
    auto empty = p::encode(p::VideoFocusRequest{});
    ASSERT_TRUE(empty);
    EXPECT_EQ(empty.value(), bytes({0x80, 7}));
    for (auto mode : {p::VideoFocusMode::projected, p::VideoFocusMode::native,
                      p::VideoFocusMode::native_transient, p::VideoFocusMode::projected_no_input_focus}) {
        auto encoded = p::encode(p::VideoFocusRequest{mode});
        ASSERT_TRUE(encoded);
        auto decoded = p::decode_video_focus_request(encoded.value());
        ASSERT_TRUE(decoded);
        EXPECT_EQ(decoded.value().mode, mode);
        auto notification = p::encode(p::VideoFocusNotification{mode, false});
        ASSERT_TRUE(notification);
        aap_protobuf::service::media::video::message::VideoFocusNotification wire;
        read_reply(wire, notification.value(), 0x8008);
        EXPECT_TRUE(wire.has_focus());
        EXPECT_EQ(static_cast<int>(wire.focus()), static_cast<int>(mode));
        EXPECT_TRUE(wire.has_unsolicited());
        EXPECT_FALSE(wire.unsolicited());
    }
    auto notification = p::encode(p::VideoFocusNotification{});
    ASSERT_TRUE(notification);
    EXPECT_EQ(notification.value(), bytes({0x80, 8}));
    EXPECT_FALSE(p::encode(p::VideoFocusRequest{static_cast<p::VideoFocusMode>(0)}));
    EXPECT_FALSE(p::encode(p::VideoFocusNotification{static_cast<p::VideoFocusMode>(5), {}}));
    EXPECT_FALSE(p::encode(p::AvSinkSetup{static_cast<p::AvSinkCodec>(2)}));
}

TEST(AvSink, StrictProtobufRejectsMissingUnknownEnumsFieldsWrongWireTypesAndSizes) {
    const auto check = [](auto decode, Bytes valid, std::initializer_list<unsigned int> tags) {
        ASSERT_TRUE(decode(valid));
        auto wrong_id = valid;
        wrong_id[1] ^= std::byte{0x40};
        EXPECT_FALSE(decode(wrong_id));
        EXPECT_FALSE(decode(Bytes{}));
        EXPECT_FALSE(decode(bytes({0x80})));
        for (auto tag : tags) {
            auto bad = valid;
            bad.push_back(static_cast<std::byte>((tag << 3U) | 5U));
            bad.insert(bad.end(), 4, std::byte{0});
            EXPECT_FALSE(decode(bad)) << "wrong wire type on tag " << tag;
        }
        auto unknown = valid;
        unknown.insert(unknown.end(), {std::byte{0x78}, std::byte{0}});
        EXPECT_FALSE(decode(unknown));
        auto truncated = valid;
        truncated.push_back(std::byte{0x80});
        EXPECT_FALSE(decode(truncated));
        valid.resize((1U << 20U) + 3U);
        EXPECT_FALSE(decode(valid));
    };
    check(p::decode_av_sink_setup, bytes({0x80, 0, 8, 1}), {1});
    check(p::decode_av_sink_start, bytes({0x80, 1, 8, 0, 16, 0}), {1, 2});
    check(p::decode_av_sink_stop, bytes({0x80, 2}), {});
    check(p::decode_video_focus_request, bytes({0x80, 7, 16, 1}), {1, 2, 3});
    EXPECT_FALSE(p::decode_av_sink_setup(bytes({0x80, 0})));
    for (auto codec : {0U, 2U, 4U, 5U, 6U, 7U, 255U}) {
        EXPECT_FALSE(p::decode_av_sink_setup(bytes({0x80, 0, 8, codec})));
    }
    EXPECT_FALSE(p::decode_av_sink_start(bytes({0x80, 1, 8, 0})));
    EXPECT_FALSE(p::decode_av_sink_start(bytes({0x80, 1, 16, 0})));
    for (auto mode : {0U, 5U, 127U}) {
        EXPECT_FALSE(p::decode_video_focus_request(bytes({0x80, 7, 16, mode})));
    }
    EXPECT_FALSE(p::decode_video_focus_request(bytes({0x80, 7, 16, 1, 24, 3})));
    // Known ancillary fields are legal, but absent mode remains absent.
    auto ancillary = p::decode_video_focus_request(bytes({0x80, 7, 8, 0, 24, 2}));
    ASSERT_TRUE(ancillary);
    EXPECT_FALSE(ancillary.value().mode);
}

TEST(AvSink, DataTimestampIsUnsignedBigEndianAndBodyIsBorrowed) {
    auto payload = bytes({0, 0, 0x81, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef, 0x42});
    auto decoded = p::decode_av_sink_data(payload);
    ASSERT_TRUE(decoded);
    EXPECT_EQ(decoded.value().peer_timestamp, UINT64_C(0x8123456789abcdef));
    EXPECT_EQ(decoded.value().body.data(), payload.data() + 10);
    ASSERT_EQ(decoded.value().body.size(), 1U);
    EXPECT_EQ(decoded.value().body[0], std::byte{0x42});
    for (auto timestamp_byte : {std::byte{0}, std::byte{0xff}}) {
        std::fill(payload.begin() + 2, payload.begin() + 10, timestamp_byte);
        auto edge = p::decode_av_sink_data(payload);
        ASSERT_TRUE(edge);
        EXPECT_EQ(edge.value().peer_timestamp, timestamp_byte == std::byte{0} ? 0 : UINT64_MAX);
    }
    for (std::size_t size = 0; size <= 10; ++size) {
        EXPECT_FALSE(p::decode_av_sink_data(std::span<const std::byte>(payload).first(size)));
    }
    payload[1] = std::byte{1};
    EXPECT_FALSE(p::decode_av_sink_data(payload));
    auto config = p::decode_av_sink_codec_config(payload);
    ASSERT_TRUE(config);
    EXPECT_EQ(config.value().data(), payload.data() + 2);
    EXPECT_EQ(config.value().size(), payload.size() - 2);
    EXPECT_TRUE(p::decode_av_sink_codec_config(bytes({0, 1})));
    EXPECT_FALSE(p::decode_av_sink_codec_config(bytes({0})));
    EXPECT_FALSE(p::decode_av_sink_codec_config(bytes({0, 0})));
    payload.assign(aa::transport::kMaxMessageBytes, std::byte{0});
    EXPECT_TRUE(p::decode_av_sink_data(payload));
    payload[1] = std::byte{1};
    EXPECT_TRUE(p::decode_av_sink_codec_config(payload));
    payload.push_back(std::byte{0});
    EXPECT_FALSE(p::decode_av_sink_codec_config(payload));
    payload[1] = std::byte{0};
    EXPECT_FALSE(p::decode_av_sink_data(payload));
}
} // namespace
