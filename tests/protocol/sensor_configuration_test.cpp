// SPDX-License-Identifier: GPL-3.0-or-later
#include "../../src/protocol/aasdk_adapter/MediaConfiguration.hpp"

#include <aap_protobuf/service/control/message/ServiceDiscoveryResponse.pb.h>
#include <aap_protobuf/service/sensorsource/message/SensorType.pb.h>

#include <gtest/gtest.h>

namespace {
namespace proto = aa::protocol;
namespace adapter = proto::aasdk_adapter;
namespace sensor = aap_protobuf::service::sensorsource::message;

TEST(ControlMessages, MicrophoneSourceDescriptorAcceptsPinnedPcmConfiguration) {
    aap_protobuf::service::control::message::ServiceDiscoveryResponse wire;
    auto* entry = wire.add_channels();
    entry->set_id(106);
    auto* source = entry->mutable_media_source_service();
    source->set_available_type(aap_protobuf::service::media::shared::message::MEDIA_CODEC_AUDIO_PCM);
    source->set_available_while_in_call(false);
    auto* audio = source->mutable_audio_config();
    audio->set_sampling_rate(16000);
    audio->set_number_of_bits(16);
    audio->set_number_of_channels(1);
    const auto body = wire.SerializeAsString();
    std::vector<std::byte> payload{std::byte{0}, std::byte{6}};
    for (const char byte : body) { payload.push_back(static_cast<std::byte>(byte)); }
    const auto decoded = proto::decode_service_discovery_response(payload);
    ASSERT_TRUE(decoded);
    ASSERT_EQ(decoded.value().services.size(), 1u);
    EXPECT_EQ(decoded.value().services.front().role, proto::ChannelRole::microphone);
    const auto encoded = proto::encode(decoded.value());
    ASSERT_TRUE(encoded);
    EXPECT_EQ(encoded.value(), payload);
}

TEST(ControlMessages, NightModeSensorConfigurationRoundTripsThroughPinnedFields) {
    const proto::ServiceDescriptor descriptor{proto::ServiceKey{105}, proto::ChannelRole::sensors,
        proto::SensorConfiguration{{proto::SensorKind::night_mode}}};
    ASSERT_TRUE(proto::validate_service(descriptor));
    aap_protobuf::service::Service wire;
    wire.set_id(105);
    ASSERT_TRUE(adapter::put_configuration(wire, descriptor));
    ASSERT_TRUE(wire.has_sensor_source_service());
    ASSERT_EQ(wire.sensor_source_service().sensors_size(), 1);
    EXPECT_EQ(wire.sensor_source_service().sensors(0).sensor_type(), sensor::SENSOR_NIGHT_MODE);
    const auto decoded = adapter::read_configuration(wire, proto::ChannelRole::sensors);
    ASSERT_TRUE(decoded);
    EXPECT_EQ(decoded.value(), descriptor.configuration);

    const proto::ServiceDiscoveryResponse original{{descriptor}};
    const auto encoded = proto::encode(original);
    ASSERT_TRUE(encoded);
    const auto response = proto::decode_service_discovery_response(encoded.value());
    ASSERT_TRUE(response);
    EXPECT_EQ(response.value(), original);
}

TEST(MessageReject, SensorConfigurationRejectsEmptyUnknownDuplicateAndWrongAlternative) {
    for (const proto::ServiceConfiguration& configuration : {
        proto::ServiceConfiguration{proto::SensorConfiguration{}},
        proto::ServiceConfiguration{proto::SensorConfiguration{{static_cast<proto::SensorKind>(13)}}},
        proto::ServiceConfiguration{proto::SensorConfiguration{{proto::SensorKind::night_mode,
                                                              proto::SensorKind::night_mode}}},
        proto::ServiceConfiguration{proto::ButtonConfiguration{{85}}}}) {
        const proto::ServiceDescriptor descriptor{proto::ServiceKey{105}, proto::ChannelRole::sensors,
                                                  configuration};
        EXPECT_FALSE(proto::validate_service(descriptor));
        EXPECT_FALSE(proto::encode(proto::ServiceDiscoveryResponse{{descriptor}}));
    }
}

TEST(MessageReject, SensorConfigurationRejectsUnqualifiedPinnedSensorAndUnknownWireEnum) {
    aap_protobuf::service::Service wire;
    wire.set_id(105);
    auto* source = wire.mutable_sensor_source_service();
    EXPECT_FALSE(adapter::read_configuration(wire, proto::ChannelRole::sensors));
    auto* item = source->add_sensors();
    item->set_sensor_type(sensor::SENSOR_DRIVING_STATUS_DATA);
    EXPECT_FALSE(adapter::read_configuration(wire, proto::ChannelRole::sensors));
    ASSERT_TRUE(item->ParsePartialFromString(std::string{"\x08\x7f", 2}));
    EXPECT_FALSE(adapter::read_configuration(wire, proto::ChannelRole::sensors));
}
} // namespace
