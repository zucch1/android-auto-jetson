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
BluezBackend::Impl::Impl(const std::string& address, Now clock) : now(std::move(clock)) {
    context = g_main_context_new();
    g_main_context_push_thread_default(context);
    GError* error = nullptr;
    bus = bluez_adapter::connect_bus(address, context);
    if (bus) {
        g_dbus_connection_set_exit_on_close(bus, FALSE);
        watch_continuity();
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
            hfp_export = g_dbus_connection_register_object(bus, kHfpPath, xml->interfaces[0], &vtable, this, nullptr, &error);
            g_clear_error(&error);
            agent_export = g_dbus_connection_register_object(bus, kAgentPath, xml->interfaces[1], &vtable, this, nullptr, &error);
            g_clear_error(&error);
        }
    }
    g_main_context_pop_thread_default(context);
}
BluezBackend::Impl::~Impl() {
    close_window();
    (void)unregister_profiles();
    if (agent_registered) {
        if (auto* reply = call("/org/bluez", "org.bluez.AgentManager1", "UnregisterAgent", g_variant_new("(o)", kAgentPath))) g_variant_unref(reply);
    }
    if (bus) {
        if (owner_subscription) g_dbus_connection_signal_unsubscribe(bus, owner_subscription);
        if (removal_subscription) g_dbus_connection_signal_unsubscribe(bus, removal_subscription);
        if (properties_subscription) g_dbus_connection_signal_unsubscribe(bus, properties_subscription);
        if (profile_export) g_dbus_connection_unregister_object(bus, profile_export);
        if (hfp_export) g_dbus_connection_unregister_object(bus, hfp_export);
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
void BluezBackend::Impl::close_connection() {
    if (connection >= 0) ::close(connection);
    connection = -1;
    if (bootstrap) { bootstrap->deny(HandoffFailure::eligibility); last_failure = bootstrap->failure(); }
    connected.reset(); handoff_handle = 0;
}
void BluezBackend::Impl::close_admission() {
    reject_comparison();
    window = {};
    pairing_enabled = false;
    if (allowed) {
        if (auto* reply = call(allowed->device.path.c_str(), "org.bluez.Device1", "CancelPairing", nullptr)) g_variant_unref(reply);
    }
    allowed.reset();
    quiet_pending = !quiet();
}
void BluezBackend::Impl::close_window() { close_hfp(); close_connection(); close_admission(); }
void BluezBackend::Impl::reset() {
    reject_comparison();
    ++owner_epoch;
    close_hfp();
    close_connection();
    allowed.reset(); window = {}; pairing_enabled = false; observations.clear(); adapters.clear();
    registered = false; hfp_registered = false; agent_registered = false;
}
void BluezBackend::Impl::pump() {
    drain_events();
    if (comparison && now() >= comparison->deadline) close_admission();
    if (now() >= next_check) {
        const bool alive = current_owner();
        if (alive && profile_requested && (!registered || !hfp_registered || !agent_registered)) (void)ensure();
        if (alive && quiet_pending) quiet_pending = !quiet();
        if (alive && allowed && now() >= window) {
            close_admission();
        }
        next_check = now() + std::chrono::milliseconds(100);
    }
    std::erase_if(observations, [this](const auto& item) { return now() >= item.second.expires; });
    tick_hfp();
    if (bootstrap && bootstrap->live()) {
        const auto captured = bootstrap;
        const auto handle = handoff_handle;
        captured->tick(now(), snapshots, fence, true);
        if (handoff_handle == handle) last_failure = captured->failure();
    }
}
bool BluezBackend::Impl::ensure() {
    if (!profile_requested) return false;
    if (!current_owner() || !profile_export || !hfp_export || !agent_export) return false;
    if (registered && hfp_registered && agent_registered) return true;
    std::vector<Device> devices;
    if (!inventory(devices) || adapters.size() != 1) return false;
    if ((!pairing_enabled || !allowed) && !quiet()) return false;
    if (!agent_registered) {
        auto* reply = call("/org/bluez", "org.bluez.AgentManager1", "RegisterAgent", g_variant_new("(os)", kAgentPath, "DisplayYesNo"));
        if (!reply) return false;
        g_variant_unref(reply); agent_registered = true;
    }
    auto* reply = call("/org/bluez", "org.bluez.AgentManager1", "RequestDefaultAgent", g_variant_new("(o)", kAgentPath));
    if (!reply) return false;
    g_variant_unref(reply);
    return register_profiles();
}
BluezBackend::BluezBackend(const std::string& address, Now now) : impl_(std::make_unique<Impl>(address, std::move(now))) {}
BluezBackend::~BluezBackend() = default;
bool BluezBackend::available() const { return impl_->bus && !g_dbus_connection_is_closed(impl_->bus); }
core::Result<ObservedPeer> BluezBackend::handoff() {
    if (impl_->bootstrap && impl_->bootstrap->expire()) impl_->last_failure = impl_->bootstrap->failure();
    if (!impl_->bootstrap || (!impl_->bootstrap->live() && !impl_->bootstrap->complete()) || !impl_->connected) return Error{ErrorCode::helper_rejected};
    const auto handle = impl_->handoff_handle;
    const auto captured = *impl_->connected;
    if (!impl_->exact(captured, true)) { impl_->retire(handle, HandoffFailure::eligibility); return Error{ErrorCode::helper_rejected}; }
    if (!handle || impl_->handoff_handle != handle || !impl_->connected) return Error{ErrorCode::helper_rejected};
    if (impl_->bootstrap->expire()) { impl_->last_failure = impl_->bootstrap->failure(); return Error{ErrorCode::helper_rejected}; }
    return ObservedPeer{handle, captured.device.identity};
}
SnapshotDeadline BluezBackend::handoff_deadline() const {
    return impl_->bootstrap ? impl_->bootstrap->deadline() : SnapshotDeadline{impl_->now(), impl_->now};
}
HandoffFailure BluezBackend::handoff_failure() const { return impl_->last_failure; }
void BluezBackend::fail_handoff(std::uint64_t handle, HandoffFailure reason) { impl_->retire(handle, reason); }
void BluezBackend::snapshot_provider(const SnapshotProvider& provider) { impl_->snapshots = provider; }
void BluezBackend::snapshot_fence(const SnapshotFence& fence) { impl_->fence = fence; }
core::Result<void> BluezBackend::authorize_handoff(std::uint64_t handle, bool allow, bool ready) {
    const auto peer = handoff();
    if (!peer || peer.value().handle != handle) return Error{ErrorCode::helper_rejected};
    if (!allow) {
        impl_->bootstrap->deny(HandoffFailure::denied); impl_->last_failure = HandoffFailure::denied; return {};
    }
    // v2.7: completed confirmation (allow, no listener) revalidates the pinned
    // snapshot like ready authorization, so either success is renewing evidence.
    const bool completed = impl_->bootstrap->complete();
    if ((!ready && !completed) || !impl_->snapshots) {
        impl_->bootstrap->deny(HandoffFailure::eligibility); impl_->last_failure = HandoffFailure::eligibility;
        return Error{ErrorCode::helper_rejected};
    }
    const auto snapshot = impl_->snapshots();
    const auto current = handoff();
    if (!current || current.value().handle != handle) return Error{ErrorCode::helper_rejected};
    if (!snapshot || !impl_->bootstrap->authorize(snapshot.value(), ready || completed, impl_->now())) {
        impl_->retire(handle, HandoffFailure::snapshot);
        return Error{ErrorCode::helper_unavailable};
    }
    return {};
}
void BluezBackend::tick() { impl_->pump(); }
void BluezBackend::discard(std::uint64_t handle) { impl_->observations.erase(handle); }
void BluezBackend::cleanup() noexcept { impl_->observations.clear(); impl_->close_window(); }
void BluezBackend::retire_lease() noexcept { impl_->close_window(); }
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
        if (!impl_->unregister_profiles()) return Error{ErrorCode::helper_unavailable};
        return State::inactive;
    }
    default: { UnavailableBackend unavailable; return unavailable.apply(action, channel); }
    }
}
}
