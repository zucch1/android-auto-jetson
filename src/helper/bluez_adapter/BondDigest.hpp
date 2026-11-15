// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <aa/helper/SystemHelper.hpp>
#include <array>
#include <cstdint>
#include <optional>
#include <string>

namespace aa::helper::bluez_adapter {
// Per-helper keyed HMAC-SHA256 digest of BlueZ persistent link/LTKs for a bonded peer.
// Used solely to distinguish whether a BlueZ bond present across restart or lost reply
// is still the original bond Android Auto owed removal for (digest match) or a newer bond
// paired outside Android Auto (digest mismatch). Raw key material is never persisted or logged.
class BondDigest {
public:
    BondDigest(std::string state_directory, std::uint32_t owner = 0,
               std::string bluetooth_directory = {});
    ~BondDigest();
    BondDigest(const BondDigest&) = delete;
    BondDigest& operator=(const BondDigest&) = delete;

    bool valid() const { return valid_; }
    const std::string& reason() const { return reason_; }
    std::optional<std::string> compute(const PeerIdentity& peer);

private:
    bool ensure_key();
    std::string state_directory_;
    std::string bluetooth_directory_;
    std::uint32_t owner_;
    std::array<unsigned char, 32> secret_{};
    bool valid_{};
    bool key_on_disk_{};
    bool key_durable_{};
    std::string reason_;
};
}
