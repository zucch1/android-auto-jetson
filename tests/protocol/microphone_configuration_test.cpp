// SPDX-License-Identifier: GPL-3.0-or-later
#include "../../src/protocol/aasdk_adapter/MediaConfiguration.hpp"

#include <aap_protobuf/service/control/message/ServiceDiscoveryResponse.pb.h>
#include <gtest/gtest.h>

namespace {
namespace proto = aa::protocol;
namespace adapter = proto::aasdk_adapter;

TEST(MessageReject, MicrophoneConfigurationRequiresExactly16k16bitMonoAndCorrectAlternative) {
    for (const proto::ServiceConfiguration& config : {
        proto::ServiceConfiguration{},
        proto::ServiceConfiguration{proto::AudioConfiguration{{{16000, 16, 1}}}},
        proto::ServiceConfiguration{proto::MicrophoneConfiguration{}},
        proto::ServiceConfiguration{proto::MicrophoneConfiguration{{48000, 16, 1}}},
        proto::ServiceConfiguration{proto::MicrophoneConfiguration{{16000, 24, 1}}},
        proto::ServiceConfiguration{proto::MicrophoneConfiguration{{16000, 16, 2}}}}) {
        const proto::ServiceDescriptor service{proto::ServiceKey{106},
                                               proto::ChannelRole::microphone, config};
        EXPECT_FALSE(proto::validate_service(service));
        EXPECT_FALSE(proto::encode(proto::ServiceDiscoveryResponse{{service}}));
    }
}

TEST(MessageReject, MicrophoneSourceRejectsMissingInvalidAndNestedUnknownConfiguration) {
    const proto::ServiceDescriptor service{proto::ServiceKey{106}, proto::ChannelRole::microphone,
                                           proto::MicrophoneConfiguration{{16000, 16, 1}}};
    aap_protobuf::service::Service valid;
    valid.set_id(106);
    ASSERT_TRUE(adapter::put_configuration(valid, service));
    auto check = [](const auto& wire) {
        EXPECT_FALSE(adapter::read_configuration(wire, proto::ChannelRole::microphone));
        aap_protobuf::service::control::message::ServiceDiscoveryResponse response;
        *response.add_channels() = wire;
        std::vector<std::byte> bytes{std::byte{0}, std::byte{6}};
        for (const char byte : response.SerializePartialAsString()) {
            bytes.push_back(static_cast<std::byte>(byte));
        }
        EXPECT_FALSE(proto::decode_service_discovery_response(bytes));
    };
    auto wire = valid;
    wire.mutable_media_source_service()->clear_audio_config();
    check(wire);
    wire = valid;
    wire.mutable_media_source_service()->mutable_audio_config()->clear_sampling_rate();
    check(wire);
    wire = valid;
    wire.mutable_media_source_service()->set_available_while_in_call(true);
    check(wire);
    wire = valid;
    wire.mutable_media_source_service()->set_available_type(
        aap_protobuf::service::media::shared::message::MEDIA_CODEC_VIDEO_H264_BP);
    check(wire);
    wire = valid;
    wire.mutable_media_source_service()->mutable_audio_config()->set_number_of_channels(2);
    check(wire);
    for (int level = 0; level < 3; ++level) {
        const int last_tag = level == 0 ? 14 : 3;
        for (int tag = 1; tag <= last_tag + 1; ++tag) {
            wire = valid;
            google::protobuf::Message* nested = &wire;
            if (level >= 1) { nested = wire.mutable_media_source_service(); }
            if (level == 2) { nested = wire.mutable_media_source_service()->mutable_audio_config(); }
            nested->GetReflection()->MutableUnknownFields(nested)->AddFixed32(tag, 0);
            check(wire);
        }
    }
}
} // namespace
