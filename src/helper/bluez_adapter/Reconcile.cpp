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
void BluezBackend::Impl::mark_disk_owed(const PeerIdentity& peer) {
    if (!listed(disk_owed, peer)) disk_owed.push_back(peer);
}
void BluezBackend::Impl::capture_pending_digest(const PeerIdentity& peer) {
    if (!pending.contains(peer)) return;
    if (pending.from_disk(peer) || listed(disk_owed, peer) || listed(removal_attempted, peer)) return;
    if (!pending.digest(peer).empty()) return;
    if (const auto digest = bond_digest.compute(peer)) (void)pending.remember(peer, false, false, *digest);
}
BluezBackend::Impl::BondVerification BluezBackend::Impl::verify_bonded_removal(const PeerIdentity& peer) {
    std::string stored = pending.digest(peer);
    if (stored.empty()) stored = owed_removals.digest(peer);
    const bool must_verify = pending.from_disk(peer) || owed_removals.from_disk(peer) ||
                             listed(disk_owed, peer) || listed(removal_attempted, peer);
    if (stored.empty() && !must_verify) {
        capture_pending_digest(peer);
        stored = pending.digest(peer);
        if (stored.empty() && owed_removals.contains(peer) && !owed_removals.from_disk(peer)) {
            if (const auto digest = bond_digest.compute(peer)) {
                (void)owed_removals.remember(peer, false, false, *digest);
                stored = owed_removals.digest(peer);
            }
        }
    }
    if (!stored.empty()) {
        const auto current = bond_digest.compute(peer);
        if (!current) return BondVerification::ambiguous;
        return *current == stored ? BondVerification::verified_original : BondVerification::outside_rebond;
    }
    return must_verify ? BondVerification::ambiguous : BondVerification::verified_original;
}
bool BluezBackend::Impl::resolve_unremovable_bond(const PeerIdentity& peer, BondVerification verdict,
                                                   const std::vector<Device>& devices) {
    const bool retired = (verdict == BondVerification::outside_rebond) || revoked.retired(peer);
    if (!revoked.remember(peer, true, retired)) return false;
    if (managed.contains(peer) && !managed.forget(peer)) return false;
    if (!pending.forget(peer)) return false;
    if (!owed_removals.forget(peer)) return false;
    std::erase(disk_owed, peer);
    std::erase(removal_attempted, peer);
    std::erase(removal_done, peer);
    std::erase(awaiting, peer);
    std::erase(refused, peer);
    std::erase_if(removal_retries, [&](const RemovalRetry& entry) { return entry.peer == peer; });
    warn_revoked_bonded_peers(devices);
    return true;
}
void BluezBackend::Impl::warn_revoked_bonded_peers(const std::vector<Device>& devices) {
    const auto is_bonded = [&](const PeerIdentity& peer) {
        return std::any_of(devices.begin(), devices.end(),
            [&](const Device& device) { return device.identity == peer && device.bonded; });
    };
    std::erase_if(refused_bond_warned, [&](const PeerIdentity& peer) { return !is_bonded(peer); });
    std::optional<PeerIdentity> live;
    if (allowed && pending.contains(allowed->device.identity)) live = allowed->device.identity;
    for (const auto& device : devices) {
        const auto& peer = device.identity;
        const bool parked = listed(approved, peer) &&
                            (pending.contains(peer) || owed_removals.contains(peer));
        if (!device.bonded || !revoked.contains(peer) || live == peer || listed(awaiting, peer) ||
            listed(refused, peer) || ((pending.contains(peer) || owed_removals.contains(peer)) && !parked))
            continue;
        if (!listed(refused_bond_warned, peer)) {
            std::cerr << "system-helper: warning bond_refused_revoked_identity "
                      << diagnostics::redact_identifier(peer.address)
                      << (parked
                              ? ": this phone was forgotten in Android Auto but a stale approval "
                                "has parked its owed removal; keeping the shared adapter bond "
                                "and refusing Android Auto use. Forget or enrol the phone again in Android Auto.\n"
                              : revoked.retired(peer)
                              ? ": this phone was forgotten in Android Auto and is bonded again "
                                "(paired outside Android Auto); keeping the shared adapter bond "
                                "and refusing Android Auto use. Enrol the phone again in Android Auto to use it.\n"
                              : ": this phone was forgotten in Android Auto and its bond history "
                                "is ambiguous; keeping the shared adapter bond and refusing Android "
                                "Auto use. Enrol the phone again in Android Auto to use it.\n");
            refused_bond_warned.push_back(peer);
        }
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
    const bool bonded = std::any_of(devices.begin(), devices.end(),
                                    [&](const Device& device) { return device.identity == peer && device.bonded; });
    if (bonded) {
        const auto verdict = verify_bonded_removal(peer);
        if (verdict != BondVerification::verified_original) {
            return resolve_unremovable_bond(peer, verdict, devices) ? Removal::kept_refused : back_off();
        }
    }
    // RemoveDevice can succeed for an unbonded Device1 too. A later visible bond
    // must never be adopted as the original after an uncertain reply.
    if (!listed(removal_attempted, peer)) removal_attempted.push_back(peer);
    ++budget.attempts;
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
// historical managed or revoked entry never suppresses removal.
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
        if (revoked.retired(peer)) {
            (void)pending.forget(peer);
            if (!pending.contains(peer)) {
                std::erase(disk_owed, peer);
                std::erase(removal_attempted, peer);
            }
            continue;
        }
        const bool bonded = std::any_of(devices.begin(), devices.end(),
            [&](const Device& device) { return device.identity == peer && device.bonded; });
        if (bonded && listed(approved, peer)) {
            // Approved, so the bond stays (#74). If revoked already owes removal of an AA bond,
            // park the digest-tracked obligation in owed_removals before dropping pending-removals
            // so a repeated ForgetPhone or later unapproved reconciliation still removes the bond
            // while not blocking a fresh enrolment.
            if (revoked.contains(peer)) {
                std::string digest = pending.digest(peer);
                if (digest.empty()) digest = owed_removals.digest(peer);
                if (digest.empty() && !pending.from_disk(peer) && !owed_removals.from_disk(peer) &&
                    !listed(disk_owed, peer) && !listed(removal_attempted, peer)) {
                    if (const auto computed = bond_digest.compute(peer)) digest = *computed;
                }
                if (pending.from_disk(peer)) mark_disk_owed(peer);
                if (owed_removals.remember(peer, false, false, digest)) (void)pending.forget(peer);
            }
            continue;
        }
        if (std::find(removal_done.begin(), removal_done.end(), peer) == removal_done.end()) {
            const auto removal = remove_bond(devices, peer, budget, own ? Retire::own : mode);
            if (budget.exhausted) { cursor = (first + step) % peers.size(); break; }
            if (removal == Removal::kept_refused) { cursor = (first + step + 1) % peers.size(); continue; }
            if (removal == Removal::failed) cursor = (first + step + 1) % peers.size();
            if (removal != Removal::gone) continue;
            cursor = (first + step + 1) % peers.size();
            // A record that cannot be rewritten must not make a later device with the same
            // address (legitimately paired again) be removed on every tick.
            removal_done.push_back(peer);
        }
        // A managed entry left by an interrupted commit is demoted before the obligation is
        // dropped, so a removed, never-admitted bond can never count as admitted later.
        if (!demote(peer, false)) continue;
        // Mark revocation history retired BEFORE dropping the journal entry, so a crash between
        // the two writes never loses the proof that removal already completed.
        if (revoked.contains(peer) && !revoked.remember(peer, true, true)) continue;
        (void)pending.forget(peer);
        if (!pending.contains(peer)) {                       // gone from memory; sync retried
            std::erase(removal_done, peer);
            std::erase(disk_owed, peer);
            std::erase(removal_attempted, peer);
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
    if (!reconciled || !pending.valid() || !owed_removals.valid() || pending.contains(peer) ||
        listed(awaiting, peer) || listed(refused, peer) || revoked.contains(peer))
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
    // The removal obligation (with keyed bond digest) first, then history (revocation authority
    // never lapses), then the managed entry; each confirmed durable (rename + directory fsync)
    // before the next. Managed state is never dropped before the obligation to remove its bond is
    // durable, so a crash or a failed write between any two steps leaves a state reconciliation
    // completes, never an admitted or a stranded bond. The obligation is dropped only once BlueZ
    // confirmed the removal (or verified as outside re-bond / ambiguous).
    if (owe_removal) {
        std::string digest = pending.digest(peer);
        if (digest.empty()) digest = owed_removals.digest(peer);
        if (digest.empty() && !pending.from_disk(peer) && !owed_removals.from_disk(peer) &&
            !listed(disk_owed, peer) && !listed(removal_attempted, peer)) {
            if (const auto computed = bond_digest.compute(peer)) digest = *computed;
        }
        if (!pending.remember(peer, false, false, digest)) {
            // Keep managed authority until the ordinary demotion can complete.
            // Best-effort RemoveDevice can still have an uncertain outcome while
            // pending-removals is unwritable: retain its original evidence in the
            // independent journal so retries cannot adopt a replacement bond.
            (void)owed_removals.remember(peer, false, false, digest);
            return false;
        }
    }
    if (!revoked.remember(peer, true, false)) return false;
    if (!managed.forget(peer)) return false;
    std::erase(refused, peer);
    return true;
}
bool BluezBackend::Impl::clear_revocation(const PeerIdentity& peer) {
    if (revoked.contains(peer) && !revoked.forget(peer)) return false;
    if (owed_removals.contains(peer) && !owed_removals.forget(peer)) return false;
    std::erase(refused_bond_warned, peer);
    std::erase(disk_owed, peer);
    std::erase(removal_attempted, peer);
    std::erase(removal_done, peer);
    return true;
}
// ReconcileBondsV1. The receiver's approved set decides:
//   approved, revoked, its fresh enrolment -> admitted: this helper run acknowledged a fresh
//     acknowledged and awaiting the commit    enrolment of the identity and its approval landed
//                                             (history cleared first, then the commit's writes)
//   approved, revoked otherwise            -> stays revoked, not admitted, bond kept (#74),
//                                             counted as unadmitted; if still bonded, an owed
//                                             removal is parked in `owed-removals` (digest kept)
//                                             and the journal entry dropped so re-enrolment works;
//                                             kept revoked bonded peers also log once
//   approved, managed                      -> admitted (a journal entry left by a commit that
//                                             stopped between its two writes is dropped)
//   approved, journalled, bonded           -> admitted (the approval landed, the commit did not)
//   approved, nothing to admit             -> counted as unadmitted, nothing removed
//   managed, not approved                  -> demoted (removal owed first), device removed
//   owed-removals, not approved            -> moved back to `pending-removals` (if bonded & not
//                                             retired) for digest-verified removal
//   journalled, not approved, not retired  -> digest-verified device removal (D_cur == D_stored),
//                                             or outside rebond (R) / ambiguous (P) kept & warned
//   journalled, not approved, retired      -> stale post-removal journal entry dropped; bond kept
//   revoked, bonded, not journalled        -> outside re-bond or ambiguous history: bond kept
//                                             on shared adapter, refused for AA, warning logged
// A bond of an identity none of the helper's files name is never touched, and the open enrolment
// window's own bond is left to that window. Success only once every state change is durable and
// every bond Android Auto still owes removal for is gone from BlueZ or its removal durably owed
// in the journal / owed-removals; on failure the stricter state stays and the receiver retries.
core::Result<std::uint64_t> BluezBackend::reconcile_bonds(const std::vector<PeerIdentity>& approved) {
    auto& s = *impl_;
    if (!s.managed.valid() || !s.pending.valid() || !s.owed_removals.valid() ||
        !s.revoked.valid() || !s.bond_digest.valid())
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
            durable = s.demote(peer, false) && durable;
            if (!listed(s.refused, peer) && (s.pending.contains(peer) || s.owed_removals.contains(peer))) {
                if (s.revoked.retired(peer)) {
                    durable = s.pending.forget(peer) && durable;
                    durable = s.owed_removals.forget(peer) && durable;
                    if (!s.pending.contains(peer) && !s.owed_removals.contains(peer)) {
                        std::erase(s.disk_owed, peer);
                        std::erase(s.removal_attempted, peer);
                    }
                } else if (!bonded(peer)) {
                    durable = s.revoked.remember(peer, true, true) && durable;
                    if (s.revoked.retired(peer)) {
                        durable = s.pending.forget(peer) && durable;
                        durable = s.owed_removals.forget(peer) && durable;
                        if (!s.pending.contains(peer) && !s.owed_removals.contains(peer)) {
                            std::erase(s.disk_owed, peer);
                            std::erase(s.removal_attempted, peer);
                        }
                    }
                } else if (s.pending.contains(peer)) {
                    std::string digest = s.pending.digest(peer);
                    if (digest.empty()) digest = s.owed_removals.digest(peer);
                    if (digest.empty() && !s.pending.from_disk(peer) && !s.owed_removals.from_disk(peer) &&
                        !listed(s.disk_owed, peer) && !listed(s.removal_attempted, peer)) {
                        if (const auto computed = s.bond_digest.compute(peer)) digest = *computed;
                    }
                    if (s.pending.from_disk(peer)) s.mark_disk_owed(peer);
                    if (s.owed_removals.remember(peer, false, false, digest)) {
                        durable = s.pending.forget(peer) && durable;
                    } else {
                        durable = false;
                    }
                }
            }
            std::erase(s.awaiting, peer);
            ++unadmitted;
            continue;
        }
        if (s.managed.contains(peer)) {
            durable = s.managed.confirm_durable() && durable;
            if (s.pending.contains(peer)) durable = s.pending.forget(peer) && durable;
            if (s.owed_removals.contains(peer)) durable = s.owed_removals.forget(peer) && durable;
            if (!s.pending.contains(peer) && !s.owed_removals.contains(peer)) {
                std::erase(s.disk_owed, peer);
                std::erase(s.removal_attempted, peer);
            }
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
    for (const auto& peer : std::vector<PeerIdentity>{s.owed_removals.peers()}) {
        if (listed(approved, peer) || live == peer) continue;
        std::erase(s.awaiting, peer);
        if (s.revoked.retired(peer)) {
            durable = s.owed_removals.forget(peer) && durable;
            if (!s.owed_removals.contains(peer) && !s.pending.contains(peer)) {
                std::erase(s.disk_owed, peer);
                std::erase(s.removal_attempted, peer);
            }
            continue;
        }
        if (!bonded(peer)) {
            if (s.revoked.contains(peer)) durable = s.revoked.remember(peer, true, true) && durable;
            if (!s.revoked.contains(peer) || s.revoked.retired(peer)) {
                durable = s.owed_removals.forget(peer) && durable;
                if (!s.owed_removals.contains(peer) && !s.pending.contains(peer)) {
                    std::erase(s.disk_owed, peer);
                    std::erase(s.removal_attempted, peer);
                }
            }
            continue;
        }
        if (s.owed_removals.from_disk(peer)) s.mark_disk_owed(peer);
        if (!s.pending.contains(peer) ||
            (s.pending.digest(peer).empty() && !s.owed_removals.digest(peer).empty())) {
            if (!s.pending.remember(peer, false, false, s.owed_removals.digest(peer))) {
                if (!listed(unwanted, peer)) unwanted.push_back(peer);
                durable = false;
                continue;
            }
        }
        durable = s.owed_removals.forget(peer) && durable;
    }
    for (const auto& peer : std::vector<PeerIdentity>{s.pending.peers()}) {
        if (listed(approved, peer) || live == peer) continue;
        std::erase(s.awaiting, peer);
        if (s.revoked.retired(peer)) {
            // Crash after marking revoked-bonds retired (post-RemoveDevice) and before forgetting
            // pending-removals: removal already completed, so drop the stale journal entry without
            // removing any bond created outside Android Auto since.
            durable = s.pending.forget(peer) && durable;
            if (!s.pending.contains(peer)) {
                std::erase(s.disk_owed, peer);
                std::erase(s.removal_attempted, peer);
            }
            continue;
        }
        if (!listed(unwanted, peer)) unwanted.push_back(peer);
    }
    // Emit non-removal warnings (even when a stale approval keeps the bond under #74) and suppress
    // repeats within the same bonded span. Outside re-bonds/ambiguous entries resolved below also
    // emit via resolve_unremovable_bond.
    s.warn_revoked_bonded_peers(devices);
    s.next_warning_check = s.now() + std::chrono::seconds(1);
    s.reconciled = durable;
    // One removal scan, within one invocation's budget, and never a retry of an earlier failure
    // (those are the tick's, Impl::retire_unaccepted): the reply waits on no other identity's
    // backoff. With every state change durable, each unwanted bond's removal is durably owed in
    // `pending`, so what this scan does not remove is the helper's to retry and no receiver record
    // pins capacity (finding 3).
    if (durable) {
        s.retire_unaccepted(Impl::Retire::first, nullptr, &devices);
    } else {
        // A failed write keeps the stricter state: an unwanted bond is still disconnected and
        // removed (best effort, same budget, digest-verified) whether or not its obligation was
        // recorded, and the reconciliation fails so the receiver retries.
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
