// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <aa/helper/SystemHelper.hpp>
#include <cstddef>
#include <string>
#include <vector>
namespace aa::helper::bluez_adapter {
// Root-owned state is the authority, not BlueZ's global bond inventory.
// valid(): the stored state loaded cleanly; it gates revocation and admission
// and never changes afterwards. revocable(): present on disk or renamed into
// place (used by revoke). admitted(): durably committed on disk (used by
// reconnect and admission). confirm_durable(): retries a pending directory
// fsync when durability was not yet confirmed. remember(): new admissions only;
// a failed write refuses that admission but never removes revocation of an
// existing or renamed-into-place entry.
class ManagedPeers {
public:
    explicit ManagedPeers(std::string directory, std::uint32_t owner = 0);
    bool valid() const { return valid_; }
    const std::string& reason() const { return reason_; }
    bool revocable(const PeerIdentity&) const;
    bool admitted(const PeerIdentity&) const;
    bool confirm_durable();
    bool remember(const PeerIdentity&);
private:
    bool sync_directory(int dir_fd);
    std::string directory_;
    std::uint32_t owner_;
    bool valid_{};
    bool durable_{true};
    std::size_t admitted_{};
    std::string reason_;
    std::vector<PeerIdentity> peers_;
};
}
