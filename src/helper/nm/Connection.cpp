// SPDX-License-Identifier: GPL-3.0-or-later
#include "Platform.hpp"
#include <fcntl.h>
#include <limits>
#include <unistd.h>
#include <arpa/inet.h>
#include <cctype>
#include <algorithm>

namespace aa::helper::nm {
namespace {
constexpr auto root = "/org/freedesktop/NetworkManager";
constexpr auto manager = "org.freedesktop.NetworkManager";
constexpr auto device_interface = "org.freedesktop.NetworkManager.Device";
constexpr auto active_interface = "org.freedesktop.NetworkManager.Connection.Active";
struct Startup final {
    const gint64 deadline{g_get_monotonic_time() + 300 * G_TIME_SPAN_MILLISECOND};
    GCancellable* cancel{g_cancellable_new()};
    GIOStream* stream{};
    GSocket* abort_socket{};
    GDBusConnection* bus{};
    bool expired{}, done{};
    void expire() {
        if (expired) return;
        expired = true;
        g_cancellable_cancel(cancel);
        // Never touch the handed-off stream/socket object: Hello ignores cancel,
        // so shutdown an independent duplicate to interrupt the kernel endpoint.
        if (abort_socket) {
            GError* error{};
            (void)g_socket_shutdown(abort_socket, TRUE, TRUE, &error);
            g_clear_error(&error);
        }
    }
    bool check_expiry() {
        if (g_get_monotonic_time() >= deadline) expire();
        return expired;
    }
    void close_late_bus() {
        done = false;
        g_dbus_connection_close(bus, nullptr, [](GObject* source, GAsyncResult* result, gpointer data) {
            auto& state = *static_cast<Startup*>(data);
            (void)g_dbus_connection_close_finish(G_DBUS_CONNECTION(source), result, nullptr);
            g_clear_object(&state.bus);
            state.done = true;
        }, this);
    }
    static void initialized(GObject*, GAsyncResult* result, gpointer data) {
        auto& state = *static_cast<Startup*>(data);
        state.bus = g_dbus_connection_new_finish(result, nullptr);
        if (state.bus) g_dbus_connection_set_exit_on_close(state.bus, FALSE);
        if (state.check_expiry() && state.bus) state.close_late_bus();
        else state.done = true;
    }
    static void acquired(GObject*, GAsyncResult* result, gpointer data) {
        auto& state = *static_cast<Startup*>(data);
        state.stream = g_dbus_address_get_stream_finish(result, nullptr, nullptr);
        if (state.check_expiry() || !state.stream || !G_IS_SOCKET_CONNECTION(state.stream)) {
            state.done = true; return;
        }
        auto* socket = g_socket_connection_get_socket(G_SOCKET_CONNECTION(state.stream));
        const int original = g_socket_get_fd(socket);
        const int duplicate = original >= 0 ? ::fcntl(original, F_DUPFD_CLOEXEC, 0) : -1;
        if (duplicate < 0) { state.done = true; return; }
        state.abort_socket = g_socket_new_from_fd(duplicate, nullptr);
        if (!state.abort_socket) { ::close(duplicate); state.done = true; return; }
        if (state.check_expiry()) { state.done = true; return; }
        g_dbus_connection_new(state.stream, nullptr,
            static_cast<GDBusConnectionFlags>(G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT |
                                             G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION),
            nullptr, state.cancel, initialized, &state);
    }
};
}
Connection::Connection(const std::string& address) {
    auto* context = g_main_context_new();
    g_main_context_push_thread_default(context);
    Startup pending;
    auto* timeout = g_timeout_source_new(0);
    g_source_set_ready_time(timeout, pending.deadline);
    g_source_set_callback(timeout, [](gpointer data) -> gboolean {
        static_cast<Startup*>(data)->expire(); return G_SOURCE_REMOVE;
    }, &pending, nullptr);
    g_source_attach(timeout, context);
    g_dbus_address_get_stream(address.c_str(), pending.cancel, Startup::acquired, &pending);
    while (!pending.done) g_main_context_iteration(context, TRUE);
    if (pending.check_expiry() && pending.bus) {
        pending.close_late_bus();
        while (!pending.done) g_main_context_iteration(context, TRUE);
    }
    bus_ = pending.bus;
    g_source_destroy(timeout); g_source_unref(timeout);
    g_clear_object(&pending.stream); g_clear_object(&pending.abort_socket); g_object_unref(pending.cancel);
    g_main_context_pop_thread_default(context); g_main_context_unref(context);
    if (!bus_) return;
    g_dbus_connection_set_exit_on_close(bus_, FALSE);
    auto* reply = g_dbus_connection_call_sync(bus_, "org.freedesktop.DBus", "/org/freedesktop/DBus",
        "org.freedesktop.DBus", "GetNameOwner", g_variant_new("(s)", manager), G_VARIANT_TYPE("(s)"),
        G_DBUS_CALL_FLAGS_NONE, 300, nullptr, nullptr);
    if (reply) { const char* owner{}; g_variant_get(reply, "(&s)", &owner); owner_ = owner; g_variant_unref(reply); }
}
Connection::~Connection() {
    if (bus_) { (void)g_dbus_connection_close_sync(bus_, nullptr, nullptr); g_object_unref(bus_); }
}
bool Connection::healthy() const {
    if (!bus_ || g_dbus_connection_is_closed(bus_) || owner_.empty()) return false;
    const int timeout = call_timeout();
    if (!timeout) return false;
    auto* reply = g_dbus_connection_call_sync(bus_, "org.freedesktop.DBus", "/org/freedesktop/DBus",
        "org.freedesktop.DBus", "GetNameOwner", g_variant_new("(s)", manager), G_VARIANT_TYPE("(s)"),
        G_DBUS_CALL_FLAGS_NONE, timeout, nullptr, nullptr);
    if (!reply) return false;
    const char* owner{};
    g_variant_get(reply, "(&s)", &owner);
    const bool same = owner_ == owner;
    g_variant_unref(reply);
    return same;
}
GVariant* Connection::call(const std::string& path, const char* interface, const char* method,
                            GVariant* args, const GVariantType* result, GError** error) {
    const int timeout = call_timeout();
    if (!bus_ || owner_.empty() || !timeout) {
        if (args) { g_variant_ref_sink(args); g_variant_unref(args); }
        return nullptr;
    }
    return g_dbus_connection_call_sync(bus_, owner_.c_str(), path.c_str(), interface, method,
        args, result, G_DBUS_CALL_FLAGS_NO_AUTO_START, timeout, nullptr, error);
}
int Connection::call_timeout() const {
    if (collection_deadline_.until == std::chrono::steady_clock::time_point::max()) return 300;
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(collection_deadline_.until - collection_deadline_.now()).count();
    return static_cast<int>(std::clamp<std::int64_t>(left, 0, 300));
}
std::string Connection::device(const std::string& interface) {
    auto* reply = call(root, manager, "GetDeviceByIpIface", g_variant_new("(s)", interface.c_str()), G_VARIANT_TYPE("(o)"));
    if (!reply) return {};
    const char* path{}; g_variant_get(reply, "(&o)", &path);
    std::string result{path}; g_variant_unref(reply); return result;
}
std::string Connection::text_property(const std::string& path, const char* interface, const char* property) {
    auto* reply = call(path, "org.freedesktop.DBus.Properties", "Get", g_variant_new("(ss)", interface, property), G_VARIANT_TYPE("(v)"));
    if (!reply) return {};
    GVariant* value{}; g_variant_get(reply, "(v)", &value);
    std::string result;
    if (g_variant_is_of_type(value, G_VARIANT_TYPE_STRING) || g_variant_is_of_type(value, G_VARIANT_TYPE_OBJECT_PATH))
        result = g_variant_get_string(value, nullptr);
    g_variant_unref(value); g_variant_unref(reply); return result;
}
unsigned Connection::number_property(const std::string& path, const char* interface, const char* property) {
    auto* reply = call(path, "org.freedesktop.DBus.Properties", "Get", g_variant_new("(ss)", interface, property), G_VARIANT_TYPE("(v)"));
    if (!reply) return std::numeric_limits<unsigned>::max();
    GVariant* value{}; g_variant_get(reply, "(v)", &value);
    const auto result = g_variant_is_of_type(value, G_VARIANT_TYPE_UINT32) ? g_variant_get_uint32(value) : std::numeric_limits<unsigned>::max();
    g_variant_unref(value); g_variant_unref(reply); return result;
}
bool Connection::start(const Policy& policy, const Credentials& secret, const std::string& device_path) {
    GVariantBuilder settings;
    g_variant_builder_init(&settings, G_VARIANT_TYPE("a{sa{sv}}"));
    auto section = [&settings](const char* name, std::initializer_list<std::pair<const char*, GVariant*>> values) {
        GVariantBuilder fields;
        g_variant_builder_init(&fields, G_VARIANT_TYPE_VARDICT);
        for (const auto& [key, value] : values) g_variant_builder_add(&fields, "{sv}", key, value);
        g_variant_builder_add(&settings, "{s@a{sv}}", name, g_variant_builder_end(&fields));
    };
    section("connection", {{"id", g_variant_new_string("aa-system-helper-ap-v1")},
        {"uuid", g_variant_new_string("589c713e-9d8b-452a-9e53-f1f4289ee035")},
        {"type", g_variant_new_string("802-11-wireless")},
        {"interface-name", g_variant_new_string(policy.interface.c_str())},
        {"autoconnect", g_variant_new_boolean(FALSE)}});
    section("802-11-wireless", {{"ssid", g_variant_new_fixed_array(G_VARIANT_TYPE_BYTE, secret.ssid.data(), secret.ssid.size(), 1)},
        {"mode", g_variant_new_string("ap")}, {"band", g_variant_new_string("a")},
        {"channel", g_variant_new_uint32(policy.channel)}, {"powersave", g_variant_new_uint32(2)}});
    const char* rsn[]{"rsn", nullptr};
    const char* ccmp[]{"ccmp", nullptr};
    section("802-11-wireless-security", {{"key-mgmt", g_variant_new_string("wpa-psk")},
        {"psk", g_variant_new_string(secret.psk.c_str())}, {"proto", g_variant_new_strv(rsn, -1)},
        {"pairwise", g_variant_new_strv(ccmp, -1)}, {"group", g_variant_new_strv(ccmp, -1)}});
    section("ipv4", {{"method", g_variant_new_string("shared")}, {"never-default", g_variant_new_boolean(TRUE)}});
    section("ipv6", {{"method", g_variant_new_string("disabled")}});
    GVariantBuilder options;
    g_variant_builder_init(&options, G_VARIANT_TYPE_VARDICT);
    g_variant_builder_add(&options, "{sv}", "persist", g_variant_new_string("volatile"));
    g_variant_builder_add(&options, "{sv}", "bind-activation", g_variant_new_string("dbus-client"));
    GError* error{};
    auto* reply = call(root, manager, "AddAndActivateConnection2",
        g_variant_new("(@a{sa{sv}}oo@a{sv})", g_variant_builder_end(&settings), device_path.c_str(), "/",
                      g_variant_builder_end(&options)), G_VARIANT_TYPE("(ooa{sv})"), &error);
    if (!reply) {
        unknown_activation_ = !error || !g_dbus_error_is_remote_error(error);
        if (error) g_error_free(error);
        return false;
    }
    const char *profile{}, *active{};
    GVariant* result{};
    g_variant_get(reply, "(&o&o@a{sv})", &profile, &active, &result);
    profile_ = profile; active_ = active;
    g_variant_unref(result); g_variant_unref(reply);
    unknown_activation_ = !profile_.starts_with("/org/freedesktop/NetworkManager/Settings/") ||
                          !active_.starts_with("/org/freedesktop/NetworkManager/ActiveConnection/");
    return !unknown_activation_;
}
bool Connection::active() const {
    return !active_.empty() && const_cast<Connection*>(this)->number_property(active_, active_interface, "State") == 2;
}
bool Connection::identity_current(const Policy& policy, const std::string& path) {
    return device(policy.interface) == path && text_property(path, device_interface, "Interface") == policy.interface &&
        text_property(path, device_interface, "ActiveConnection") == active_ && active() && healthy();
}
core::Result<std::pair<std::string, std::string>> Connection::endpoint(const Policy& policy, const std::string& path) {
    constexpr auto wireless = "org.freedesktop.NetworkManager.Device.Wireless";
    if (!healthy() || !active() || device(policy.interface) != path ||
        text_property(path, device_interface, "Interface") != policy.interface ||
        text_property(path, device_interface, "ActiveConnection") != active_ ||
        number_property(path, device_interface, "DeviceType") != 2 || number_property(path, wireless, "Mode") != 3)
        return Error{ErrorCode::helper_unavailable};
    auto mac = text_property(path, wireless, "HwAddress");
    if (mac.size() != 17) return Error{ErrorCode::helper_rejected};
    for (std::size_t i = 0; i < mac.size(); ++i) {
        if (i % 3 == 2 ? mac[i] != ':' : !std::isxdigit(static_cast<unsigned char>(mac[i])))
            return Error{ErrorCode::helper_rejected};
        mac[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(mac[i])));
    }
    if (mac == "00:00:00:00:00:00" || (std::stoi(mac.substr(0, 2), nullptr, 16) & 1))
        return Error{ErrorCode::helper_rejected};
    const auto config = text_property(path, device_interface, "Ip4Config");
    if (config.empty() || config == "/") return Error{ErrorCode::helper_unavailable};
    auto* reply = call(config, "org.freedesktop.DBus.Properties", "Get",
        g_variant_new("(ss)", "org.freedesktop.NetworkManager.IP4Config", "AddressData"), G_VARIANT_TYPE("(v)"));
    if (!reply) return Error{ErrorCode::helper_unavailable};
    GVariant* addresses{}; g_variant_get(reply, "(v)", &addresses);
    std::string ip;
    if (g_variant_is_of_type(addresses, G_VARIANT_TYPE("aa{sv}")) && g_variant_n_children(addresses) == 1) {
        auto* entry = g_variant_get_child_value(addresses, 0); const char* text{};
        if (g_variant_lookup(entry, "address", "&s", &text)) ip = text;
        g_variant_unref(entry);
    }
    g_variant_unref(addresses); g_variant_unref(reply);
    in_addr parsed{};
    if (::inet_pton(AF_INET, ip.c_str(), &parsed) != 1 || ip == "0.0.0.0" || ip == "255.255.255.255" || ip.starts_with("127."))
        return Error{ErrorCode::helper_rejected};
    if (!identity_current(policy, path)) return Error{ErrorCode::helper_unavailable};
    return std::pair{mac, ip};
}
bool Connection::stop(const std::string& device_path) {
    if (unknown_activation_ || !healthy()) {
        if (bus_) (void)g_dbus_connection_close_sync(bus_, nullptr, nullptr);
        return false;
    }
    if (!active_.empty() && !deactivation_ack_) {
        auto* reply = call(root, manager, "DeactivateConnection", g_variant_new("(o)", active_.c_str()), G_VARIANT_TYPE("()"));
        if (!reply) return false;
        g_variant_unref(reply);
        deactivation_ack_ = true;
    }
    if (text_property(device_path, device_interface, "ActiveConnection") != "/") return false;
    if (!profile_.empty()) {
        auto* reply = call(profile_, "org.freedesktop.NetworkManager.Settings.Connection", "Delete", nullptr, G_VARIANT_TYPE("()"));
        if (reply) g_variant_unref(reply);
    }
    active_.clear(); profile_.clear(); deactivation_ack_ = false; return true;
}
}
