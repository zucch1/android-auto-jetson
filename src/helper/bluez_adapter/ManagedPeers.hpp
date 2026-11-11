// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <aa/helper/SystemHelper.hpp>
#include <string>
#include <vector>
namespace aa::helper::bluez_adapter {
// Root-owned state is the authority, not BlueZ's global bond inventory.
class ManagedPeers {
public:
    explicit ManagedPeers(std::string directory, std::uint32_t owner = 0);
    bool valid() const { return valid_; }
    bool contains(const PeerIdentity&) const;
    bool remember(const PeerIdentity&);
private:
    std::string directory_;
    std::uint32_t owner_;
    bool valid_{};
    std::vector<PeerIdentity> peers_;
};
}
