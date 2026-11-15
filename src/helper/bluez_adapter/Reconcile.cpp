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
BluezBackend::Impl::Removal BluezBackend::Impl::remove_bond(const std::vector<Device>& devices,
                                                             const PeerIdentity& peer, RemovalBudget& budget,
                                                             Retire mode) {
    const auto retry = std::find_if(removal_retries.begin(), removal_retries.end(),
                                    [&](const RemovalRetry& item) { return item.peer == peer; });
    const bool present = std::any_of(devices.begin(), devices.end(),
                                     [&](const Device& device) { return device.identity == peer; });
    if (!present) {
        if (retry != removal_retries.end()) removal_retries.erase(retry);
        return Removal::gone;
    }
    if (retry != removal_retries.end() &&
        (mode == Retire::first || (mode == Retire::tick && now() < retry->next)))
        return Removal::deferred;
    if (budget.exhausted || budget.attempts >= kRemovalAttemptBudget ||
        budget.remaining() < kRemovalMinimumAttempt.count()) {
        budget.exhausted = true;
        return Removal::deferred;
    }
    ++budget.attempts;
    const auto back_off = [&] {
        // Backed off from the failed attempt's completion: 1 s, doubling to 30 s.
        auto item = std::find_if(removal_retries.begin(), removal_retries.end(),
                                 [&](const RemovalRetry& entry) { return entry.peer == peer; });
        if (item == removal_retries.end()) {
            if (removal_retries.size() >= 512)
                std::erase_if(removal_retries, [&](const RemovalRetry& entry) { return !pending.contains(entry.peer); });
            item = removal_retries.insert(removal_retries.end(), RemovalRetry{peer});
        }
        if (item->failures < 16) ++item->failures;
        const auto delay = std::chrono::seconds{1U << std::min<std::uint32_t>(item->failures - 1, 5)};
        item->next = now() + std::min<Clock::duration>(delay, std::chrono::seconds{30});
        return Removal::failed;
    };
    for (const auto& device : devices) {
        if (device.identity != peer) continue;
        // Disconnect gets at most half of what is left, so a slow Disconnect cannot starve the
        // RemoveDevice that follows; RemoveDevice gets the rest, never past the deadline. An attempt
        // that runs out of time is a failed one (backed off); the obligation stays durable.
        disconnect(device, budget.remaining() / 2);
        const int left = budget.remaining();
        if (left <= 0) return back_off();
        auto* reply = call(device.adapter_path.c_str(), "org.bluez.Adapter1", "RemoveDevice",
                           g_variant_new("(o)", device.path.c_str()), nullptr, left);
        if (!reply) return back_off();
        g_variant_unref(reply);
    }
    std::erase_if(removal_retries, [&](const RemovalRetry& entry) { return entry.peer == peer; });
    return Removal::gone;
}
// Retries RemoveDevice for the journal's entries on the helper owner thread, within one
// invocation's budget (remove_bond). The list is persisted, so this also completes removals left
// pending by a restarted helper. Nothing is removed before this helper run's first reconciliation
// (any entry may be an approval whose commit never landed), an acknowledged bond awaiting the
// receiver's answer is kept, and a bonded device the receiver approves is never removed (#74). A
// historical managed or revoked entry never suppresses removal. Only an entry the helper owns
// (owns(): its window opened with no pairing of the device) is ever removed from BlueZ; any other
// entry is released, its bond kept (shared adapter, #136).
void BluezBackend::Impl::retire_unaccepted(Retire mode, const PeerIdentity* own, const std::vector<Device>* known) {
    if (!reconciled || pending.peers().empty()) return;
    if (own && !pending.contains(*own)) return;
    // Not even one attempt fits before this invocation's deadline: no BlueZ call at all.
    if (RemovalBudget{*this}.remaining() < kRemovalMinimumAttempt.count()) return;
    if (!known && mode == Retire::tick) {
        // Nothing due (every entry parked, live or backing off): no BlueZ call at all this tick.
        const auto due = std::any_of(pending.peers().begin(), pending.peers().end(), [&](const PeerIdentity& peer) {
            if ((allowed && allowed->device.identity == peer) || listed(awaiting, peer)) return false;
            const auto retry = std::find_if(removal_retries.begin(), removal_retries.end(),
                                            [&](const RemovalRetry& item) { return item.peer == peer; });
            return retry == removal_retries.end() || now() >= retry->next;
        });
        if (!due) return;
    }
    std::vector<Device> fetched;
    if (!known) {
        if (!current_owner() || !inventory(fetched)) return;
        known = &fetched;
    }
    const auto& devices = *known;
    RemovalBudget budget{*this};
    const auto peers = pending.peers();  // forget() rewrites the list
    const std::size_t first = removal_cursor % peers.size();
    std::size_t cursor = first;
    for (std::size_t step = 0; step < peers.size(); ++step) {
        const auto& peer = peers[(first + step) % peers.size()];
        if (own && peer != *own) continue;
        if (allowed && allowed->device.identity == peer) continue;
        if (listed(awaiting, peer)) continue;
        const bool bonded = std::any_of(devices.begin(), devices.end(),
            [&](const Device& device) { return device.identity == peer && device.bonded; });
        if (bonded && listed(approved, peer)) {
            // Approved, so the bond stays (#74). A revoked identity's approval is never admitted
            // without a fresh committed enrolment, so its window's journal entry is not needed (the
            // history keeps the refusal) and must not block the identity's next enrolment.
            if (revoked.contains(peer)) (void)drop_journal(peer);
            continue;
        }
        const bool owned = owns(peer);
        if (!owned) {
            // Not provably created by its window: a bond that existed when the window opened, or
            // an entry an older (exclusive-adapter) helper journalled. Released without any BlueZ
            // call; a revoked identity is refused and reported by reconciliation instead.
            if (bonded && !revoked.contains(peer))
                std::cerr << "system-helper: warning bond_kept_unowned_enrolment "
                          << diagnostics::redact_identifier(peer.address)
                          << ": this phone was already paired in Bluetooth when its Android Auto enrolment "
                             "opened, or was queued by an older helper; the Bluetooth adapter is shared, so its "
                             "pairing is kept and Android Auto does not admit the phone.\n";
        } else if (std::find(removal_done.begin(), removal_done.end(), peer) == removal_done.end()) {
            const auto removal = remove_bond(devices, peer, budget, own ? Retire::own : mode);
            if (budget.exhausted) { cursor = (first + step) % peers.size(); break; }
            if (removal == Removal::failed) cursor = (first + step + 1) % peers.size();
            if (removal != Removal::gone) continue;
            cursor = (first + step + 1) % peers.size();
            // A record that cannot be rewritten must not make a later device with the same
            // address (legitimately paired again) be removed on every tick.
            removal_done.push_back(peer);
        }
        // A managed entry left by an interrupted commit is demoted before the obligation is
        // dropped, so a removed, never-admitted bond can never count as admitted later.
        if (!demote(peer)) continue;
        (void)drop_journal(peer);
        if (owned && !pending.contains(peer)) {              // gone from memory; sync retried
            std::erase(removal_done, peer);
            std::erase(cleaned, peer);
            if (cleaned.size() >= 256) cleaned.erase(cleaned.begin());
            cleaned.push_back(peer);
        }
    }
    if (!own) removal_cursor = cursor;
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
bool BluezBackend::Impl::drop_journal(const PeerIdentity& peer) {
    if (pending.contains(peer) && !pending.forget(peer)) return false;
    // A marker left behind is no authority (owns() needs the journal entry too); reconciliation
    // drops it, and a window that opens for an already paired device drops it first.
    if (created.contains(peer)) (void)created.forget(peer);
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
//   journalled, not approved               -> device removed, entry dropped (enrolment cleanup)
//   revoked, bonded, not approved          -> refused, bond kept, a warning with the redacted
//                                             address (#136)
// The Bluetooth adapter is shared (docs/enrolment-ordering.md, #136): only an enrolment's own
// journal entry (`pending`) ever leads to a removal; a bond outside it is never deleted, because
// BlueZ cannot remove a bond conditionally on it still being the one the helper decided about. A
// bond of an identity none of the helper's files name is never touched, and the open enrolment
// window's own bond is left to that window. Success only once every state change is durable and
// every journalled bond the receiver does not approve is gone from BlueZ or its removal still
// durably owed; on failure the stricter state stays and the receiver retries.
core::Result<std::uint64_t> BluezBackend::reconcile_bonds(const std::vector<PeerIdentity>& approved) {
    auto& s = *impl_;
    if (!s.managed.valid() || !s.pending.valid() || !s.revoked.valid() || !s.created.valid())
        return Error{ErrorCode::helper_unavailable};
    if (approved.size() > SystemHelper::kMaxApprovedBonds) return Error{ErrorCode::helper_rejected};
    for (const auto& peer : approved)
        if (!address_valid(peer.address) || !address_valid(peer.adapter)) return Error{ErrorCode::helper_rejected};
    // Every removal of this reconciliation (window closes included) shares one deadline from here.
    const Impl::CleanupScope scope{s};
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
            durable = s.demote(peer) && durable;
            if (!listed(s.refused, peer) && s.pending.contains(peer)) durable = s.drop_journal(peer) && durable;
            std::erase(s.awaiting, peer);
            ++unadmitted;
            continue;
        }
        if (s.managed.contains(peer)) {
            durable = s.managed.confirm_durable() && durable;
            if (s.pending.contains(peer)) durable = s.drop_journal(peer) && durable;
            std::erase(s.awaiting, peer);
            continue;
        }
        if (s.pending.contains(peer) && bonded(peer)) {
            // A revoked identity reaches this only with its fresh enrolment acknowledged (`awaiting`).
            durable = s.clear_revocation(peer) && s.managed.remember(peer) && s.drop_journal(peer) && durable;
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
        // Refused from now on; its bond stays in BlueZ (a journal entry beside it, left by an
        // interrupted commit, is still the enrolment's cleanup below).
        durable = s.demote(peer) && durable;
    }
    // A marker whose journal entry is gone (a crash between the two writes) is no authority: drop it.
    for (const auto& peer : std::vector<PeerIdentity>{s.created.peers()})
        if (!s.pending.contains(peer)) (void)s.created.forget(peer);
    // Only the helper's own enrolment bonds are unwanted (removed); every other unapproved journal
    // entry is released by the cleanup scan below without any BlueZ call (#136).
    for (const auto& peer : std::vector<PeerIdentity>{s.pending.peers()}) {
        if (listed(approved, peer) || live == peer) continue;
        std::erase(s.awaiting, peer);
        if (s.owns(peer) && !listed(unwanted, peer)) unwanted.push_back(peer);
    }
    // The adapter is shared (docs/enrolment-ordering.md, #136): a forgotten phone that is still,
    // or again, bonded is refused (admitted() never passes a revoked identity), never removed, and
    // a warning names its redacted address, whether or not the store still (stale) approves it. An
    // identity whose fresh enrolment was admitted above has no history left; the live window's
    // identity is that window's to decide.
    for (const auto& device : devices) {
        const auto& peer = device.identity;
        if (!device.bonded || !s.revoked.contains(peer) || live == peer || listed(unwanted, peer)) continue;
        std::cerr << "system-helper: warning bond_kept_revoked_identity "
                  << diagnostics::redact_identifier(peer.address)
                  << ": this phone was forgotten in Android Auto but is still paired in Bluetooth; the "
                     "Bluetooth adapter is shared, so its pairing is kept and Android Auto refuses the "
                     "phone. Enrol the phone again in Android Auto to use it.\n";
    }
    s.reconciled = durable;
    // One removal scan, within one invocation's budget, and never a retry of an earlier failure
    // (those are the tick's, Impl::retire_unaccepted): the reply waits on no other identity's
    // backoff. With every state change durable, each unwanted (journalled) bond's removal is owed in
    // `pending`, so what this scan does not remove is the helper's to retry and no receiver record
    // pins capacity (finding 3).
    if (durable) {
        s.retire_unaccepted(Impl::Retire::first, nullptr, &devices);
    } else {
        // A failed write keeps the stricter state: an unwanted journalled bond is still
        // disconnected and removed (best effort, same budget), and the reconciliation fails so the
        // receiver retries.
        Impl::RemovalBudget budget{s};
        for (const auto& peer : unwanted) {
            (void)s.remove_bond(devices, peer, budget, Impl::Retire::first);
            if (budget.exhausted) break;
        }
        return Error{ErrorCode::helper_unavailable};
    }
    return unadmitted | (live ? std::uint64_t{1} << 32 : 0U);
}
}
