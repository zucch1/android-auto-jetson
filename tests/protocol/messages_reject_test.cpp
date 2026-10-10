// SPDX-License-Identifier: GPL-3.0-or-later
// Malformed-payload rejection (task 15): the adapter fails closed on truncated
// payloads, wrong discriminators, missing proto2 required fields, semantic-bound
// violations and unclassifiable service entries, always with the typed
// protocol_malformed_message error.
#include "payload_builder.hpp"

#include <aap_protobuf/service/control/message/ChannelOpenRequest.pb.h>
#include <aap_protobuf/service/control/message/PingRequest.pb.h>
#include <aap_protobuf/service/control/message/ServiceDiscoveryResponse.pb.h>
#include <aap_protobuf/service/Service.pb.h>

#include <aa/protocol/Messages.hpp>

#include <cstddef>
#include <string>
#include <vector>

#include <gtest/gtest.h>

namespace {

namespace proto = aa::protocol;
namespace ctl = aap_protobuf::service::control::message;

void expect_malformed(const aa::core::Result<std::vector<std::byte>>& result) {
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code(), aa::ErrorCode::protocol_malformed_message);
}

} // namespace

TEST(MessageReject, RejectsShortAndEmptyPayloads) {
    // Given: payloads too short to carry a discriminator.
    const std::vector<std::byte> empty{};
    const std::vector<std::byte> one_byte{std::byte{0x00}};
    // When/Then: every decode fails typed.
    for (const auto* payload : {&empty, &one_byte}) {
        const auto version = proto::decode_version_request(*payload);
        ASSERT_FALSE(version.has_value());
        EXPECT_EQ(version.error().code(), aa::ErrorCode::protocol_malformed_message);
        const auto ping = proto::decode_ping_request(*payload);
        ASSERT_FALSE(ping.has_value());
        EXPECT_EQ(ping.error().code(), aa::ErrorCode::protocol_malformed_message);
    }
}

TEST(MessageReject, RejectsWrongDiscriminator) {
    // Given: a well-formed ping body under the version-request discriminator.
    ctl::PingRequest wire;
    wire.set_timestamp(1);
    std::string body;
    ASSERT_TRUE(wire.SerializeToString(&body));
    const auto payload =
        testwire::control_payload(static_cast<std::uint16_t>(proto::ControlMessageId::version_request),
                                  body);
    // When: the ping decoder reads it.
    const auto result = proto::decode_ping_request(payload);
    // Then: the mismatched id space value is rejected.
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code(), aa::ErrorCode::protocol_malformed_message);
}

TEST(MessageReject, RejectsPingMissingRequiredTimestamp) {
    // Given: proto2 bytes with the required timestamp absent.
    ctl::PingRequest wire;
    wire.set_bug_report(true);
    std::string body;
    ASSERT_TRUE(wire.SerializePartialToString(&body));
    const auto payload =
        testwire::control_payload(static_cast<std::uint16_t>(proto::ControlMessageId::ping_request),
                                  body);
    // When: the adapter parses them.
    const auto result = proto::decode_ping_request(payload);
    // Then: the missing required field is a typed rejection.
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code(), aa::ErrorCode::protocol_malformed_message);
}

TEST(MessageReject, RejectsPingDataPastSemanticBound) {
    // Given: a ping whose opaque data exceeds kMaxPingDataBytes.
    proto::PingRequest message{1, false, std::vector<std::byte>(proto::kMaxPingDataBytes + 1)};
    // When/Then: both directions reject the same bound.
    expect_malformed(proto::encode(message));
    const proto::PingRequest inbound{1, false, std::vector<std::byte>(proto::kMaxPingDataBytes)};
    ASSERT_TRUE(proto::encode(inbound).has_value());
}

TEST(MessageReject, RejectsGarbageAndWrongSizedBodies) {
    // Given: a garbage protobuf body and a version body of the wrong size.
    const auto garbage = testwire::control_payload(
        static_cast<std::uint16_t>(proto::ControlMessageId::ping_request), std::string(1, '\xFF'));
    const auto short_version =
        testwire::control_payload(static_cast<std::uint16_t>(proto::ControlMessageId::version_request),
                                  std::string(3, '\x00'));
    // When/Then: parsing fails closed.
    const auto ping = proto::decode_ping_request(garbage);
    ASSERT_FALSE(ping.has_value());
    EXPECT_EQ(ping.error().code(), aa::ErrorCode::protocol_malformed_message);
    const auto version = proto::decode_version_request(short_version);
    ASSERT_FALSE(version.has_value());
    EXPECT_EQ(version.error().code(), aa::ErrorCode::protocol_malformed_message);
}

TEST(MessageReject, RejectsChannelOpenWithoutValidServiceId) {
    // Given: a request missing the required service id and one with id zero.
    ctl::ChannelOpenRequest missing;
    missing.set_priority(1000);
    std::string body;
    ASSERT_TRUE(missing.SerializePartialToString(&body));
    const auto missing_payload = testwire::control_payload(
        static_cast<std::uint16_t>(proto::ControlMessageId::channel_open_request), body);
    // When/Then: both are typed rejections, so id 0 never enters the space.
    const auto absent = proto::decode_channel_open_request(missing_payload);
    ASSERT_FALSE(absent.has_value());
    EXPECT_EQ(absent.error().code(), aa::ErrorCode::protocol_malformed_message);
    const auto zero = proto::encode(proto::ChannelOpenRequest{1, proto::ServiceKey{0}});
    ASSERT_FALSE(zero.has_value());
    EXPECT_EQ(zero.error().code(), aa::ErrorCode::protocol_malformed_message);
}

