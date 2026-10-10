// SPDX-License-Identifier: GPL-3.0-or-later
// Known-message round trips (task 15): every supported control message
// round-trips through the real adapter and the pinned generated protobuf code,
// payloads built independently (SDK framing + generated serialization) decode
// to the project-owned value, and the adapter framing is byte-identical to the
// SDK's MessageId framing.
#include "payload_builder.hpp"

#include <aap_protobuf/service/control/message/ChannelOpenRequest.pb.h>
#include <aap_protobuf/service/control/message/PingRequest.pb.h>
#include <aap_protobuf/service/control/message/PingResponse.pb.h>
#include <aap_protobuf/service/control/message/ServiceDiscoveryResponse.pb.h>
#include <aap_protobuf/service/media/sink/message/AudioStreamType.pb.h>

#include <aa/protocol/Messages.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <gtest/gtest.h>

namespace {

namespace proto = aa::protocol;
namespace ctl = aap_protobuf::service::control::message;
namespace sink = aap_protobuf::service::media::sink::message;

} // namespace

TEST(ControlMessages, VersionRequestRoundTripsAndDecodesKnownBytes) {
    // Given: a project-owned version request.
    const proto::VersionRequest original{300, 1};
    // When: it round-trips through the adapter.
    const auto payload = proto::encode(original);
    ASSERT_TRUE(payload.has_value());
    const auto decoded = proto::decode_version_request(payload.value());
    // Then: the value is preserved and the wire shape is the known 6 bytes.
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(decoded.value(), original);
    ASSERT_EQ(payload.value().size(), 6u);
    const std::vector<std::byte> known{
        std::byte{0x00}, std::byte{0x01}, std::byte{0x01},
        std::byte{0x2C}, std::byte{0x00}, std::byte{0x01}};
    EXPECT_EQ(payload.value(), known);
}

TEST(ControlMessages, PingRequestRoundTrips) {
    // Given: a ping request with optional fields populated.
    const proto::PingRequest original{987654321, true, {std::byte{0xDE}, std::byte{0xAD}}};
    // When: it round-trips through the adapter.
    const auto payload = proto::encode(original);
    ASSERT_TRUE(payload.has_value());
    const auto decoded = proto::decode_ping_request(payload.value());
    // Then: every field survives.
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(decoded.value(), original);
}

TEST(ControlMessages, PingRequestDecodesIndependentProtobuf) {
    // Given: bytes built by generated code and SDK framing, not by the adapter.
    ctl::PingRequest wire;
    wire.set_timestamp(42);
    wire.set_bug_report(true);
    wire.set_data(std::string("\x01\x02", 2));
    std::string body;
    ASSERT_TRUE(wire.SerializeToString(&body));
    const auto payload =
        testwire::control_payload(static_cast<std::uint16_t>(proto::ControlMessageId::ping_request),
                                  body);
    // When: the adapter decodes them.
    const auto decoded = proto::decode_ping_request(payload);
    // Then: the project-owned value matches field for field.
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(decoded.value().timestamp, 42);
    EXPECT_TRUE(decoded.value().bug_report);
    EXPECT_EQ(decoded.value().data.size(), 2u);
}

TEST(ControlMessages, PingResponseRoundTrips) {
    // Given: a ping response echoing a timestamp and opaque data.
    const proto::PingResponse original{55, {std::byte{0x01}}};
    // When: it round-trips through the adapter.
    const auto payload = proto::encode(original);
    ASSERT_TRUE(payload.has_value());
    const auto decoded = proto::decode_ping_response(payload.value());
    // Then: the value is preserved.
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(decoded.value(), original);
}

TEST(ControlMessages, ChannelOpenRequestCarriesDynamicServiceId) {
    // Given: a channel open request naming dynamic service 99.
    const proto::ChannelOpenRequest original{1000, proto::ServiceKey{99}};
    // When: it round-trips through the adapter.
    const auto payload = proto::encode(original);
    ASSERT_TRUE(payload.has_value());
    const auto decoded = proto::decode_channel_open_request(payload.value());
    // Then: the dynamic id (ID space 2) survives untouched.
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(decoded.value(), original);
    EXPECT_EQ(decoded.value().service.value, 99);
}

