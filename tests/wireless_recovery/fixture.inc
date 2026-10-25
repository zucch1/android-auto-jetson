// SPDX-License-Identifier: GPL-3.0-or-later
struct RecoveryEvents {
    Clock::time_point t0{};
    std::optional<long long> admission_closed, link_retired, fresh_acquisition, terminal_recovery;
    std::optional<long long> grace_expiry_observed, teardown_started, ap_inactive;
    int old_activation{}, new_activation{};
    long long elapsed() const {
        return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count();
    }
};
struct RecoveryDiagnostics final : diagnostics::DiagnosticSink {
    void emit_json(std::string_view) override {}
};
class WirelessRecovery : public NmDaemon {
protected:
    RecoveryEvents timing;
    RecoveryNmIdentity nm_identity;
    std::unique_ptr<RecoveryCleanupMonitor> cleanup_monitor;
    session::SteadyClock session_clock;
    RecoveryDiagnostics sink;
    diagnostics::Diagnostics diagnostics{sink};
    trust::SteadyClock trust_clock;
    ipc::test::FakePeerCredentials peers;
    ipc::PeerCredentialLookup* peer_lookup{&peers};
    ipc::test::RecordingEventSink events;
    ipc::test::SequencePairingIds ids;
    std::unique_ptr<trust::PairingPolicy> phones;
    std::unique_ptr<app::WirelessReceiver> receiver;
    std::unique_ptr<ipc::ControlService> control;
    core::ConsumerId consumer{};
    ipc::BusName caller{":1.900"};
    const ipc::BusName name{"org.custom.Consumer"};
    const trust::TransportIdentity identity{trust::WirelessIdentity{"02:00:00:00:00:01", "02:00:00:00:00:AA"}};
    std::string approvals;
    std::uint64_t old_handle{}, old_snapshot_generation{}, request{100};
    struct AuthorityObservation final { std::uint64_t handle, generation; };
    std::optional<AuthorityObservation> observe_snapshot() {
        if (!cleanup_monitor) return std::nullopt;
        const auto previous = old_handle;
        const bool observed = eventually([&] {
            pump();
            return cleanup_monitor->handle && cleanup_monitor->generation && cleanup_monitor->handle != previous;
        }, 5s);
        if (!observed) return std::nullopt;
        return AuthorityObservation{cleanup_monitor->handle, cleanup_monitor->generation};
    }
    bool negative_control() const { return std::getenv("AA_RECOVERY_NEGATIVE_CONTROL") != nullptr; }
    bool inactive_observed() {
        auto* connection = g_dbus_connection_new_for_address_sync(address.c_str(),
            static_cast<GDBusConnectionFlags>(G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT |
                G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION), nullptr, nullptr, nullptr);
        if (!connection) return false;
        const auto property = [&](const char* path, const char* interface, const char* key) {
            return g_dbus_connection_call_sync(connection, manager, path, "org.freedesktop.DBus.Properties", "Get",
                g_variant_new("(ss)", interface, key), G_VARIANT_TYPE("(v)"), G_DBUS_CALL_FLAGS_NO_AUTO_START, 500, nullptr, nullptr);
        };
        auto* state = property(active, "org.freedesktop.NetworkManager.Connection.Active", "State");
        auto* device = property(dev, "org.freedesktop.NetworkManager.Device", "ActiveConnection");
        bool inactive = false;
        if (state && device) {
            GVariant *state_value{}, *device_value{};
            g_variant_get(state, "(v)", &state_value); g_variant_get(device, "(v)", &device_value);
            inactive = g_variant_get_uint32(state_value) == 4 && std::string{g_variant_get_string(device_value, nullptr)} == "/";
            g_variant_unref(state_value); g_variant_unref(device_value);
        }
        if (state) g_variant_unref(state);
        if (device) g_variant_unref(device);
        (void)g_dbus_connection_close_sync(connection, nullptr, nullptr); g_object_unref(connection);
        if (inactive && nm->teardown_started) {
            timing.teardown_started = std::chrono::duration_cast<std::chrono::milliseconds>(
                Clock::time_point{Clock::duration{nm->teardown_started.load()}} - timing.t0).count();
        }
        auto* observer = RecoveryNmIdentity::connect(address);
        if (!observer) return false;
        const bool same_incarnation = nm_identity.current(observer) && !nm->replaced && !nm->device_remapped;
        RecoveryNmIdentity::close(observer);
        return inactive && same_incarnation && nm->starts == timing.old_activation &&
            nm->deactivations > 0 && !boundary->table;
    }
    bool occupied() {
        auto probe = transport::TcpListener::listen(wireless_test::host_endpoint().c_str(), 5277);
        return !probe;
    }
    void pump() { (void)receiver->poll(*control); }
    virtual app::WirelessLink::Config link_config() { return {}; }
    void setup_admission() {
        start(); acquire(99); ASSERT_TRUE(state(State::active));
        ASSERT_TRUE(store->approve(identity)); approvals = read_text(directory / "phones");
        ASSERT_EQ(bluez.command("paired"), "ok");
        receiver = std::make_unique<app::WirelessReceiver>(app::WiredSessionDeps{*store, session_clock, diagnostics}, *client, link_config());
        phones = std::make_unique<trust::PairingPolicy>(*store, trust_clock);
        control = std::make_unique<ipc::ControlService>(ipc::ControlServiceConfig{::getuid()},
            ipc::ControlServiceDeps{*peer_lookup, events, ids, *phones, &receiver->confirmation()});
        peers.add_connection(caller, ::getuid()); peers.set_owner(name, caller);
        auto registered = control->register_consumer(caller, name); ASSERT_TRUE(registered); consumer = registered.value();
        ASSERT_TRUE(control->start_projection(caller));
        cleanup_monitor = std::make_unique<RecoveryCleanupMonitor>(address); ASSERT_TRUE(cleanup_monitor->ready);
        ASSERT_NO_FATAL_FAILURE(admit());
        const auto observation = observe_snapshot(); ASSERT_TRUE(observation);
        old_handle = observation->handle;
        old_snapshot_generation = observation->generation;
        timing.old_activation = nm->starts;
        nm_identity.capture(address);
        pump(); ASSERT_TRUE(occupied());
    }
    void admit() {
        ASSERT_TRUE(receiver->reconnect(*control, identity));
        ASSERT_EQ(bluez.command("NewConnection"), "allowed");
        ASSERT_TRUE(eventually([&] { pump(); return occupied(); }, 10s));
        auto phone = std::async(std::launch::async, [&] {
            const auto read_record = [&](std::uint16_t expected) {
                AawDecoder decoder;
                bool received = false, terminal = false;
                EXPECT_TRUE(eventually([&] {
                    const auto raw = bluez.command("wire-read");
                    if (raw == "closed") { terminal = true; return true; }
                    if (raw == "open") return false;
                    decoder.feed(unhex(raw));
                    const auto decoded = decoder.next();
                    if (const auto* record = std::get_if<AawRecord>(&decoded)) {
                        if (record->id != expected) { terminal = true; return true; }
                        received = true;
                    } else if (std::get_if<AawError>(&decoded)) { terminal = true; return true; }
                    return received;
                }));
                EXPECT_FALSE(terminal) << "RFCOMM closed, mismatched or undecodable before record " << expected;
                return received;
            };
            ASSERT_TRUE(read_record(1));
            ASSERT_EQ(bluez.command("wire-send 00000002"), "ok");
            ASSERT_TRUE(read_record(3));
            ASSERT_EQ(bluez.command("wire-send 000200071800000200060800"), "ok");
            ASSERT_TRUE(eventually([&] { return bluez.command("wire-read") == "closed"; }));
        });
        ASSERT_TRUE(eventually([&] { pump(); return phone.wait_for(0ms) == std::future_status::ready; }, 5s));
        phone.get(); ASSERT_FALSE(HasFatalFailure());
        const auto terminal = observe_snapshot(); ASSERT_TRUE(terminal);
    }
    void closed() {
        auto listener = std::async(std::launch::async, [&]() -> std::optional<long long> {
            if (!eventually([&] { return !occupied(); }, 1s)) return std::nullopt;
            return timing.elapsed();
        });
        ASSERT_TRUE(eventually([&] { pump(); return listener.wait_for(0ms) == std::future_status::ready; }, 1s));
        timing.admission_closed = listener.get(); ASSERT_TRUE(timing.admission_closed);
        EXPECT_LE(*timing.admission_closed, 1000);
        EXPECT_FALSE(receiver->active()); timing.link_retired = timing.elapsed();
        EXPECT_FALSE(client->get_ap_credentials({consumer.value, 0}, old_handle));
    }
    void stays_retired() {
        for (int i = 0; i < 5; ++i) { pump(); EXPECT_FALSE(occupied()); }
        EXPECT_FALSE(client->get_ap_credentials({consumer.value, 0}, old_handle));
    }
    core::Result<void> acquire_only(Source source) {
        const auto result = receiver->acquire_ap(*control, source);
        return result ? core::Result<void>{} : core::Result<void>{result.error()};
    }
    void recover_ap() {
        ASSERT_TRUE(receiver->reconnect(*control, identity));
        ASSERT_TRUE(acquire_only({consumer.value, ++request}));
        ASSERT_TRUE(state(State::active)); timing.fresh_acquisition = timing.elapsed();
        timing.new_activation = nm->starts;
        ASSERT_NO_FATAL_FAILURE(admit()); timing.terminal_recovery = timing.elapsed();
        const auto observation = observe_snapshot(); ASSERT_TRUE(observation);
        EXPECT_NE(observation->handle, old_handle);
        if (timing.new_activation > timing.old_activation) { EXPECT_NE(observation->generation, old_snapshot_generation); }
        EXPECT_LE(*timing.terminal_recovery, 15000);
    }
    void TearDown() override {
        const auto cleanup_request = cleanup_monitor ? cleanup_monitor->first.load() : Clock::duration::rep{};
        if (control) control->shutdown();
        control.reset(); receiver.reset(); phones.reset();
        if (!approvals.empty()) {
            const bool trust_unmodified = read_text(directory / "phones") == approvals;
            EXPECT_TRUE(trust_unmodified);
            RecordProperty("trust_store_unmodified", trust_unmodified ? "true" : "false");
        }
        if (timing.t0 != Clock::time_point{}) {
            if (cleanup_request) {
                timing.teardown_started = std::chrono::duration_cast<std::chrono::milliseconds>(
                    Clock::time_point{Clock::duration{cleanup_request}} - timing.t0).count();
            }
            RecordProperty("t0_monotonic_ns", std::to_string(std::chrono::duration_cast<std::chrono::nanoseconds>(timing.t0.time_since_epoch()).count()));
            const auto record = [&](const char* key, std::optional<long long> value) {
                if (value) { RecordProperty(key, std::to_string(*value)); std::cout << key << '=' << *value << '\n'; }
            };
            record("admission_closed_ms", timing.admission_closed); record("link_retired_ms", timing.link_retired);
            record("fresh_acquisition_ms", timing.fresh_acquisition); record("terminal_recovery_ms", timing.terminal_recovery);
            record("teardown_started_ms", timing.teardown_started); record("old_ap_inactive_ms", timing.ap_inactive);
            record("grace_expiry_observed_ms", timing.grace_expiry_observed);
            RecordProperty("old_activation_id", timing.old_activation); RecordProperty("new_activation_id", timing.new_activation);
        }
        cleanup_monitor.reset();
        NmDaemon::TearDown();
    }
};
