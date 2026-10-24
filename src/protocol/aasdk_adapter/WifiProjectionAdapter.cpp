// SPDX-License-Identifier: GPL-3.0-or-later
#include "Framing.hpp"
#include "UnknownFields.hpp"

#include <aa/protocol/WifiProjection.hpp>

#include <aap_protobuf/service/wifiprojection/WifiProjectionMessageId.pb.h>
#include <aap_protobuf/service/wifiprojection/message/WifiCredentialsRequest.pb.h>
#include <aap_protobuf/service/wifiprojection/message/WifiCredentialsResponse.pb.h>

namespace aa::protocol {
namespace source = aap_protobuf::service::wifiprojection;
namespace wifi = source::message;

static_assert(source::WIFI_MESSAGE_CREDENTIALS_REQUEST == 32769);
static_assert(source::WIFI_MESSAGE_CREDENTIALS_RESPONSE == 32770);
static_assert(static_cast<int>(WifiSecurityMode::unknown) == wifi::UNKNOWN_SECURITY_MODE);
static_assert(static_cast<int>(WifiSecurityMode::open) == wifi::OPEN);
static_assert(static_cast<int>(WifiSecurityMode::wep_64) == wifi::WEP_64);
static_assert(static_cast<int>(WifiSecurityMode::wep_128) == wifi::WEP_128);
static_assert(static_cast<int>(WifiSecurityMode::wpa_personal) == wifi::WPA_PERSONAL);
static_assert(static_cast<int>(WifiSecurityMode::wpa2_personal) == wifi::WPA2_PERSONAL);
static_assert(static_cast<int>(WifiSecurityMode::wpa_wpa2_personal) == wifi::WPA_WPA2_PERSONAL);
static_assert(static_cast<int>(WifiSecurityMode::wpa_enterprise) == wifi::WPA_ENTERPRISE);
static_assert(static_cast<int>(WifiSecurityMode::wpa2_enterprise) == wifi::WPA2_ENTERPRISE);
static_assert(static_cast<int>(WifiSecurityMode::wpa_wpa2_enterprise) == wifi::WPA_WPA2_ENTERPRISE);
static_assert(static_cast<int>(WifiAccessPointType::static_ap) == wifi::STATIC);
static_assert(static_cast<int>(WifiAccessPointType::dynamic) == wifi::DYNAMIC);

core::Result<WifiCredentialsRequest> decode_wifi_credentials_request(const MessageView& message) {
    if (message.header.channel != ChannelRole::wifi_projection) {
        return Error{ErrorCode::protocol_unsupported_channel};
    }
    const auto payload = message.payload;
    if (payload.size() < 2 || payload[0] != std::byte{0x80} || payload[1] != std::byte{0x01}) {
        return aasdk_adapter::malformed();
    }
    wifi::WifiCredentialsRequest wire;
    if (const auto parsed = aasdk_adapter::parse_body(wire, payload.subspan(2)); !parsed) {
        return parsed.error();
    }
    if (aasdk_adapter::classify_unknown_fields(wire, {})
            .rejected_under(aasdk_adapter::kStrictExtensionPolicy)) {
        return aasdk_adapter::malformed();
    }
    return WifiCredentialsRequest{};
}

core::Result<std::vector<std::byte>> encode(const WifiCredentialsResponse& message) {
    wifi::WifiCredentialsResponse wire;
    if (message.security_mode) {
        const auto value = static_cast<int>(*message.security_mode);
        if (!wifi::WifiSecurityMode_IsValid(value)) { return aasdk_adapter::malformed(); }
        wire.set_car_wifi_security_mode(static_cast<wifi::WifiSecurityMode>(value));
    }
    for (const auto channel : message.supported_wifi_channels) {
        wire.add_supported_wifi_channels(channel);
    }
    if (message.access_point_type) {
        const auto value = static_cast<int>(*message.access_point_type);
        if (!wifi::AccessPointType_IsValid(value)) { return aasdk_adapter::malformed(); }
        wire.set_access_point_type(static_cast<wifi::AccessPointType>(value));
    }
    if (wire.ByteSizeLong() > aasdk_adapter::kMaxProtoBodyBytes) {
        return aasdk_adapter::malformed();
    }
    return aasdk_adapter::framed_id(source::WIFI_MESSAGE_CREDENTIALS_RESPONSE, wire);
}
} // namespace aa::protocol
