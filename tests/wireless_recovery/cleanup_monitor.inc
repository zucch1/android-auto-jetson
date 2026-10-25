// SPDX-License-Identifier: GPL-3.0-or-later
class RecoveryCleanupMonitor final {
public:
    std::atomic<Clock::duration::rep> first{};
    std::atomic<std::uint64_t> handle{}, generation{};
    explicit RecoveryCleanupMonitor(const std::string& address) {
        bus_ = RecoveryNmIdentity::connect(address);
        if (!bus_) return;
        filter_ = g_dbus_connection_add_filter(bus_, [](GDBusConnection*, GDBusMessage* message,
            gboolean incoming, gpointer data) -> GDBusMessage* {
            auto& self = *static_cast<RecoveryCleanupMonitor*>(data);
            if (!incoming) return message;
            const auto type = g_dbus_message_get_message_type(message);
            const auto* member = g_dbus_message_get_member(message);
            if (type == G_DBUS_MESSAGE_TYPE_METHOD_CALL && member) {
                const std::string_view name{member};
                const auto* sender = g_dbus_message_get_sender(message);
                if (sender && (name == "PollBootstrapV1" || name == "GetAPCredentialsV1"))
                    self.requests_[{sender, g_dbus_message_get_serial(message)}] = name == "PollBootstrapV1";
                if (name == "Invalidate") {
                    Clock::duration::rep empty{};
                    (void)self.first.compare_exchange_strong(empty, Clock::now().time_since_epoch().count());
                }
            }
            if (type == G_DBUS_MESSAGE_TYPE_METHOD_CALL) { g_object_unref(message); return nullptr; }
            if (type == G_DBUS_MESSAGE_TYPE_METHOD_RETURN) {
                const auto* destination = g_dbus_message_get_destination(message);
                if (!destination) return message;
                const auto request = self.requests_.find({destination, g_dbus_message_get_reply_serial(message)});
                if (request == self.requests_.end()) return message;
                auto* body = g_dbus_message_get_body(message);
                std::uint32_t error = 1;
                const auto* shape = request->second ? "(utssu)" : "(utssssut)";
                if (body && g_variant_is_of_type(body, G_VARIANT_TYPE(shape))) g_variant_get_child(body, 0, "u", &error);
                if (!error) {
                    std::uint64_t value{};
                    g_variant_get_child(body, request->second ? 1 : 7, "t", &value);
                    if (request->second) self.handle = value;
                    else self.generation = value;
                }
                self.requests_.erase(request);
            }
            return message;
        }, this, nullptr);
        const char* rules[]{
            "type='method_call',interface='org.custom.AndroidAutoSystemHelper1',member='Invalidate'",
            "type='method_call',interface='org.custom.AndroidAutoSystemHelper1',member='PollBootstrapV1'",
            "type='method_call',interface='org.custom.AndroidAutoSystemHelper1',member='GetAPCredentialsV1'",
            "type='method_return'", nullptr};
        auto* reply = g_dbus_connection_call_sync(bus_, "org.freedesktop.DBus", "/org/freedesktop/DBus",
            "org.freedesktop.DBus.Monitoring", "BecomeMonitor", g_variant_new("(^asu)", rules, 0U),
            G_VARIANT_TYPE("()"), G_DBUS_CALL_FLAGS_NONE, 500, nullptr, nullptr);
        ready = reply != nullptr; if (reply) g_variant_unref(reply);
    }
    ~RecoveryCleanupMonitor() {
        if (bus_) { g_dbus_connection_remove_filter(bus_, filter_); RecoveryNmIdentity::close(bus_); }
    }
    bool ready{};
private:
    GDBusConnection* bus_{};
    guint filter_{};
    std::map<std::pair<std::string, guint32>, bool> requests_;
};
