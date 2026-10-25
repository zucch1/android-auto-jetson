// SPDX-License-Identifier: GPL-3.0-or-later
class WirelessRecoveryProcess : public WirelessRecovery {
protected:
    void fresh_receiver() {
        receiver = std::make_unique<app::WirelessReceiver>(
            app::WiredSessionDeps{*store, session_clock, diagnostics}, *client, app::WirelessLink::Config{});
        phones = std::make_unique<trust::PairingPolicy>(*store, trust_clock);
        control = std::make_unique<ipc::ControlService>(ipc::ControlServiceConfig{::getuid()},
            ipc::ControlServiceDeps{*peer_lookup, events, ids, *phones, &receiver->confirmation()});
        const auto registered = control->register_consumer(caller, name);
        ASSERT_TRUE(registered); consumer = registered.value();
        ASSERT_TRUE(control->start_projection(caller));
    }
    void dispose_receiver() {
        control.reset(); receiver.reset(); phones.reset(); client.reset();
    }
    void recover_process() {
        ASSERT_TRUE(receiver->reconnect(*control, identity));
        ASSERT_TRUE(acquire_only({consumer.value, ++request}));
        ASSERT_TRUE(state(State::active)); timing.fresh_acquisition = timing.elapsed();
        timing.new_activation = nm->starts;
        ASSERT_NO_FATAL_FAILURE(admit()); timing.terminal_recovery = timing.elapsed();
        ASSERT_TRUE(observe_snapshot());
        EXPECT_GT(timing.new_activation, timing.old_activation);
        EXPECT_LE(*timing.terminal_recovery, 15000);
    }
    void cleanup_observed(bool helper_stopped) {
        ASSERT_TRUE(eventually([&] { return inactive_observed(); }, 5s));
        timing.ap_inactive = timing.elapsed();
        EXPECT_LE(*timing.ap_inactive, 5000);
        ASSERT_TRUE(timing.teardown_started);
        EXPECT_GE(*timing.teardown_started, 0);
        EXPECT_EQ(bluez.command("wire-read"), "closed");
        if (helper_stopped) {
            ASSERT_TRUE(eventually([&] { return bluez.command("state").find("\"profile\": []") != std::string::npos; }));
        }
        const auto bluetooth = bluez.command("state");
        if (helper_stopped) { EXPECT_NE(bluetooth.find("\"profile\": []"), std::string::npos); }
        else { EXPECT_NE(bluetooth.find("4de17a00-52cb-11e6-bdf4-0800200c9a66"), std::string::npos); }
        EXPECT_NE(bluetooth.find("\"Pairable\": false"), std::string::npos);
        RecordProperty("cleanup_nm", "old activation state4; Device.ActiveConnection=/; same NM/device fixture");
        RecordProperty("cleanup_nft", "owned table absent");
        RecordProperty("cleanup_bluez", helper_stopped ? "profile absent; pairing closed; RFCOMM EOF" :
            "helper-global profile retained; pairing closed; RFCOMM EOF");
    }
};

TEST_F(WirelessRecoveryProcess, R05FreshReceiverReloadsPersistentApproval) {
    // Given an approved composed admission, owned by the old receiver client.
    ASSERT_NO_FATAL_FAILURE(setup_admission());
    // When the receiver objects and their bus client are destroyed.
    timing.t0 = Clock::now();
    auto destruction = std::async(std::launch::async, [&] { if (!negative_control()) dispose_receiver(); });
    ASSERT_TRUE(eventually([&] { return !occupied(); }, 1s));
    timing.admission_closed = timing.elapsed(); EXPECT_LE(*timing.admission_closed, 1000);
    destruction.get(); timing.link_retired = timing.elapsed(); EXPECT_LE(*timing.link_retired, 5000);
    // Then actual receiver-loss cleanup precedes fresh objects and store reload.
    ASSERT_NO_FATAL_FAILURE(cleanup_observed(false));
    EXPECT_FALSE(occupied());
    store = std::make_unique<trust::ApprovedPhoneStore>(directory / "phones");
    ASSERT_TRUE(store->load()); ASSERT_EQ(store->phones().size(), 1U);
    ASSERT_EQ(read_text(directory / "phones"), approvals);
    auto connection = BusClient::private_bus(address, *store); ASSERT_TRUE(connection);
    client = std::move(connection.value());
    ASSERT_NO_FATAL_FAILURE(fresh_receiver());
    ASSERT_NO_FATAL_FAILURE(recover_process());
    EXPECT_GT(timing.new_activation, timing.old_activation);
    EXPECT_FALSE(control->pairing_window_open()); EXPECT_EQ(events.count_opened(), 0U);
}

TEST_F(WirelessRecoveryProcess, R06GracefulHelperRespawnReacquiresWithoutPairing) {
    // Given a receiver-owned approval and a live helper activation.
    ASSERT_NO_FATAL_FAILURE(setup_admission());
    // When the actual helper receives SIGTERM, without replacing bus or doubles.
    timing.t0 = Clock::now();
    if (!negative_control()) { ASSERT_EQ(::kill(daemon.pid, SIGTERM), 0); }
    // Then admission fails closed without grace and graceful cleanup is observed.
    ASSERT_NO_FATAL_FAILURE(closed()); EXPECT_LE(*timing.link_retired, 5000);
    ASSERT_NO_FATAL_FAILURE(cleanup_observed(true));
    ASSERT_TRUE(eventually([&] { return ::waitpid(daemon.pid, nullptr, WNOHANG) == daemon.pid; }));
    daemon.pid = 0;
    daemon.start({std::string{AA_HELPER_BUILD} + "/aa-system-helper", "--private-bus", address,
        directory.string(), "--combined", (directory / "commands").string()});
    ASSERT_TRUE(eventually([&] { return client->status(Action::ap_status).has_value(); }));
    ASSERT_NO_FATAL_FAILURE(recover_process());
    EXPECT_GT(timing.new_activation, timing.old_activation);
    EXPECT_EQ(store->phones().size(), 1U); EXPECT_EQ(events.count_opened(), 0U);
    EXPECT_FALSE(control->pairing_window_open());
}
