// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <aa/protocol/Protocol.hpp>

#include <optional>
#include <string>
#include <vector>

namespace aa::protocol {

struct WifiProjectionConfiguration final {
    std::optional<std::string> car_wifi_bssid{};
    [[nodiscard]] friend bool operator==(const WifiProjectionConfiguration&,
                                         const WifiProjectionConfiguration&) = default;
};

// Negotiated AAP values, NOT the RFCOMM AAW security bitmask.
enum class WifiSecurityMode : std::int32_t {
    unknown = 0, open = 1, wep_64 = 2, wep_128 = 3, wpa_personal = 4,
    wpa2_personal = 5, wpa_wpa2_personal = 6, wpa_enterprise = 7,
    wpa2_enterprise = 8, wpa_wpa2_enterprise = 9,
};
enum class WifiAccessPointType : std::int32_t { static_ap = 0, dynamic = 1 };

struct WifiCredentialsRequest final {};

// Metadata-only subset of the pinned response. Password and SSID credentials
// belong to helper-owned AAW bootstrap and cannot enter this public model.
// Channels are schema int32 values, not an enum; no RF policy is invented here.
struct WifiCredentialsResponse final {
    std::optional<WifiSecurityMode> security_mode{};
    std::vector<std::int32_t> supported_wifi_channels{};
    std::optional<WifiAccessPointType> access_point_type{};
};

// The negotiator resolves dynamic Service.id to MessageView.header.channel;
// this decoder checks that semantic role before interpreting the payload ID.
[[nodiscard]] core::Result<WifiCredentialsRequest>
decode_wifi_credentials_request(const MessageView& message);
[[nodiscard]] core::Result<std::vector<std::byte>> encode(const WifiCredentialsResponse& message);

} // namespace aa::protocol
