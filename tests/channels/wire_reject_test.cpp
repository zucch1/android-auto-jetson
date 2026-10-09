// SPDX-License-Identifier: GPL-3.0-or-later
#include <aa/channels/Negotiator.hpp>
#include "../protocol/payload_builder.hpp"
#include <aap_protobuf/service/control/message/ChannelOpenResponse.pb.h>
#include <aap_protobuf/service/control/message/ServiceDiscoveryResponse.pb.h>
#include <gtest/gtest.h>

namespace {
namespace proto = aa::protocol;
namespace ctl = aap_protobuf::service::control::message;
namespace sink = aap_protobuf::service::media::sink::message;
namespace shared = aap_protobuf::service::media::shared::message;

std::vector<std::byte> payload(const google::protobuf::MessageLite& message,
                              proto::ControlMessageId id = proto::ControlMessageId::service_discovery_response) {
    std::string body;
    EXPECT_TRUE(message.SerializePartialToString(&body));
    return testwire::control_payload(static_cast<std::uint16_t>(id), body);
}

ctl::ServiceDiscoveryResponse wire_video() {
    ctl::ServiceDiscoveryResponse response;
    auto* entry = response.add_channels();
    entry->set_id(99);
    auto* media = entry->mutable_media_sink_service();
    media->set_available_type(shared::MEDIA_CODEC_VIDEO_H264_BP);
    auto* video_config = media->add_video_configs();
    video_config->set_codec_resolution(sink::VIDEO_1280x720);
    video_config->set_frame_rate(sink::VIDEO_FPS_30);
    video_config->set_video_codec_type(shared::MEDIA_CODEC_VIDEO_H264_BP);
    return response;
}

TEST(ChannelWireReject, RejectsWireDuplicateIdsRolesAndBounds) {
    // Given: independent wire entries with collisions or IDs outside the byte space.
    for (const int scenario : {0, 1, 2, 3, 4}) {
        auto wire = wire_video();
        if (scenario <= 1) {
            auto* second = wire.add_channels();
            second->set_id(scenario == 0 ? 99 : 100);
            if (scenario == 0) {
                second->mutable_input_source_service()->add_keycodes_supported(85);
            } else {
                *second->mutable_media_sink_service() = wire.channels(0).media_sink_service();
            }
        } else {
            wire.mutable_channels(0)->set_id(scenario == 2 ? 0 : scenario == 3 ? 256 : -1);
        }
        // When: untrusted discovery bytes are decoded.
        const auto decoded = proto::decode_service_discovery_response(payload(wire));
        // Then: no ambiguous or unrepresentable route enters the project boundary.
        ASSERT_FALSE(decoded) << scenario;
        EXPECT_EQ(decoded.error().code(), aa::ErrorCode::protocol_malformed_message);
    }
}

TEST(ChannelWireReject, RejectsMissingVideoFieldsMarginsCodecAndProfileFlood) {
    // Given: configurations malformed independently of the project encoder.
    for (const int scenario : {0, 1, 2, 3, 4, 5, 6}) {
        auto wire = wire_video();
        auto* media = wire.mutable_channels(0)->mutable_media_sink_service();
        auto* config = media->mutable_video_configs(0);
        switch (scenario) {
        case 0: config->clear_codec_resolution(); break;
        case 1: config->clear_frame_rate(); break;
        case 2: config->set_video_codec_type(shared::MEDIA_CODEC_VIDEO_VP9); break;
        case 3: config->set_width_margin(1); break;
        case 4: config->set_codec_resolution(sink::VIDEO_1920x1080); break;
        case 5: config->set_frame_rate(sink::VIDEO_FPS_60); break;
        case 6:
            for (std::size_t i = 0; i < proto::kMaxMediaProfiles; ++i) {
                *media->add_video_configs() = media->video_configs(0);
            }
            break;
        }
        // When: the malformed bytes are decoded.
        const auto decoded = proto::decode_service_discovery_response(payload(wire));
        // Then: codec-only placeholders and unsupported configurations fail closed.
        ASSERT_FALSE(decoded) << scenario;
        EXPECT_EQ(decoded.error().code(), aa::ErrorCode::protocol_malformed_message);
    }
}

TEST(ChannelWireReject, RejectsMissingAudioFieldsAndOutOfRangeNumbers) {
    // Given: independently built audio configuration violations.
    for (const int scenario : {0, 1, 2, 3, 4, 5}) {
        ctl::ServiceDiscoveryResponse wire;
        auto* entry = wire.add_channels();
        entry->set_id(30);
        auto* media = entry->mutable_media_sink_service();
        media->set_audio_type(sink::AUDIO_STREAM_MEDIA);
        auto* audio_config = media->add_audio_configs();
        audio_config->set_sampling_rate(scenario == 1 ? 0 : 48000);
        audio_config->set_number_of_bits(scenario == 2 ? 256 : 16);
        audio_config->set_number_of_channels(scenario == 3 ? 0 : 2);
        if (scenario == 0) { audio_config->clear_sampling_rate(); }
        if (scenario == 4) { media->set_available_type(shared::MEDIA_CODEC_AUDIO_AAC_LC); }
        if (scenario == 5) { media->mutable_unknown_fields()->AddVarint(1, 99); }
        // When: the real decoder handles the wire body.
        const auto decoded = proto::decode_service_discovery_response(payload(wire));
        // Then: missing fields, overflow and unavailable codecs are rejected.
        ASSERT_FALSE(decoded) << scenario;
        EXPECT_EQ(decoded.error().code(), aa::ErrorCode::protocol_malformed_message);
    }
}

TEST(ChannelWireReject, RejectsPlaceholderAndDuplicateProjectConfigurations) {
    // Given: role/config mismatches and malformed typed configurations.
    const std::vector<proto::ServiceDescriptor> bad{
        {proto::ServiceKey{99}, proto::ChannelRole::video},
        {proto::ServiceKey{99}, proto::ChannelRole::video, proto::VideoConfiguration{{{1280, 720, 60}}}},
        {proto::ServiceKey{99}, proto::ChannelRole::video,
         proto::VideoConfiguration{{{1280, 720, 30}, {1280, 720, 30}}}},
        {proto::ServiceKey{99}, proto::ChannelRole::media_audio, proto::AudioConfiguration{{{0, 16, 2}}}},
        {proto::ServiceKey{99}, proto::ChannelRole::input, proto::ButtonConfiguration{{85, 85}}},
        {proto::ServiceKey{99}, proto::ChannelRole::input, proto::ButtonConfiguration{{}}},
        {proto::ServiceKey{99}, proto::ChannelRole::video, proto::AudioConfiguration{{{48000, 16, 2}}}}};
    // When/Then: neither configuration creation nor wire encoding publishes these.
    for (const auto& descriptor : bad) {
        EXPECT_FALSE(aa::channels::SupportConfiguration::create({descriptor}));
        EXPECT_FALSE(proto::encode(proto::ServiceDiscoveryResponse{{descriptor}}));
    }
}

TEST(ChannelWireReject, RefusesUnqualifiedHardwareAndLocalDiagnosticsOnWire) {
    // Given: no qualified hardware configurations or diagnostics wire kind exist.
    for (const auto role : {proto::ChannelRole::microphone, proto::ChannelRole::sensors,
         proto::ChannelRole::bluetooth_projection, proto::ChannelRole::wifi_projection,
         proto::ChannelRole::diagnostics}) {
        // When: a caller tries to advertise a placeholder role.
        const auto encoded = proto::encode(proto::ServiceDiscoveryResponse{{{proto::ServiceKey{99}, role}}});
        // Then: no fabricated address, transport or sensor capability is emitted.
        ASSERT_FALSE(encoded);
        EXPECT_EQ(encoded.error().code(), aa::ErrorCode::protocol_unsupported_channel);
    }
}

TEST(ChannelWireReject, RejectsOpenResponseMissingStatusWrongDiscriminatorAndUnsupportedStatus) {
    // Given: invalid response bodies and a supported body under the wrong message ID.
    ctl::ChannelOpenResponse wire;
    EXPECT_FALSE(proto::decode_channel_open_response(payload(wire, proto::ControlMessageId::channel_open_response)));
    wire.set_status(aap_protobuf::shared::STATUS_INTERNAL_ERROR);
    // When/Then: statuses outside the project open contract are rejected.
    EXPECT_FALSE(proto::decode_channel_open_response(payload(wire, proto::ControlMessageId::channel_open_response)));
    wire.set_status(aap_protobuf::shared::STATUS_SUCCESS);
    EXPECT_FALSE(proto::decode_channel_open_response(payload(wire, proto::ControlMessageId::ping_response)));
}
} // namespace
