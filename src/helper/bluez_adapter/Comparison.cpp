// SPDX-License-Identifier: GPL-3.0-or-later
#include "Platform.hpp"
#include <algorithm>

namespace aa::helper {
// Retry failed RemoveDevice calls on the helper owner thread. The list is persisted, so this also
// completes removals left pending by a restarted helper. No trust-store identity is needed.
// The journal alone decides: an entry leaves it only at a successful commit, so every entry is a
// bond that was never admitted in its enrolment (unaccepted, provisional or rolled back), and
// a historical managed or revoked entry for the same identity never suppresses its removal.
void BluezBackend::Impl::retire_unaccepted() {
    if (pending.peers().empty() || !current_owner()) return;
    std::vector<Device> devices;
    if (!inventory(devices)) return;
    const auto peers = pending.peers();  // forget() rewrites the list
    for (const auto& peer : peers) {
        if (allowed && allowed->device.identity == peer) continue;
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
        // A managed entry left by a failed commit is demoted before the obligation is dropped,
        // so a removed, never-admitted bond can never count as admitted later.
        if (!demote(peer)) continue;
        (void)pending.forget(peer);
        if (!pending.contains(peer)) {                       // gone from memory; sync retried
            std::erase(removal_done, peer);
            std::erase(cleaned, peer);
            if (cleaned.size() >= 256) cleaned.erase(cleaned.begin());
            cleaned.push_back(peer);
        }
    }
}
bool BluezBackend::Impl::admitted(const PeerIdentity& peer) {
    if (!pending.valid() || pending.contains(peer)) return false;
    return managed.admitted(peer) || (managed.confirm_durable() && managed.admitted(peer));
}
bool BluezBackend::Impl::demote(const PeerIdentity& peer) {
    if (!managed.contains(peer)) return true;
    // History first: revocation authority must never lapse between the two lists. Entries
    // renamed into place count even while their directory sync is unconfirmed (#117).
    (void)revoked.remember(peer, true);
    if (!revoked.contains(peer)) return false;
    (void)managed.forget(peer);
    return !managed.contains(peer);
}
core::Result<bool> BluezBackend::bond_confirmed() {
    impl_->pump();
    if (!impl_->allowed) return Error{ErrorCode::helper_rejected};
    const auto captured = *impl_->allowed;
    if (!impl_->exact(captured, false) || !impl_->allowed) return Error{ErrorCode::helper_rejected};
    if (!impl_->comparison_accepted || !impl_->exact(captured, true)) return false;
    if (!impl_->allowed || impl_->now() >= impl_->window) return Error{ErrorCode::helper_rejected};
    return true;
}
// Provisional: the receiver's checks passed, its trust write has not happened. Nothing durable
// changes; the journal entry written when the window opened stays the cleanup obligation, so a
// receiver that exits now (or a helper restart) still removes the bond. Not admitted.
core::Result<void> BluezBackend::acknowledge_bond() {
    const auto bonded = bond_confirmed();
    if (!bonded) return bonded.error();
    if (!bonded.value() || !impl_->allowed) return Error{ErrorCode::helper_rejected};
    if (!impl_->pending.contains(impl_->allowed->device.identity)) return Error{ErrorCode::helper_rejected};
    impl_->acknowledged = true;
    return {};
}
// The only place a bond becomes admitted: after the receiver's durable trust write. The managed
// entry must be durably synced (#117) before the journal entry is durably removed. Any failure
// leaves the bond unadmitted with its journal entry, closes the window (no services) and lets
// cleanup remove it; the receiver retracts its approval.
core::Result<void> BluezBackend::commit_bond() {
    if (!impl_->acknowledged || !impl_->allowed) return Error{ErrorCode::helper_rejected};
    const auto identity = impl_->allowed->device.identity;
    const auto bonded = bond_confirmed();
    const auto fail = [&] {
        (void)impl_->demote(identity);
        if (!impl_->pending.contains(identity)) (void)impl_->pending.remember(identity);
        impl_->close_window();
        return core::Result<void>{Error{ErrorCode::helper_rejected}};
    };
    if (!bonded || !bonded.value() || !impl_->allowed || !impl_->acknowledged ||
        impl_->allowed->device.identity != identity) return fail();
    if (!impl_->managed.remember(identity) || !impl_->pending.forget(identity)) return fail();
    impl_->acknowledged = false;
    return {};
}
void BluezBackend::Impl::reject_comparison() {
    if (!comparison) return;
    auto* invocation = comparison->invocation;
    comparison.reset();
    g_dbus_method_invocation_return_dbus_error(invocation, "org.bluez.Error.Rejected", "Not authorized");
    g_object_unref(invocation);
}
core::Result<PairingComparison> BluezBackend::comparison() {
    impl_->pump();
    if (!impl_->comparison) return Error{ErrorCode::helper_rejected};
    const auto captured = *impl_->comparison;
    if (!impl_->exact(captured.observed, false) || !impl_->comparison ||
        impl_->comparison->value.id != captured.value.id || impl_->now() >= captured.deadline ||
        !impl_->permitted(captured.observed.device.path.c_str(), true)) {
        impl_->close_admission(); return Error{ErrorCode::helper_rejected};
    }
    return captured.value;
}
core::Result<void> BluezBackend::respond_comparison(ComparisonResponse response) {
    const auto current = comparison();
    if (!current || current.value().id != response.id) return Error{ErrorCode::helper_rejected};
    if (!response.accept) { impl_->close_admission(); return {}; }
    // exact() may dispatch cancellation/owner-loss events; never retain its invocation across it.
    const auto captured = *impl_->comparison;
    if (!impl_->exact(captured.observed, false) || !impl_->comparison ||
        impl_->comparison->value.id != response.id || !impl_->pairing_enabled ||
        !impl_->allowed || impl_->allowed->device.identity != captured.value.peer ||
        impl_->now() >= captured.deadline || impl_->now() >= impl_->window) {
        impl_->close_admission(); return Error{ErrorCode::helper_rejected};
    }
    // The acceptance is consumed here, exactly once, by the reply to the accepted
    // RequestConfirmation (comparison_used already forbids a second challenge, and
    // no other pairing method is ever accepted). Pairing closes right after the
    // reply: a genuine pairing that then fails cannot be followed by another
    // device, e.g. one spoofing the phone's address, pairing into this window.
    impl_->comparison_accepted = true;
    auto* invocation = impl_->comparison->invocation;
    impl_->comparison.reset();
    g_dbus_method_invocation_return_value(invocation, nullptr);
    g_object_unref(invocation);
    impl_->quiet_pending = !impl_->quiet();
    return {};
}
}
