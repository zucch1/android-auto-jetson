// SPDX-License-Identifier: GPL-3.0-or-later
#include "Platform.hpp"

namespace aa::helper {
// Retry failed RemoveDevice calls on the helper owner thread. The list is persisted, so this also
// completes removals left pending by a restarted helper. No trust-store identity is needed.
void BluezBackend::Impl::retire_unaccepted() {
    if (pending.peers().empty() || !current_owner()) return;
    std::vector<Device> devices;
    if (!inventory(devices)) return;
    const auto peers = pending.peers();  // forget() rewrites the list
    for (const auto& peer : peers) {
        if (allowed && allowed->device.identity == peer) continue;
        // A bond acknowledged before a restart interrupted the hand-over is managed: never remove it.
        if (!managed.contains(peer)) {
            bool removed = true;
            for (const auto& device : devices) {
                if (device.identity != peer) continue;
                auto* reply = call(device.adapter_path.c_str(), "org.bluez.Adapter1", "RemoveDevice",
                                   g_variant_new("(o)", device.path.c_str()));
                if (!reply) { removed = false; break; }
                g_variant_unref(reply);
            }
            if (!removed) continue;
        }
        (void)pending.forget(peer);
    }
}
core::Result<bool> BluezBackend::bond_confirmed() {
    impl_->pump();
    if (!impl_->allowed) return Error{ErrorCode::helper_rejected};
    const auto captured = *impl_->allowed;
    if (!impl_->exact(captured, false) || !impl_->allowed) return Error{ErrorCode::helper_rejected};
    if (!impl_->comparison_accepted || !impl_->exact(captured, true)) return false;
    if (!impl_->allowed || impl_->now() >= impl_->window) return Error{ErrorCode::helper_rejected};
    if (!impl_->managed.remember(captured.device.identity)) return Error{ErrorCode::helper_rejected};
    (void)impl_->pending.forget(captured.device.identity);
    return true;
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