TEST(ControlMessages, ServiceDiscoveryResponseRoundTripsDynamicIds) {
    // Given: qualified media and non-touch button descriptors with dynamic IDs.
    const proto::ServiceDiscoveryResponse original{
        {{proto::ServiceKey{99}, proto::ChannelRole::video,
          proto::VideoConfiguration{{{1280, 720, 30}, {800, 480, 30}}}},
         {proto::ServiceKey{3}, proto::ChannelRole::media_audio,
          proto::AudioConfiguration{{{48000, 16, 2}}}},
         {proto::ServiceKey{4}, proto::ChannelRole::guidance_audio,
          proto::AudioConfiguration{{{16000, 16, 1}}}},
         {proto::ServiceKey{5}, proto::ChannelRole::system_audio,
          proto::AudioConfiguration{{{48000, 16, 2}}}},
         {proto::ServiceKey{6}, proto::ChannelRole::speech_audio,
          proto::AudioConfiguration{{{16000, 16, 1}}}},
         {proto::ServiceKey{7}, proto::ChannelRole::input, proto::ButtonConfiguration{{85}}}}};
    // When: it round-trips through the adapter.
    const auto payload = proto::encode(original);
    ASSERT_TRUE(payload.has_value());
    const auto decoded = proto::decode_service_discovery_response(payload.value());
    // Then: every (dynamic id, role) pair survives in order.
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(decoded.value(), original);
}

TEST(ControlMessages, ServiceDiscoveryClassifiesIndependentEntryKinds) {
    // Given: entries built by generated code with mixed service kinds.
    ctl::ServiceDiscoveryResponse wire;
    auto* sink_entry = wire.add_channels();
    sink_entry->set_id(22);
    sink_entry->mutable_media_sink_service()->set_audio_type(sink::AUDIO_STREAM_GUIDANCE);
    auto* audio = sink_entry->mutable_media_sink_service()->add_audio_configs();
    audio->set_sampling_rate(16000);
    audio->set_number_of_bits(16);
    audio->set_number_of_channels(1);
    auto* video = wire.add_channels();
    video->set_id(23);
    video->mutable_media_sink_service()->set_available_type(
        aap_protobuf::service::media::shared::message::MEDIA_CODEC_VIDEO_H264_BP);
    auto* video_config = video->mutable_media_sink_service()->add_video_configs();
    video_config->set_codec_resolution(sink::VIDEO_1280x720);
    video_config->set_frame_rate(sink::VIDEO_FPS_30);
    video_config->set_video_codec_type(
        aap_protobuf::service::media::shared::message::MEDIA_CODEC_VIDEO_H264_BP);
    std::string body;
    ASSERT_TRUE(wire.SerializeToString(&body));
    const auto payload = testwire::control_payload(
        static_cast<std::uint16_t>(proto::ControlMessageId::service_discovery_response), body);
    // When: the adapter classifies them.
    const auto decoded = proto::decode_service_discovery_response(payload);
    // Then: dynamic ids and stream roles are classified, not conflated.
    ASSERT_TRUE(decoded.has_value());
    ASSERT_EQ(decoded.value().services.size(), 2u);
    EXPECT_EQ(decoded.value().services[0],
              (proto::ServiceDescriptor{proto::ServiceKey{22}, proto::ChannelRole::guidance_audio,
                                       proto::AudioConfiguration{{{16000, 16, 1}}}}));
    EXPECT_EQ(decoded.value().services[1],
              (proto::ServiceDescriptor{proto::ServiceKey{23}, proto::ChannelRole::video,
                                       proto::VideoConfiguration{{{1280, 720, 30}}}}));
}

TEST(ControlMessages, AdapterFramingMatchesSdkMessageIdBytes) {
    // Given: payloads framed by the adapter for every known message.
    const auto version = proto::encode(proto::VersionRequest{1, 2});
    const auto ping = proto::encode(proto::PingRequest{1, false, {}});
    const auto response = proto::encode(proto::PingResponse{1, {}});
    const auto open = proto::encode(proto::ChannelOpenRequest{1, proto::ServiceKey{1}});
    const auto discovery = proto::encode(proto::ServiceDiscoveryResponse{
        {{proto::ServiceKey{1}, proto::ChannelRole::input, proto::ButtonConfiguration{{85}}}}});
    const std::vector<const std::vector<std::byte>*> payloads{
        &version.value(), &ping.value(), &response.value(), &open.value(), &discovery.value()};
    const std::uint16_t expected_ids[] = {1, 11, 12, 7, 6};
    // When/Then: the leading two bytes equal the SDK's MessageId framing.
    for (std::size_t index = 0; index < payloads.size(); ++index) {
        const aasdk::messenger::MessageId reference{expected_ids[index]};
        const aasdk::common::Data sdk_bytes = reference.getData();
        ASSERT_EQ(sdk_bytes.size(), 2u);
        const auto& payload = *payloads[index];
        ASSERT_GE(payload.size(), 2u);
        EXPECT_EQ(static_cast<unsigned>(payload[0]), unsigned{sdk_bytes[0]}) << index;
        EXPECT_EQ(static_cast<unsigned>(payload[1]), unsigned{sdk_bytes[1]}) << index;
    }
}
