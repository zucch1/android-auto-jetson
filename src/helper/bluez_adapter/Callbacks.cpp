// SPDX-License-Identifier: GPL-3.0-or-later
#include "Platform.hpp"
#include "../crypto_adapter/Crypto.hpp"
#include <gio/gunixfdlist.h>
#include <string_view>
#include <sys/stat.h>

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
    const auto reject = [&] { g_dbus_method_invocation_return_dbus_error(invocation, "org.bluez.Error.Rejected", "Not authorized"); };
    if (!self.current_owner() || self.owner != sender) { reject(); return; }
    const std::string_view method{member};
    if (method == "Release" || (std::string_view{interface} == "org.bluez.Agent1" && method == "Cancel")) {
        self.close_window();
        if (method == "Release") {
            if (std::string_view{interface} == "org.bluez.Profile1") self.registered = false;
            else self.agent_registered = false;
            self.observations.clear();
        }
        g_dbus_method_invocation_return_value(invocation, nullptr); return;
    }
    const char* device{};
    g_variant_get_child(args, 0, "&o", &device);
    if (std::string_view{interface} == "org.bluez.Profile1") {
        if (method == "RequestDisconnection") {
            if ((!self.connected || self.connected->device.path != device) &&
                (!self.allowed || self.allowed->device.path != device)) { reject(); return; }
            self.close_connection();
            g_dbus_method_invocation_return_value(invocation, nullptr); return;
        }
        if (method != "NewConnection" || !self.permitted(device, false) ||
            self.connection >= 0 || (self.bootstrap && self.bootstrap->live())) { reject(); return; }
        gint index{};
        g_variant_get_child(args, 1, "h", &index);
        auto* list = g_dbus_message_get_unix_fd_list(g_dbus_method_invocation_get_message(invocation));
        if (!list || index < 0 || index >= g_unix_fd_list_get_length(list)) { reject(); return; }
        GError* error = nullptr;
        self.connection = g_unix_fd_list_get(list, index, &error);
        g_clear_error(&error);
        if (self.connection < 0) { reject(); return; }
        struct stat info{};
        if (::fstat(self.connection, &info) != 0 || !S_ISSOCK(info.st_mode)) {
            self.close_connection(); reject(); return;
        }
        const auto handle = crypto_adapter::random_id();
        if (!handle || !handle.value()) { self.close_connection(); reject(); return; }
        self.retire(self.handoff_handle, HandoffFailure::eligibility);
        self.connected = self.allowed;
        self.handoff_handle = handle.value(); self.last_failure = HandoffFailure::none;
        self.bootstrap = std::make_shared<Bootstrap>(self.connection, self.now(), self.now);
        self.connection = -1;
        g_dbus_method_invocation_return_value(invocation, nullptr); return;
    }
    if (method == "AuthorizeService") {
        const char* uuid{}; g_variant_get_child(args, 1, "&s", &uuid);
        if (std::string_view{uuid} != kAaUuid || !self.permitted(device, false)) { reject(); return; }
    } else if (method == "RequestAuthorization") {
        if (!self.permitted(device, true)) { reject(); return; }
    } else {
        reject(); return;
    }
    g_dbus_method_invocation_return_value(invocation, nullptr);
}
}
