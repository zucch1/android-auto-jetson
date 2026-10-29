// SPDX-License-Identifier: GPL-3.0-or-later
#include "Platform.hpp"
#include "../crypto_adapter/Crypto.hpp"
#include <string_view>

namespace aa::helper {
bool BluezBackend::Impl::permitted(const char* path, bool pairing) {
    if (pairing && !pairing_enabled) return false;
    if (!allowed || now() >= window) { close_admission(); return false; }
    const auto captured = *allowed;
    if (captured.device.path != path || !exact(captured, !pairing)) return false;
    return allowed && now() < window;
}
void BluezBackend::Impl::method(GDBusConnection*, const gchar* sender, const gchar*, const gchar*,
                              const gchar*, GVariant* args, GDBusMethodInvocation* invocation, gpointer data) {
    auto& self = *static_cast<Impl*>(data);
    const auto* interface = g_dbus_method_invocation_get_interface_name(invocation);
    const auto* member = g_dbus_method_invocation_get_method_name(invocation);
    const auto* path = g_dbus_method_invocation_get_object_path(invocation);
    const auto reject = [&] { g_dbus_method_invocation_return_dbus_error(invocation, "org.bluez.Error.Rejected", "Not authorized"); };
    if (!self.current_owner() || self.owner != sender) { reject(); return; }
    const std::string_view method{member};
    const bool profile = std::string_view{interface} == "org.bluez.Profile1";
    if ((profile && std::string_view{path} != kProfilePath) ||
        (!profile && std::string_view{path} != kAgentPath)) { reject(); return; }
    if (profile && method == "Release") {
        self.close_connection(); self.registered = false;
        g_dbus_method_invocation_return_value(invocation, nullptr); return;
    }
    if (!profile && (method == "Release" || method == "Cancel")) {
        self.close_window();
        if (method == "Release") {
            self.agent_registered = false;
            self.observations.clear();
        }
        g_dbus_method_invocation_return_value(invocation, nullptr); return;
    }
    const char* device{};
    g_variant_get_child(args, 0, "&o", &device);
    if (profile) {
        if (method == "RequestDisconnection") {
            if ((!self.connected || self.connected->device.path != device) &&
                (!self.allowed || self.allowed->device.path != device)) { reject(); return; }
            // Completed RFCOMM teardown is not Bluetooth link loss (contract v2.6).
            if (!self.bootstrap || !self.bootstrap->complete()) self.close_connection();
            g_dbus_method_invocation_return_value(invocation, nullptr); return;
        }
        if (method != "NewConnection" || !self.registered || !self.permitted(device, false) ||
            self.connection >= 0 || (self.bootstrap && self.bootstrap->live())) { reject(); return; }
        self.connection = socket_fd(invocation);
        if (self.connection < 0) { reject(); return; }
        const auto handle = crypto_adapter::random_id();
        if (!handle || !handle.value()) { self.close_connection(); reject(); return; }
        self.retire(self.handoff_handle, HandoffFailure::eligibility);
        self.connected = self.allowed;
        self.handoff_handle = handle.value(); self.last_failure = HandoffFailure::none;
        self.bootstrap = std::make_shared<Bootstrap>(self.connection, self.now(), self.now);
        self.connection = -1;
        g_dbus_method_invocation_return_value(invocation, nullptr); return;
    }
    if (method == "RequestConfirmation") {
        guint32 number{};
        g_variant_get_child(args, 1, "u", &number);
        if (number > 999999 || !self.permitted(device, true) ||
            self.comparison_used || !self.allowed) { reject(); return; }
        const auto id = crypto_adapter::random_id();
        if (!id || !id.value()) { reject(); return; }
        self.comparison_used = true;
        self.comparison = Comparison{{id.value(), number, self.allowed->device.identity}, *self.allowed,
            self.window, G_DBUS_METHOD_INVOCATION(g_object_ref(invocation))};
        return;
    }
    if (method == "AuthorizeService") {
        const char* uuid{}; g_variant_get_child(args, 1, "&s", &uuid);
        const bool service = std::string_view{uuid} == kAaUuid || std::string_view{uuid} == kHfpUuid;
        if (!service || !self.permitted(device, false)) { reject(); return; }
    } else if (method == "RequestAuthorization") {
        if (!self.permitted(device, true)) { reject(); return; }
    } else {
        reject(); return;
    }
    g_dbus_method_invocation_return_value(invocation, nullptr);
}
}
