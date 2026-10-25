// SPDX-License-Identifier: GPL-3.0-or-later
struct RecoveryNmIdentity {
    std::string owner, device;
    static std::pair<std::string, std::string> read(GDBusConnection* bus) {
        auto* reply = g_dbus_connection_call_sync(bus, "org.freedesktop.DBus", "/org/freedesktop/DBus",
            "org.freedesktop.DBus", "GetNameOwner", g_variant_new("(s)", manager), G_VARIANT_TYPE("(s)"),
            G_DBUS_CALL_FLAGS_NO_AUTO_START, 500, nullptr, nullptr);
        if (!reply) return {};
        const char* value{}; g_variant_get(reply, "(&s)", &value); std::string owner{value}; g_variant_unref(reply);
        reply = g_dbus_connection_call_sync(bus, owner.c_str(), root, manager, "GetDeviceByIpIface",
            g_variant_new("(s)", "wlan-test"), G_VARIANT_TYPE("(o)"), G_DBUS_CALL_FLAGS_NO_AUTO_START, 500, nullptr, nullptr);
        if (!reply) return {};
        g_variant_get(reply, "(&o)", &value); std::string device{value}; g_variant_unref(reply);
        return {owner, device};
    }
    bool current(GDBusConnection* bus) const {
        return !owner.empty() && device == dev && read(bus) == std::pair{owner, device};
    }
    bool inactive(const std::string& address) const {
        auto* bus = connect(address); if (!bus) return false;
        if (!current(bus)) { close(bus); return false; }
        const auto property = [&](const char* path, const char* interface, const char* key) {
            return g_dbus_connection_call_sync(bus, owner.c_str(), path, "org.freedesktop.DBus.Properties", "Get",
                g_variant_new("(ss)", interface, key), G_VARIANT_TYPE("(v)"),
                G_DBUS_CALL_FLAGS_NO_AUTO_START, 500, nullptr, nullptr);
        };
        auto* state = property(active, "org.freedesktop.NetworkManager.Connection.Active", "State");
        auto* device_state = property(dev, "org.freedesktop.NetworkManager.Device", "ActiveConnection");
        bool off{};
        if (state && device_state) {
            GVariant *s{}, *d{}; g_variant_get(state, "(v)", &s); g_variant_get(device_state, "(v)", &d);
            off = g_variant_get_uint32(s) == 4 && std::string{g_variant_get_string(d, nullptr)} == "/";
            g_variant_unref(s); g_variant_unref(d);
        }
        if (state) g_variant_unref(state);
        if (device_state) g_variant_unref(device_state);
        off = off && current(bus); close(bus); return off;
    }
    void capture(const std::string& address) {
        auto* bus = connect(address);
        ASSERT_NE(bus, nullptr);
        const auto identity = read(bus); owner = identity.first; device = identity.second;
        close(bus); ASSERT_FALSE(owner.empty()); ASSERT_EQ(device, dev);
    }
    static GDBusConnection* connect(const std::string& address) {
        return g_dbus_connection_new_for_address_sync(address.c_str(),
            static_cast<GDBusConnectionFlags>(G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT |
                G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION), nullptr, nullptr, nullptr);
    }
    static void close(GDBusConnection* bus) {
        (void)g_dbus_connection_close_sync(bus, nullptr, nullptr); g_object_unref(bus);
    }
};
