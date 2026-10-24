// SPDX-License-Identifier: GPL-3.0-or-later
#include "payload_builder.hpp"

#include <aa/protocol/Messages.hpp>
#include <aa/protocol/WifiProjection.hpp>

#include <aap_protobuf/service/control/message/ServiceDiscoveryResponse.pb.h>
#include <aap_protobuf/service/wifiprojection/message/WifiCredentialsResponse.pb.h>
#include <gtest/gtest.h>

namespace {
namespace proto = aa::protocol;
namespace ctl = aap_protobuf::service::control::message;
namespace wifi = aap_protobuf::service::wifiprojection::message;

TEST(WifiProjection, DescriptorRoundTripsWhenIdIsNonOrdinal) {
    // Given: a pinned Wi-Fi descriptor using dynamic Service.id 73, not ordinal 18.
    ctl::ServiceDiscoveryResponse wire;
    auto* entry = wire.add_channels();
    entry->set_id(73);
    entry->mutable_wifi_projection_service()->set_car_wifi_bssid("02:00:00:00:00:01");
    const auto payload = testwire::control_payload(6, wire.SerializeAsString());
    // When: the public adapter decodes and re-encodes discovery.
    const auto decoded = proto::decode_service_discovery_response(payload);
    // Then: identity, role, configuration and bytes survive.
    ASSERT_TRUE(decoded);
    ASSERT_EQ(decoded.value().services.size(), 1u);
    EXPECT_EQ(decoded.value().services.front().id, proto::ServiceKey{73});
    EXPECT_EQ(decoded.value().services.front().role, proto::ChannelRole::wifi_projection);
    EXPECT_EQ(std::get<proto::WifiProjectionConfiguration>(
        decoded.value().services.front().configuration).car_wifi_bssid,
        "02:00:00:00:00:01");
    const auto encoded = proto::encode(decoded.value());
    ASSERT_TRUE(encoded);
    EXPECT_EQ(encoded.value(), payload);
}

TEST(WifiProjection, DescriptorAcceptsAbsentAndPresentEmptyBssid) {
    // Given: both schema-valid optional string states.
    for (const auto& bssid : {std::optional<std::string>{}, std::optional<std::string>{""}}) {
        const proto::ServiceDiscoveryResponse original{{{proto::ServiceKey{73},
            proto::ChannelRole::wifi_projection, proto::WifiProjectionConfiguration{bssid}}}};
        // When: discovery roundtrips.
        const auto encoded = proto::encode(original);
        ASSERT_TRUE(encoded);
        const auto decoded = proto::decode_service_discovery_response(encoded.value());
        // Then: optional-field presence is preserved.
        ASSERT_TRUE(decoded);
        EXPECT_EQ(decoded.value(), original);
    }
}

TEST(WifiProjection, DescriptorRejectsMissingRequiredIdMalformedAndAmbiguousBodies) {
    // Given: invalid nested Service bodies (required ID, truncated BSSID, two service kinds).
    for (const std::string& body : {std::string{"\x0a\x02\x72\x00", 4},
             std::string{"\x0a\x07\x08\x49\x72\x03\x0a\x02\x61", 9},
             std::string{"\x0a\x06\x08\x49\x22\x00\x72\x00", 8}}) {
        // When: discovery is decoded.
        const auto result = proto::decode_service_discovery_response(testwire::control_payload(6, body));
        // Then: every invalid body fails closed.
        ASSERT_FALSE(result);
        EXPECT_EQ(result.error().code(), aa::ErrorCode::protocol_malformed_message);
    }
}

TEST(WifiProjection, DescriptorRejectsUnknownFieldsAndWrongConfiguration) {
    // Given: unknown nested fields, including a known BSSID tag with the wrong wire type.
    for (const std::string& nested : {std::string{"\x10\x01", 2}, std::string{"\x08\x01", 2}}) {
        ctl::ServiceDiscoveryResponse wire;
        auto* entry = wire.add_channels();
        entry->set_id(73);
        ASSERT_TRUE(entry->mutable_wifi_projection_service()->ParseFromString(nested));
        // When: discovery is decoded under the existing strict extension policy.
        const auto result = proto::decode_service_discovery_response(
            testwire::control_payload(6, wire.SerializeAsString()));
        // Then: no unknown field silently becomes a default configuration.
        ASSERT_FALSE(result);
        EXPECT_EQ(result.error().code(), aa::ErrorCode::protocol_malformed_message);
    }
    const proto::ServiceDescriptor wrong{proto::ServiceKey{73}, proto::ChannelRole::wifi_projection,
        proto::ButtonConfiguration{{85}}};
    EXPECT_FALSE(proto::validate_service(wrong));
    EXPECT_FALSE(proto::encode(proto::ServiceDiscoveryResponse{{wrong}}));
    const proto::ServiceDescriptor wrong_role{proto::ServiceKey{74}, proto::ChannelRole::input,
        proto::WifiProjectionConfiguration{}};
    EXPECT_FALSE(proto::encode(proto::ServiceDiscoveryResponse{{wrong_role}}));
}

TEST(WifiProjection, CredentialsRequestAcceptsEmptyPinnedBodyOnWifiRole) {
    // Given: the pinned empty request, prefixed with message ID 32769.
    const auto payload = testwire::control_payload(32769, "");
    const proto::MessageView message{{proto::ChannelRole::wifi_projection, 0, {}}, payload};
    // When: the service-specific decoder receives it.
    const auto decoded = proto::decode_wifi_credentials_request(message);
    // Then: an empty request is valid (there are no required credential fields).
    ASSERT_TRUE(decoded);
}

TEST(WifiProjection, CredentialsRequestRejectsWrongServiceAndMalformedBodies) {
    // Given: short prefixes, wrong ID, truncated protobuf, unknown field and oversize bodies.
    for (const auto& payload : {std::vector<std::byte>{}, std::vector<std::byte>{std::byte{0x80}},
             testwire::control_payload(32770, ""), testwire::control_payload(32769, "\x80"),
             testwire::control_payload(32769, std::string{"\x08\x01", 2}),
             testwire::control_payload(32769, std::string((1U << 20U) + 1U, '\0'))}) {
        // When: the Wi-Fi decoder receives each invalid payload.
        const auto result = proto::decode_wifi_credentials_request(
            {{proto::ChannelRole::wifi_projection, 0, {}}, payload});
        // Then: all fail closed.
        ASSERT_FALSE(result);
        EXPECT_EQ(result.error().code(), aa::ErrorCode::protocol_malformed_message);
    }
    const auto payload = testwire::control_payload(32769, "");
    const auto wrong = proto::decode_wifi_credentials_request(
        {{proto::ChannelRole::sensors, 0, {}}, payload});
    ASSERT_FALSE(wrong);
    EXPECT_EQ(wrong.error().code(), aa::ErrorCode::protocol_unsupported_channel);
}

TEST(WifiProjection, CredentialsResponseMapsMetadataWithoutBootstrapSecrets) {
    // Given: metadata for the receiver-hosted AP, with no credential-bearing project fields.
    const proto::WifiCredentialsResponse response{proto::WifiSecurityMode::wpa2_personal,
        {36, 44}, proto::WifiAccessPointType::dynamic};
    // When: the response is encoded.
    const auto encoded = proto::encode(response);
    // Then: pinned message ID, field tags and negotiated enum values are exact.
    ASSERT_TRUE(encoded);
    EXPECT_EQ(encoded.value(), testwire::control_payload(32770,
        std::string{"\x10\x05\x20\x24\x20\x2c\x28\x01", 8}));
    wifi::WifiCredentialsResponse wire;
    ASSERT_TRUE(wire.ParseFromArray(encoded.value().data() + 2,
        static_cast<int>(encoded.value().size() - 2)));
    EXPECT_EQ(wire.car_wifi_security_mode(), wifi::WPA2_PERSONAL);
    EXPECT_EQ(wire.access_point_type(), wifi::DYNAMIC);
    EXPECT_EQ(wire.supported_wifi_channels_size(), 2);
    EXPECT_EQ(wire.supported_wifi_channels(0), 36);
    EXPECT_EQ(wire.supported_wifi_channels(1), 44);
    EXPECT_FALSE(wire.has_car_wifi_password());
    EXPECT_FALSE(wire.has_car_wifi_ssid());
}

TEST(WifiProjection, CredentialsResponsePreservesOptionalPresenceAndRejectsUnknownEnums) {
    // Given: a response with every optional field absent, as permitted by the schema.
    const proto::WifiCredentialsResponse response;
    // When: it is encoded.
    const auto encoded = proto::encode(response);
    // Then: no defaults are fabricated; out-of-schema project enum values fail closed.
    ASSERT_TRUE(encoded);
    EXPECT_EQ(encoded.value(), testwire::control_payload(32770, ""));
    EXPECT_FALSE(proto::encode(proto::WifiCredentialsResponse{
        static_cast<proto::WifiSecurityMode>(99), {}, {}}));
    EXPECT_FALSE(proto::encode(proto::WifiCredentialsResponse{
        {}, {}, static_cast<proto::WifiAccessPointType>(99)}));
}
} // namespace
