// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <aa/helper/SystemHelper.hpp>
#include <string>
#include <vector>
namespace aa::helper::bluez_adapter {
// Root-owned state is the authority, not BlueZ's global bond inventory.
// valid(): the stored state loaded cleanly; it gates revocation authority and
// never changes afterwards. remember(): new admissions only; a failed write
// refuses that admission but never removes revocation of an existing entry.
// The same file-safety rules and format back a second, independent list under another file
// name: the enrolment windows' unaccepted bonds awaiting removal (see forget()).
class ManagedPeers {
public:
    explicit ManagedPeers(std::string directory, std::uint32_t owner = 0, std::string file = "managed-bonds");
    bool valid() const { return valid_; }
    const std::string& reason() const { return reason_; }
    bool contains(const PeerIdentity&) const;
    bool remember(const PeerIdentity&);
    // Removes an entry; for a list that is not revocation authority. True only when the entry
    // is durably gone: absent and the directory sync confirmed (a pending unsynced rename is
    // retried here), or rewritten, renamed into place and the directory synced. A transient
    // failure, or an unconfirmed directory sync, returns false: the entry is already out of
    // the in-memory list after a rename, and the next forget()/remember() retries the sync.
    bool forget(const PeerIdentity&);
    const std::vector<PeerIdentity>& peers() const { return peers_; }
private:
    enum class Write { failed, synced, unsynced };
    Write replace(int directory, const std::vector<PeerIdentity>&);
    std::string directory_;
    std::string file_;
    std::uint32_t owner_;
    bool valid_{};
    bool durable_{true};
    std::string reason_;
    std::vector<PeerIdentity> peers_;
};
}
