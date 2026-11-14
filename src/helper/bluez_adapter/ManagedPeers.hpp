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
// The same file-safety rules and format back two more, independent lists under other file
// names: the enrolment windows' bonds awaiting removal (pending-removals, see forget()) and the
// revoked bonds kept only as revocation authority (revoked-bonds, see remember(evict_oldest)).
class ManagedPeers {
public:
    explicit ManagedPeers(std::string directory, std::uint32_t owner = 0, std::string file = "managed-bonds");
    bool valid() const { return valid_; }
    const std::string& reason() const { return reason_; }
    bool contains(const PeerIdentity&) const;   // present in the list, durable or not
    bool revocable(const PeerIdentity& peer) const { return contains(peer); }
    bool admitted(const PeerIdentity&) const;
    bool confirm_durable();
    // evict_oldest: a full list drops its oldest entry instead of refusing (history lists only).
    bool remember(const PeerIdentity&, bool evict_oldest = false);
    // Removes an entry; for a list that is not revocation authority. True only when the entry
    // is durably gone: absent and the directory sync confirmed (a pending unsynced rename is
    // retried here), or rewritten, renamed into place and the directory synced. A transient
    // failure, or an unconfirmed directory sync, returns false: the entry is already out of
    // the in-memory list after a rename, and the next forget()/remember() retries the sync.
    bool forget(const PeerIdentity&);
    const std::vector<PeerIdentity>& peers() const { return peers_; }
private:
    bool replace(int directory, const std::vector<PeerIdentity>&);
    bool sync_directory(int dir_fd);
    std::string directory_;
    std::string file_;
    std::uint32_t owner_;
    bool valid_{};
    bool durable_{true};
    std::size_t admitted_{};
    std::string reason_;
    std::vector<PeerIdentity> peers_;
};
}
