// SPDX-License-Identifier: GPL-3.0-or-later
#include "Platform.hpp"

namespace aa::helper {
void BluezBackend::Impl::drain_events() {
    for (unsigned i = 0; i < 32 && g_main_context_pending(context); ++i) g_main_context_iteration(context, FALSE);
}
void BluezBackend::Impl::lost_path(const std::string& path) {
    ++incarnations[path];
    const auto matches = [&](const Observation& observed) {
        return observed.device.path == path || observed.device.adapter_path == path;
    };
    std::erase_if(observations, [&](const auto& item) { return matches(item.second); });
    if (allowed && matches(*allowed)) { reject_comparison(); allowed.reset(); }
    if (connected && matches(*connected)) retire(handoff_handle, HandoffFailure::eligibility);
}
void BluezBackend::Impl::watch_continuity() {
    removal_subscription = g_dbus_connection_signal_subscribe(bus, nullptr,
        "org.freedesktop.DBus.ObjectManager", "InterfacesRemoved", nullptr, nullptr, G_DBUS_SIGNAL_FLAGS_NONE,
        [](GDBusConnection*, const gchar* sender, const gchar*, const gchar*, const gchar*, GVariant* args, gpointer data) {
            auto& self = *static_cast<Impl*>(data);
            if (self.owner != sender || !g_variant_is_of_type(args, G_VARIANT_TYPE("(oas)"))) return;
            const char* path{}; GVariant* interfaces{};
            g_variant_get(args, "(&o@as)", &path, &interfaces);
            GVariantIter iter; g_variant_iter_init(&iter, interfaces); const char* interface{};
            while (g_variant_iter_next(&iter, "&s", &interface)) {
                const std::string_view iface{interface};
                if (iface == "org.bluez.Device1" || iface == "org.bluez.Adapter1") {
                    if (iface == "org.bluez.Adapter1") {
                        self.refused_bond_warned.clear();
                    } else if (const auto found = self.device_paths.find(path); found != self.device_paths.end()) {
                        std::erase(self.refused_bond_warned, found->second);
                    }
                    self.lost_path(path);
                    break;
                }
            }
            g_variant_unref(interfaces);
        }, this, nullptr);
    properties_subscription = g_dbus_connection_signal_subscribe(bus, nullptr,
        "org.freedesktop.DBus.Properties", "PropertiesChanged", nullptr, nullptr, G_DBUS_SIGNAL_FLAGS_NONE,
        // GDBusSignalCallback fixes the adjacent sender/path parameter order.
        // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
        [](GDBusConnection*, const gchar* sender, const gchar* path, const gchar*, const gchar*, GVariant* args, gpointer data) {
            auto& self = *static_cast<Impl*>(data);
            if (self.owner != sender || !g_variant_is_of_type(args, G_VARIANT_TYPE("(sa{sv}as)"))) return;
            const char* interface{}; GVariant *changed{}, *invalidated{};
            g_variant_get(args, "(&s@a{sv}@as)", &interface, &changed, &invalidated);
            if (std::string_view{interface} == "org.bluez.Device1") {
                gboolean paired{}, bonded{}, connected{};
                const bool has_paired = g_variant_lookup(changed, "Paired", "b", &paired) != FALSE;
                const bool has_bonded = g_variant_lookup(changed, "Bonded", "b", &bonded) != FALSE;
                const bool has_connected = g_variant_lookup(changed, "Connected", "b", &connected) != FALSE;
                bool bond_lost = (has_paired && !paired) || (has_bonded && !bonded);
                bool link_lost = bond_lost || (has_connected && !connected);
                GVariantIter iter; g_variant_iter_init(&iter, invalidated); const char* name{};
                while (g_variant_iter_next(&iter, "&s", &name)) {
                    const std::string_view prop{name};
                    if (prop == "Paired" || prop == "Bonded") {
                        bond_lost = true;
                        link_lost = true;
                    } else if (prop == "Connected") {
                        link_lost = true;
                    }
                }
                if (bond_lost) {
                    if (const auto found = self.device_paths.find(path); found != self.device_paths.end())
                        std::erase(self.refused_bond_warned, found->second);
                }
                if ((has_paired && paired) || (has_bonded && bonded))
                    self.next_warning_check = {};
                if (link_lost) self.lost_path(path);
            }
            g_variant_unref(changed); g_variant_unref(invalidated);
        }, this, nullptr);
}
void BluezBackend::Impl::retire(std::uint64_t handle, HandoffFailure reason) {
    if (!handle || handoff_handle != handle) return;
    if (bootstrap) { bootstrap->deny(reason); last_failure = bootstrap->failure(); }
    connected.reset(); handoff_handle = 0;
}
}
