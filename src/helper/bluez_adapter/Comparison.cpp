// SPDX-License-Identifier: GPL-3.0-or-later
#include "Platform.hpp"

namespace aa::helper {
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
    auto* invocation = impl_->comparison->invocation;
    impl_->comparison.reset();
    g_dbus_method_invocation_return_value(invocation, nullptr);
    g_object_unref(invocation);
    return {};
}
}
