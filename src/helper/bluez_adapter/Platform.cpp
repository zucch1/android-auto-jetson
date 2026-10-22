// SPDX-License-Identifier: GPL-3.0-or-later
#include "Platform.hpp"
#include "Startup.hpp"
#include <unistd.h>

namespace aa::helper {
namespace {
constexpr auto interfaces = R"(<node>
<interface name="org.bluez.Profile1">
<method name="Release"/>
<method name="NewConnection"><arg type="o" direction="in"/><arg type="h" direction="in"/><arg type="a{sv}" direction="in"/></method>
<method name="RequestDisconnection"><arg type="o" direction="in"/></method>
</interface>
<interface name="org.bluez.Agent1">
<method name="Release"/><method name="Cancel"/>
<method name="RequestAuthorization"><arg type="o" direction="in"/></method>
<method name="AuthorizeService"><arg type="o" direction="in"/><arg type="s" direction="in"/></method>
<method name="RequestConfirmation"><arg type="o" direction="in"/><arg type="u" direction="in"/></method>
<method name="RequestPinCode"><arg type="o" direction="in"/><arg type="s" direction="out"/></method>
<method name="RequestPasskey"><arg type="o" direction="in"/><arg type="u" direction="out"/></method>
<method name="DisplayPinCode"><arg type="o" direction="in"/><arg type="s" direction="in"/></method>
<method name="DisplayPasskey"><arg type="o" direction="in"/><arg type="u" direction="in"/><arg type="q" direction="in"/></method>
</interface></node>)";
}
BluezBackend::Impl::Impl(const std::string& address) {
    context = g_main_context_new();
    g_main_context_push_thread_default(context);
    GError* error = nullptr;
    bus = bluez_adapter::connect_bus(address, context);
    if (bus) {
        g_dbus_connection_set_exit_on_close(bus, FALSE);
        owner_subscription = g_dbus_connection_signal_subscribe(bus, "org.freedesktop.DBus",
            "org.freedesktop.DBus", "NameOwnerChanged", "/org/freedesktop/DBus", "org.bluez",
            G_DBUS_SIGNAL_FLAGS_NONE,
            [](GDBusConnection*, const gchar*, const gchar*, const gchar*, const gchar*, GVariant*, gpointer data) {
                auto& self = *static_cast<Impl*>(data);
                self.reset();
                self.owner.clear();
                self.next_check = {};
            }, this, nullptr);
        xml = g_dbus_node_info_new_for_xml(interfaces, &error);
        g_clear_error(&error);
        const GDBusInterfaceVTable vtable{method, nullptr, nullptr, {nullptr}};
        if (xml) {
            profile_export = g_dbus_connection_register_object(bus, kProfilePath, xml->interfaces[0], &vtable, this, nullptr, &error);
            g_clear_error(&error);
            agent_export = g_dbus_connection_register_object(bus, kAgentPath, xml->interfaces[1], &vtable, this, nullptr, &error);
            g_clear_error(&error);
        }
    }
    g_main_context_pop_thread_default(context);
}
BluezBackend::Impl::~Impl() {
    close_window();
    if (registered) {
        if (auto* reply = call("/org/bluez", "org.bluez.ProfileManager1", "UnregisterProfile", g_variant_new("(o)", kProfilePath))) g_variant_unref(reply);
    }
    if (agent_registered) {
        if (auto* reply = call("/org/bluez", "org.bluez.AgentManager1", "UnregisterAgent", g_variant_new("(o)", kAgentPath))) g_variant_unref(reply);
    }
    if (bus) {
        if (owner_subscription) g_dbus_connection_signal_unsubscribe(bus, owner_subscription);
        if (profile_export) g_dbus_connection_unregister_object(bus, profile_export);
        if (agent_export) g_dbus_connection_unregister_object(bus, agent_export);
        g_dbus_connection_close_sync(bus, nullptr, nullptr);
        g_object_unref(bus);
    }
    while (g_main_context_pending(context)) g_main_context_iteration(context, FALSE);
    if (xml) g_dbus_node_info_unref(xml);
    g_main_context_unref(context);
}
GVariant* BluezBackend::Impl::call(const char* path, const char* interface, const char* member,
                                 GVariant* parameters, const GVariantType* type) {
    if (!bus || owner.empty()) {
        if (parameters) { g_variant_ref_sink(parameters); g_variant_unref(parameters); }
        return nullptr;
    }
    GError* error = nullptr;
    auto* result = g_dbus_connection_call_sync(bus, owner.c_str(), path, interface, member,
        parameters, type, G_DBUS_CALL_FLAGS_NO_AUTO_START, 150, nullptr, &error);
    g_clear_error(&error);
    return result;
}
bool BluezBackend::Impl::current_owner() {
    if (!bus || g_dbus_connection_is_closed(bus)) { reset(); owner.clear(); return false; }
    GError* error = nullptr;
    auto* reply = g_dbus_connection_call_sync(bus, "org.freedesktop.DBus", "/org/freedesktop/DBus",
        "org.freedesktop.DBus", "GetNameOwner", g_variant_new("(s)", "org.bluez"),
        G_VARIANT_TYPE("(s)"), G_DBUS_CALL_FLAGS_NO_AUTO_START, 100, nullptr, &error);
    g_clear_error(&error);
    std::string current;
    if (reply) { const char* value{}; g_variant_get(reply, "(&s)", &value); current = value; g_variant_unref(reply); }
    if (current != owner) { reset(); owner = current; }
    return !owner.empty();
}
bool BluezBackend::Impl::set(const std::string& adapter, const char* property, GVariant* value) {
    auto* reply = call(adapter.c_str(), "org.freedesktop.DBus.Properties", "Set",
        g_variant_new("(ssv)", "org.bluez.Adapter1", property, value), G_VARIANT_TYPE_UNIT);
    if (!reply) return false;
    g_variant_unref(reply);
    return true;
}
bool BluezBackend::Impl::quiet() {
    bool ok = true;
    for (const auto& adapter : adapters) {
        ok = set(adapter, "Discoverable", g_variant_new_boolean(FALSE)) && ok;
        ok = set(adapter, "Pairable", g_variant_new_boolean(FALSE)) && ok;
    }
    return ok;
}
void BluezBackend::Impl::close_connection() { if (connection >= 0) ::close(connection); connection = -1; }
void BluezBackend::Impl::close_window() {
    window = {};
    pairing_enabled = false;
    close_connection();
    if (allowed) {
        if (auto* reply = call(allowed->device.path.c_str(), "org.bluez.Device1", "CancelPairing", nullptr)) g_variant_unref(reply);
    }
    allowed.reset();
    quiet_pending = !quiet();
}
void BluezBackend::Impl::reset() {
    close_connection();
    allowed.reset(); window = {}; pairing_enabled = false; observations.clear(); adapters.clear();
    registered = false; agent_registered = false;
}
void BluezBackend::Impl::pump() {
    for (unsigned i = 0; i < 32 && g_main_context_pending(context); ++i) g_main_context_iteration(context, FALSE);
    if (Clock::now() >= next_check) {
        const bool alive = current_owner();
        if (alive && profile_requested && (!registered || !agent_registered)) (void)ensure();
        if (alive && quiet_pending) quiet_pending = !quiet();
        if (alive && allowed && Clock::now() >= window) close_window();
        next_check = Clock::now() + std::chrono::milliseconds(100);
    }
    std::erase_if(observations, [](const auto& item) { return Clock::now() >= item.second.expires; });
}
bool BluezBackend::Impl::ensure() {
    if (!profile_requested) return false;
    if (!current_owner() || !profile_export || !agent_export) return false;
    if (registered && agent_registered) return true;
    std::vector<Device> devices;
    if (!inventory(devices) || adapters.size() != 1 || !quiet()) return false;
    if (!agent_registered) {
        auto* reply = call("/org/bluez", "org.bluez.AgentManager1", "RegisterAgent", g_variant_new("(os)", kAgentPath, "NoInputNoOutput"));
        if (!reply) return false;
        g_variant_unref(reply); agent_registered = true;
    }
    auto* reply = call("/org/bluez", "org.bluez.AgentManager1", "RequestDefaultAgent", g_variant_new("(o)", kAgentPath));
    if (!reply) return false;
    g_variant_unref(reply);
    GVariantBuilder options;
    g_variant_builder_init(&options, G_VARIANT_TYPE_VARDICT);
    g_variant_builder_add(&options, "{sv}", "Name", g_variant_new_string("Android Auto"));
    g_variant_builder_add(&options, "{sv}", "Role", g_variant_new_string("server"));
    g_variant_builder_add(&options, "{sv}", "Channel", g_variant_new_uint16(8));
    g_variant_builder_add(&options, "{sv}", "RequireAuthentication", g_variant_new_boolean(TRUE));
    g_variant_builder_add(&options, "{sv}", "RequireAuthorization", g_variant_new_boolean(TRUE));
    g_variant_builder_add(&options, "{sv}", "AutoConnect", g_variant_new_boolean(FALSE));
    if (registered) { g_variant_builder_clear(&options); return true; }
    reply = call("/org/bluez", "org.bluez.ProfileManager1", "RegisterProfile", g_variant_new("(os@a{sv})", kProfilePath, kAaUuid, g_variant_builder_end(&options)));
    if (!reply) return false;
    g_variant_unref(reply); registered = true;
    return true;
}
BluezBackend::BluezBackend(const std::string& address) : impl_(std::make_unique<Impl>(address)) {}
BluezBackend::~BluezBackend() = default;
bool BluezBackend::available() const { return impl_->bus && !g_dbus_connection_is_closed(impl_->bus); }
void BluezBackend::tick() { impl_->pump(); }
void BluezBackend::discard(std::uint64_t handle) { impl_->observations.erase(handle); }
void BluezBackend::cleanup() noexcept { impl_->observations.clear(); impl_->close_window(); }
core::Result<State> BluezBackend::apply(Action action, std::uint32_t channel) {
    if (channel) return Error{ErrorCode::helper_rejected};
    switch (action) {
    case Action::register_profile:
        impl_->profile_requested = true;
        return impl_->ensure() ? core::Result<State>{State::active} : core::Result<State>{Error{ErrorCode::helper_unavailable}};
    case Action::close_pairing:
        cleanup();
        return impl_->quiet_pending ? core::Result<State>{Error{ErrorCode::helper_unavailable}} : core::Result<State>{State::inactive};
    case Action::unregister_profile: {
        impl_->profile_requested = false;
        cleanup();
        if (!impl_->current_owner()) return Error{ErrorCode::helper_unavailable};
        auto* reply = impl_->call("/org/bluez", "org.bluez.ProfileManager1", "UnregisterProfile", g_variant_new("(o)", kProfilePath));
        if (!reply) return Error{ErrorCode::helper_unavailable};
        g_variant_unref(reply); impl_->registered = false; return State::inactive;
    }
    default: { UnavailableBackend unavailable; return unavailable.apply(action, channel); }
    }
}
}