TEST(MessageReject, RejectsServiceEntryMissingRequiredId) {
    // Given: a discovery response whose entry omits the required id.
    ctl::ServiceDiscoveryResponse wire;
    wire.add_channels()->mutable_sensor_source_service();
    std::string body;
    ASSERT_TRUE(wire.SerializePartialToString(&body));
    const auto payload = testwire::control_payload(
        static_cast<std::uint16_t>(proto::ControlMessageId::service_discovery_response), body);
    // When: the adapter parses them.
    const auto result = proto::decode_service_discovery_response(payload);
    // Then: the nested required field is enforced.
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code(), aa::ErrorCode::protocol_malformed_message);
}

TEST(MessageReject, RejectsZeroAndOversizedServiceIdentity) {
    // Given: dynamic service id zero and too many advertised services.
    const proto::ServiceDiscoveryResponse zero_id{
        {{proto::ServiceKey{0}, proto::ChannelRole::sensors}}};
    expect_malformed(proto::encode(zero_id));
    proto::ServiceDiscoveryResponse crowded;
    for (std::size_t index = 0; index <= proto::kMaxDiscoveredServices; ++index) {
        crowded.services.push_back(
            {proto::ServiceKey{static_cast<std::int32_t>(index) + 1}, proto::ChannelRole::sensors});
    }
    expect_malformed(proto::encode(crowded));
}

TEST(MessageReject, RejectsAmbiguousAndUnclassifiableServiceEntries) {
    // Given: entries with two service kinds, zero kinds, or a media sink that
    // names both streams and no stream at all.
    ctl::ServiceDiscoveryResponse two_kinds;
    auto* both = two_kinds.add_channels();
    both->set_id(1);
    both->mutable_sensor_source_service();
    both->mutable_input_source_service();
    ctl::ServiceDiscoveryResponse zero_kinds;
    zero_kinds.add_channels()->set_id(1);
    ctl::ServiceDiscoveryResponse both_streams;
    auto* sink_entry = both_streams.add_channels();
    sink_entry->set_id(1);
    sink_entry->mutable_media_sink_service()->set_audio_type(
        aap_protobuf::service::media::sink::message::AUDIO_STREAM_MEDIA);
    sink_entry->mutable_media_sink_service()->add_video_configs();
    ctl::ServiceDiscoveryResponse no_stream;
    no_stream.add_channels()->mutable_media_sink_service();
    no_stream.mutable_channels(0)->set_id(1);
    for (const auto* wire : {&two_kinds, &zero_kinds, &both_streams, &no_stream}) {
        std::string body;
        ASSERT_TRUE(wire->SerializePartialToString(&body));
        const auto payload = testwire::control_payload(
            static_cast<std::uint16_t>(proto::ControlMessageId::service_discovery_response), body);
        // When/Then: every unclassifiable entry is rejected.
        const auto result = proto::decode_service_discovery_response(payload);
        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code(), aa::ErrorCode::protocol_malformed_message);
    }
}

TEST(MessageReject, RejectsUnsupportedServiceKindTyped) {
    // Given: a well-formed radio service entry (no dispatch role).
    ctl::ServiceDiscoveryResponse wire;
    auto* radio = wire.add_channels();
    radio->set_id(9);
    radio->mutable_radio_service();
    std::string body;
    ASSERT_TRUE(wire.SerializeToString(&body));
    const auto payload = testwire::control_payload(
        static_cast<std::uint16_t>(proto::ControlMessageId::service_discovery_response), body);
    // When: the adapter classifies it.
    const auto result = proto::decode_service_discovery_response(payload);
    // Then: the typed error names the unsupported channel, not a parse fault.
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code(), aa::ErrorCode::protocol_unsupported_channel);
}

TEST(MessageReject, RejectsBluetoothServiceWithoutCarAddress) {
    // Given: a bluetooth entry missing its proto2 required car_address.
    ctl::ServiceDiscoveryResponse wire;
    wire.add_channels()->set_id(3);
    wire.mutable_channels(0)->mutable_bluetooth_service();
    std::string body;
    ASSERT_TRUE(wire.SerializePartialToString(&body));
    const auto payload = testwire::control_payload(
        static_cast<std::uint16_t>(proto::ControlMessageId::service_discovery_response), body);
    // When: the adapter parses them.
    const auto result = proto::decode_service_discovery_response(payload);
    // Then: the nested required field is a typed rejection.
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code(), aa::ErrorCode::protocol_malformed_message);
}

TEST(MessageReject, EncodeRefusesBluetoothPlaceholder) {
    // Given: bluetooth_projection whose wire entry requires an address the
    // project type deliberately does not carry.
    const proto::ServiceDiscoveryResponse original{
        {{proto::ServiceKey{1}, proto::ChannelRole::bluetooth_projection}}};
    // When: the adapter encodes it.
    const auto result = proto::encode(original);
    // Then: it refuses rather than fabricating identity content.
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code(), aa::ErrorCode::protocol_unsupported_channel);
}
