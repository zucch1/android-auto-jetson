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
    if (allowed && matches(*allowed)) allowed.reset();
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
                if (std::string_view{interface} == "org.bluez.Device1" || std::string_view{interface} == "org.bluez.Adapter1") {
                    self.lost_path(path); break;
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
                gboolean value{};
                bool lost = (g_variant_lookup(changed, "Paired", "b", &value) && !value) ||
                    (g_variant_lookup(changed, "Bonded", "b", &value) && !value);
                GVariantIter iter; g_variant_iter_init(&iter, invalidated); const char* name{};
                while (g_variant_iter_next(&iter, "&s", &name)) {
                    if (std::string_view{name} == "Paired" || std::string_view{name} == "Bonded") lost = true;
                }
                if (lost) self.lost_path(path);
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
