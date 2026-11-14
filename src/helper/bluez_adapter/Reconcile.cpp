// SPDX-License-Identifier: GPL-3.0-or-later
// Bond state as a reconciled cache of the receiver's trust store (docs/enrolment-ordering.md).
#include "Platform.hpp"
#include <algorithm>

namespace aa::helper {
namespace {
bool listed(const std::vector<PeerIdentity>& list, const PeerIdentity& peer) {
    return std::find(list.begin(), list.end(), peer) != list.end();
}
bool address_valid(const std::string& address) {
    if (address.size() != 17) return false;
    for (std::size_t i = 0; i < address.size(); ++i) {
        const char c = address[i];
        if (i % 3 == 2 ? c != ':' : !((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F'))) return false;
    }
    return true;
}
}
// Retries RemoveDevice for every journal entry on the helper owner thread. The list is persisted,
// so this also completes removals left pending by a restarted helper. Nothing is removed before
// this helper run's first reconciliation (any entry may be an approval whose commit never landed),
// an acknowledged bond awaiting the receiver's answer is kept, and a bonded device the receiver
// approves is never removed (#74). A historical managed or revoked entry never suppresses removal.
void BluezBackend::Impl::retire_unaccepted() {
    if (!reconciled || pending.peers().empty() || !current_owner()) return;
    std::vector<Device> devices;
    if (!inventory(devices)) return;
    const auto peers = pending.peers();  // forget() rewrites the list
    for (const auto& peer : peers) {
        if (allowed && allowed->device.identity == peer) continue;
        if (listed(awaiting, peer)) continue;
        const bool bonded = std::any_of(devices.begin(), devices.end(),
            [&](const Device& device) { return device.identity == peer && device.bonded; });
        if (bonded && listed(approved, peer)) {
            // Approved, so the bond stays (#74). A revoked identity's approval is never admitted
            // without a fresh committed enrolment, so its window's journal entry is not needed (the
            // history keeps the removal authority) and must not block the identity's next enrolment.
            if (revoked.contains(peer)) (void)pending.forget(peer);
            continue;
        }
        const auto done = std::find(removal_done.begin(), removal_done.end(), peer);
        if (done == removal_done.end()) {
            bool removed = true;
            for (const auto& device : devices) {
                if (device.identity != peer) continue;
                disconnect(device);
                auto* reply = call(device.adapter_path.c_str(), "org.bluez.Adapter1", "RemoveDevice",
                                   g_variant_new("(o)", device.path.c_str()));
                if (!reply) { removed = false; break; }
                g_variant_unref(reply);
            }
            if (!removed) continue;
            // A record that cannot be rewritten must not make a later device with the same
            // address (legitimately paired again) be removed on every tick.
            removal_done.push_back(peer);
        }
        // A managed entry left by an interrupted commit is demoted before the obligation is
        // dropped, so a removed, never-admitted bond can never count as admitted later.
        if (!demote(peer, false)) continue;
        (void)pending.forget(peer);
        if (!pending.contains(peer)) {                       // gone from memory; sync retried
            std::erase(removal_done, peer);
            std::erase(cleaned, peer);
            if (cleaned.size() >= 256) cleaned.erase(cleaned.begin());
            cleaned.push_back(peer);
        }
    }
}
void BluezBackend::Impl::park() {
    if (!acknowledged || !allowed) return;
    const auto& identity = allowed->device.identity;
    if (pending.contains(identity) && !listed(awaiting, identity)) awaiting.push_back(identity);
}
bool BluezBackend::Impl::admitted(const PeerIdentity& peer) {
    if (!reconciled || !pending.valid() || pending.contains(peer) || listed(awaiting, peer) || listed(refused, peer) ||
        revoked.contains(peer))
        return false;
    return managed.admitted(peer) || (managed.confirm_durable() && managed.admitted(peer));
}
bool BluezBackend::Impl::demote(const PeerIdentity& peer, bool owe_removal) {
    if (!managed.contains(peer)) {
        // Absent in memory; that absence is durable only once the rename that removed it is.
        if (!managed.forget(peer)) return false;
        std::erase(refused, peer);
        return true;
    }
    // Refused from here on, whatever the writes below report: a failure leaves the stricter state.
    if (!listed(refused, peer)) refused.push_back(peer);
    // The removal obligation first, then history (revocation authority never lapses), then the
    // managed entry; each confirmed durable (rename + directory fsync) before the next. Managed
    // state is never dropped before the obligation to remove its bond is durable, so a crash or a
    // failed write between any two steps leaves a state reconciliation completes, never an admitted
    // or a stranded bond. The obligation is dropped only once BlueZ confirmed the removal.
    if (owe_removal && !pending.remember(peer)) return false;
    if (!revoked.remember(peer, true)) return false;
    if (!managed.forget(peer)) return false;
    std::erase(refused, peer);
    return true;
}
bool BluezBackend::Impl::clear_revocation(const PeerIdentity& peer) {
    return !revoked.contains(peer) || revoked.forget(peer);
}
// ReconcileBondsV1. The receiver's approved set decides:
//   approved, revoked, its fresh enrolment -> admitted: this helper run acknowledged a fresh
//     acknowledged and awaiting the commit    enrolment of the identity and its approval landed
//                                             (history cleared first, then the commit's writes)
//   approved, revoked otherwise            -> stays revoked, not admitted, bond kept (#74),
//                                             counted as unadmitted; a stale journal entry is
//                                             dropped (the history keeps the removal authority)
//   approved, managed                      -> admitted (a journal entry left by a commit that
//                                             stopped between its two writes is dropped)
//   approved, journalled, bonded           -> admitted (the approval landed, the commit did not)
//   approved, nothing to admit             -> counted as unadmitted, nothing removed
//   managed, not approved                  -> demoted (removal owed first), device removed
//   journalled, not approved               -> device removed, entry dropped
//   revoked, bonded, not approved          -> removal owed again and the device removed, whatever
//                                             the journal says (a lost entry strands no bond)
// A bond of an identity none of the helper's files name is never touched, and the open enrolment
// window's own bond is left to that window. Success only once every state change is durable and
// BlueZ holds no bond the helper names that the receiver does not approve; on failure the
// stricter state stays and the receiver retries.
core::Result<std::uint64_t> BluezBackend::reconcile_bonds(const std::vector<PeerIdentity>& approved) {
    auto& s = *impl_;
    if (!s.managed.valid() || !s.pending.valid() || !s.revoked.valid()) return Error{ErrorCode::helper_unavailable};
    if (approved.size() > SystemHelper::kMaxApprovedBonds) return Error{ErrorCode::helper_rejected};
    for (const auto& peer : approved)
        if (!address_valid(peer.address) || !address_valid(peer.adapter)) return Error{ErrorCode::helper_rejected};
    s.drain_events();
    if (!s.current_owner()) return Error{ErrorCode::helper_unavailable};
    std::vector<Impl::Device> devices;
    if (!s.inventory(devices)) return Error{ErrorCode::helper_unavailable};
    const auto bonded = [&](const PeerIdentity& peer) {
        return std::any_of(devices.begin(), devices.end(),
            [&](const Impl::Device& device) { return device.identity == peer && device.bonded; });
    };
    std::optional<PeerIdentity> live;
    if (s.allowed && s.pending.contains(s.allowed->device.identity)) live = s.allowed->device.identity;
    bool durable = true;
    std::uint64_t unadmitted = 0;
    s.approved = approved;
    for (const auto& peer : approved) {
        if (live == peer) continue;
        if (listed(s.refused, peer) || (s.revoked.contains(peer) && !listed(s.awaiting, peer))) {
            // Only a fresh enrolment that reached its commit clears history, so this approval was
            // never committed after the revocation: a resurrected or stale approval, or one whose
            // window closed without acceptance. The revocation wins.
            durable = s.demote(peer, false) && durable;
            if (!listed(s.refused, peer) && s.pending.contains(peer)) durable = s.pending.forget(peer) && durable;
            std::erase(s.awaiting, peer);
            ++unadmitted;
            continue;
        }
        if (s.managed.contains(peer)) {
            durable = s.managed.confirm_durable() && durable;
            if (s.pending.contains(peer)) durable = s.pending.forget(peer) && durable;
            std::erase(s.awaiting, peer);
            continue;
        }
        if (s.pending.contains(peer) && bonded(peer)) {
            // A revoked identity reaches this only with its fresh enrolment acknowledged (`awaiting`).
            durable = s.clear_revocation(peer) && s.managed.remember(peer) && s.pending.forget(peer) && durable;
            std::erase(s.awaiting, peer);
            continue;
        }
        std::erase(s.awaiting, peer);
        ++unadmitted;
    }
    std::vector<PeerIdentity> unwanted;
    for (const auto& peer : std::vector<PeerIdentity>{s.managed.peers()}) {
        if (listed(approved, peer)) continue;
        if ((s.allowed && s.allowed->device.identity == peer) || (s.connected && s.connected->device.identity == peer))
            s.close_window();
        durable = s.demote(peer) && durable;
        unwanted.push_back(peer);
    }
    for (const auto& peer : std::vector<PeerIdentity>{s.pending.peers()}) {
        if (listed(approved, peer) || live == peer) continue;
        std::erase(s.awaiting, peer);
        if (!listed(unwanted, peer)) unwanted.push_back(peer);
    }
    // The removal obligation is derived, not only journalled: a bond whose journal entry was lost
    // (a failed write, a crash, an older helper's demotion order) is still removed. Only identities
    // the helper's own history names count; BlueZ cannot tell the owner's other bonds apart.
    for (const auto& device : devices) {
        const auto& peer = device.identity;
        if (!device.bonded || !s.revoked.contains(peer) || listed(approved, peer) || live == peer ||
            listed(unwanted, peer)) continue;
        durable = s.pending.remember(peer) && durable;
        unwanted.push_back(peer);
    }
    s.reconciled = durable;
    if (durable) s.retire_unaccepted();
    // What cleanup could not remove (or a failed write left unrecorded) is disconnected and removed
    // now. A bond the helper names that the receiver does not approve and BlueZ still holds fails
    // the reconciliation, so the receiver retries.
    bool clean = true;
    if (!unwanted.empty()) {
        devices.clear();
        if (!s.current_owner() || !s.inventory(devices)) {
            clean = false;
        } else {
            for (const auto& device : devices) {
                if (!listed(unwanted, device.identity) || (!device.bonded && !s.pending.contains(device.identity)))
                    continue;
                s.disconnect(device);
                auto* reply = s.call(device.adapter_path.c_str(), "org.bluez.Adapter1", "RemoveDevice",
                                     g_variant_new("(o)", device.path.c_str()));
                if (reply) g_variant_unref(reply);
                else if (device.bonded) clean = false;
            }
            if (durable) s.retire_unaccepted();  // drops the entries of what was removed just now
        }
    }
    if (!durable || !clean) return Error{ErrorCode::helper_unavailable};
    return unadmitted | (live ? std::uint64_t{1} << 32 : 0U);
}
}
