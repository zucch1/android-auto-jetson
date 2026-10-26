// SPDX-License-Identifier: GPL-3.0-or-later
class RecoveryBusPeer final : public ipc::PeerCredentialLookup {
public:
    GDBusConnection* connection{};
    explicit RecoveryBusPeer(const std::string& address) {
        connection = g_dbus_connection_new_for_address_sync(address.c_str(),
            static_cast<GDBusConnectionFlags>(G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT |
                G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION), nullptr, nullptr, nullptr);
    }
    ~RecoveryBusPeer() override {
        if (connection) { (void)g_dbus_connection_close_sync(connection, nullptr, nullptr); g_object_unref(connection); }
    }
    core::Result<std::uint32_t> unix_uid(const ipc::BusName& peer) const override {
        auto* reply = call("GetConnectionUnixUser", peer.value, G_VARIANT_TYPE("(u)"));
        if (!reply) return Error{ErrorCode::ipc_peer_unauthorized};
        guint32 uid{}; g_variant_get(reply, "(u)", &uid); g_variant_unref(reply); return uid;
    }
    core::Result<ipc::BusName> name_owner(const ipc::BusName& name) const override {
        auto* reply = call("GetNameOwner", name.value, G_VARIANT_TYPE("(s)"));
        if (!reply) return Error{ErrorCode::ipc_peer_unauthorized};
        const char* owner{}; g_variant_get(reply, "(&s)", &owner);
        ipc::BusName result{owner}; g_variant_unref(reply); return result;
    }
private:
    GVariant* call(const char* method, const std::string& name, const GVariantType* type) const {
        return g_dbus_connection_call_sync(connection, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus",
            method, g_variant_new("(s)", name.c_str()), type,
            G_DBUS_CALL_FLAGS_NO_AUTO_START, 500, nullptr, nullptr);
    }
};
class WirelessRecoveryConsumer : public WirelessRecoveryProcess {
protected:
    std::unique_ptr<RecoveryBusPeer> lookup;
    GDBusConnection* owner{};
    void own_name() {
        owner = g_dbus_connection_new_for_address_sync(address.c_str(),
            static_cast<GDBusConnectionFlags>(G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT |
                G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION), nullptr, nullptr, nullptr);
        ASSERT_NE(owner, nullptr);
        auto* reply = g_dbus_connection_call_sync(owner, "org.freedesktop.DBus", "/org/freedesktop/DBus",
            "org.freedesktop.DBus", "RequestName", g_variant_new("(su)", name.value.c_str(), 4U),
            G_VARIANT_TYPE("(u)"), G_DBUS_CALL_FLAGS_NONE, 500, nullptr, nullptr);
        ASSERT_NE(reply, nullptr); guint32 result{}; g_variant_get(reply, "(u)", &result);
        g_variant_unref(reply); ASSERT_EQ(result, 1U);
        caller = ipc::BusName{g_dbus_connection_get_unique_name(owner)};
    }
    void lose_owner() {
        ASSERT_NE(owner, nullptr);
        ASSERT_TRUE(g_dbus_connection_close_sync(owner, nullptr, nullptr));
        g_object_unref(owner); owner = nullptr;
    }
    void SetUp() override {
        NmDaemon::SetUp(); ASSERT_FALSE(HasFatalFailure());
        lookup = std::make_unique<RecoveryBusPeer>(address);
        ASSERT_NE(lookup->connection, nullptr); peer_lookup = lookup.get();
        ASSERT_NO_FATAL_FAILURE(own_name());
    }
    void TearDown() override {
        WirelessRecovery::TearDown();
        if (owner) { (void)g_dbus_connection_close_sync(owner, nullptr, nullptr); g_object_unref(owner); owner = nullptr; }
        lookup.reset();
    }
};
