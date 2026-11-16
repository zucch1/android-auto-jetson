// SPDX-License-Identifier: GPL-3.0-or-later
// Bond state as a reconciled cache of the receiver's trust store (docs/enrolment-ordering.md).
#include "Platform.hpp"
#include <aa/diagnostics/Diagnostics.hpp>
#include <algorithm>
#include <iostream>

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
// The Bluetooth adapter is shared (docs/enrolment-ordering.md, #136): the helper never removes,
// cancels or disconnects-to-remove a pairing, because BlueZ cannot remove a bond conditionally on
// it still being the one the helper decided about. An enrolment that ends without a commit only
// loses its journal entry; its bond, if one was made, stays in BlueZ and is never admitted.
void BluezBackend::Impl::release(const PeerIdentity& peer, const std::vector<Device>& devices) {
    if (!pending.contains(peer)) return;  // e.g. a reconnect window of an admitted bond
    (void)pending.forget(peer);  // a failed rewrite is retried by the next reconciliation
    const bool bonded = std::any_of(devices.begin(), devices.end(),
        [&](const Device& device) { return device.identity == peer && device.bonded; });
    // A revoked identity's kept bond is reported by reconciliation (bond_kept_revoked_identity).
    if (bonded && !revoked.contains(peer))
        std::cerr << "system-helper: warning bond_kept_unowned_enrolment "
                  << diagnostics::redact_identifier(peer.address)
                  << ": this phone is paired in Bluetooth but its Android Auto enrolment did not "
                     "complete; the Bluetooth adapter is shared, so its pairing is kept and Android "
                     "Auto refuses the phone. To use it, unpair it in Bluetooth settings first, then "
                     "enrol it again in Android Auto.\n";
}
void BluezBackend::Impl::release_window(const std::vector<Device>& devices) {
    if (allowed && !listed(awaiting, allowed->device.identity)) release(allowed->device.identity, devices);
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
bool BluezBackend::Impl::demote(const PeerIdentity& peer) {
    if (!managed.contains(peer)) {
        // Absent in memory; that absence is durable only once the rename that removed it is.
        if (!managed.forget(peer)) return false;
        std::erase(refused, peer);
        return true;
    }
    // Refused from here on, whatever the writes below report: a failure leaves the stricter state.
    if (!listed(refused, peer)) refused.push_back(peer);
    // History first (revocation authority never lapses), then the managed entry, each confirmed
    // durable (rename + directory fsync) before the next, so a crash or a failed write between the
    // two leaves a state reconciliation completes, never an admitted bond. No removal is owed: on
    // the shared adapter a demoted bond stays in BlueZ, refused (docs/enrolment-ordering.md, #136).
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
//                                             dropped (the history keeps the refusal)
//   approved, managed                      -> admitted (a journal entry left by a commit that
//                                             stopped between its two writes is dropped)
//   approved, journalled, bonded           -> admitted (the approval landed, the commit did not)
//   approved, nothing to admit             -> counted as unadmitted, nothing removed
//   managed, not approved                  -> demoted to revoked: refused, bond kept (#136)
//   journalled otherwise                   -> entry released, bond kept and refused, a warning
//                                             with the redacted address (#136)
//   revoked, bonded                        -> refused, bond kept, a warning with the redacted
//                                             address (#136)
// Nothing is ever removed from BlueZ (shared adapter, #136), and the open enrolment window's own
// identity is left to that window. Success only once every state change is durable; on failure the
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
    for (const auto& peer : approved) {
        if (live == peer) continue;
        if (listed(s.refused, peer) || (s.revoked.contains(peer) && !listed(s.awaiting, peer))) {
            // Only a fresh enrolment that reached its commit clears history, so this approval was
            // never committed after the revocation: a resurrected or stale approval, or one whose
            // window closed without acceptance. The revocation wins.
            durable = s.demote(peer) && durable;
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
    for (const auto& peer : std::vector<PeerIdentity>{s.managed.peers()}) {
        if (listed(approved, peer)) continue;
        if ((s.allowed && s.allowed->device.identity == peer) || (s.connected && s.connected->device.identity == peer))
            s.close_window();
        // Refused from now on; its bond stays in BlueZ (#136).
        durable = s.demote(peer) && durable;
    }
    // Every other journal entry (an unapproved or unbonded enrolment, a restart with a window open,
    // or an entry an older helper wrote) is released, its bond kept and refused. An approved, bonded
    // entry still journalled is a failed write above, kept for the receiver's retry.
    for (const auto& peer : std::vector<PeerIdentity>{s.pending.peers()}) {
        if (live == peer || (listed(approved, peer) && bonded(peer))) continue;
        std::erase(s.awaiting, peer);
        s.release(peer, devices);
    }
    // The adapter is shared (docs/enrolment-ordering.md, #136): a forgotten phone that is still,
    // or again, bonded is refused (admitted() never passes a revoked identity), never removed, and
    // a warning names its redacted address, whether or not the store still (stale) approves it. An
    // identity whose fresh enrolment was admitted above has no history left; the live window's
    // identity is that window's to decide.
    for (const auto& device : devices) {
        const auto& peer = device.identity;
        if (!device.bonded || !s.revoked.contains(peer) || live == peer) continue;
        std::cerr << "system-helper: warning bond_kept_revoked_identity "
                  << diagnostics::redact_identifier(peer.address)
                  << ": this phone was forgotten in Android Auto but is still paired in Bluetooth; the "
                     "Bluetooth adapter is shared, so its pairing is kept and Android Auto refuses the "
                     "phone. To use it, unpair it in Bluetooth settings first, then enrol it again in "
                     "Android Auto.\n";
    }
    s.reconciled = durable;
    if (!durable) return Error{ErrorCode::helper_unavailable};
    return unadmitted | (live ? std::uint64_t{1} << 32 : 0U);
}
}
