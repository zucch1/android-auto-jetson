// SPDX-License-Identifier: GPL-3.0-or-later
#include "Platform.hpp"
#include <algorithm>

namespace aa::helper {
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
// The fast path to admission, after the receiver's durable trust write (reconciliation is the
// other, for a commit that never landed). The managed entry must be durably synced (#117) before
// the journal entry is durably removed. Any failure leaves the bond provisional: unadmitted, its
// journal entry kept, and parked in `awaiting` when the window closes (no services), because the
// receiver's approval may already be durable. The receiver's next reconciliation decides.
core::Result<void> BluezBackend::commit_bond() {
    if (!impl_->acknowledged || !impl_->allowed) return Error{ErrorCode::helper_rejected};
    const auto identity = impl_->allowed->device.identity;
    const auto bonded = bond_confirmed();
    const auto fail = [&] {
        if (std::find(impl_->awaiting.begin(), impl_->awaiting.end(), identity) == impl_->awaiting.end())
            impl_->awaiting.push_back(identity);
        impl_->close_window();
        return core::Result<void>{Error{ErrorCode::helper_rejected}};
    };
    if (!bonded || !bonded.value() || !impl_->allowed || !impl_->acknowledged ||
        impl_->allowed->device.identity != identity) return fail();
    if (!impl_->managed.remember(identity) || !impl_->pending.forget(identity)) return fail();
    impl_->acknowledged = false;
    std::erase(impl_->awaiting, identity);
    if (std::find(impl_->approved.begin(), impl_->approved.end(), identity) == impl_->approved.end())
        impl_->approved.push_back(identity);
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
