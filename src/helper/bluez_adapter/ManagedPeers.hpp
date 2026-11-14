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
    // Removes an entry; for a list that is not revocation authority. True when the entry is no
    // longer in the list (absent, or the rewritten file was renamed into place). A transient
    // failure leaves the entry for the next attempt; the directory sync is retried by the next
    // remember(), and an entry that reappears after a crash is merely retried.
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
