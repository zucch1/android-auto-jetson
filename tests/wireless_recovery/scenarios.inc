// SPDX-License-Identifier: GPL-3.0-or-later
TEST_F(WirelessRecovery, R01ConnectedLossNeedsFreshHandoff) {
    // Given completed composed admission and persistent approval.
    ASSERT_NO_FATAL_FAILURE(setup_admission());
    // When the BlueZ link goes down (not the RFCOMM callback).
    timing.t0 = Clock::now(); ASSERT_EQ(bluez.command(negative_control() ? "connected-true" : "connected-false"), "ok");
    // Then admission retires, restoration alone cannot revive it, fresh reconnect can.
    ASSERT_NO_FATAL_FAILURE(closed());
    ASSERT_EQ(bluez.command("connected-true"), "ok"); ASSERT_NO_FATAL_FAILURE(stays_retired());
    ASSERT_NO_FATAL_FAILURE(recover_ap());
}
TEST_F(WirelessRecovery, R02BluezOwnerLossFailsClosed) {
    // Given completed admission.
    ASSERT_NO_FATAL_FAILURE(setup_admission());
    // When BlueZ releases its actual private-bus name.
    timing.t0 = Clock::now(); ASSERT_EQ(bluez.command(negative_control() ? "paired" : "release-name"), "ok");
    // Then restoring only the name does not revive authority.
    ASSERT_NO_FATAL_FAILURE(closed());
    ASSERT_EQ(bluez.command("acquire-name"), "ok"); ASSERT_NO_FATAL_FAILURE(stays_retired());
    timing.terminal_recovery = timing.elapsed(); EXPECT_LE(*timing.terminal_recovery, 15000);
}
TEST_F(WirelessRecovery, R03NmTeardownClosesAdmissionAndCleansOwnedRules) {
    // Given completed admission and owned AP resources.
    ASSERT_NO_FATAL_FAILURE(setup_admission()); ASSERT_TRUE(boundary->table);
    // When an actual helper AP teardown operation is requested.
    timing.t0 = Clock::now();
    if (!negative_control()) { ASSERT_TRUE(client->call({Operation::teardown_access_point, 0}, core::Milliseconds{1000}, {})); }
    // Then both admission and independently observed NM resources retire.
    ASSERT_NO_FATAL_FAILURE(closed());
    ASSERT_TRUE(eventually([&] { pump(); return inactive_observed(); }, 5s));
    timing.ap_inactive = timing.elapsed(); EXPECT_LE(*timing.ap_inactive, 5000);
    timing.terminal_recovery = timing.elapsed();
}
TEST_F(WirelessRecovery, R04FreshApAcquisitionCannotReuseOldSnapshot) {
    // Given a pinned old activation.
    ASSERT_NO_FATAL_FAILURE(setup_admission());
    // When actual AP release invalidates the snapshot.
    timing.t0 = Clock::now();
    ASSERT_TRUE(client->call({Operation::teardown_access_point, 0}, core::Milliseconds{1000}, {}));
    ASSERT_NO_FATAL_FAILURE(closed());
    ASSERT_TRUE(eventually([&] { return inactive_observed(); }));
    timing.ap_inactive = timing.elapsed();
    // Then a fresh AP-only grant and bootstrap recover admission, never the old handle.
    ASSERT_NO_FATAL_FAILURE(recover_ap());
    EXPECT_GT(timing.new_activation, timing.old_activation);
    EXPECT_FALSE(client->get_ap_credentials({consumer.value, 0}, old_handle));
}
TEST_F(WirelessRecovery, R10SyntheticUnapprovedUnbondedPeerDeniedBeforeCredentials) {
    // Given approved first-peer admission, with an unchanged persistent store.
    ASSERT_NO_FATAL_FAILURE(setup_admission());
    // When a synthetic second unbonded peer requests the AA profile.
    timing.t0 = Clock::now();
    ASSERT_EQ(bluez.command(negative_control() ? "paired" : "unpaired"), "ok");
    EXPECT_EQ(bluez.command(negative_control() ? "NewConnection" : "NewConnection wrong"), "rejected");
    // Then no credential/session is created for it; this is not TCP identity proof.
    EXPECT_EQ(bluez.command("wire-read"), "closed");
    EXPECT_FALSE(receiver->active());
    timing.terminal_recovery = timing.elapsed(); EXPECT_LE(*timing.terminal_recovery, 1000);
}
TEST_F(WirelessRecovery, ApOnlyAcquisitionUsesFreshLiveConsumerWithoutPairing) {
    // Given the actual registered consumer, then its actual authorized unregister.
    ASSERT_NO_FATAL_FAILURE(setup_admission());
    ASSERT_TRUE(control->unregister_consumer(caller));
    EXPECT_FALSE(acquire_only({consumer.value, ++request}));
    ASSERT_TRUE(eventually([&] { return !nm->active_now && !nm->pending && !boundary->table; }));
    ASSERT_EQ(bluez.command("state").find("\"Pairable\": false") != std::string::npos, true);
    auto registered = control->register_consumer(caller, name); ASSERT_TRUE(registered); consumer = registered.value();
    // When an approved recovery needs AP-only acquisition with a fresh request.
    timing.t0 = Clock::now();
    EXPECT_FALSE(acquire_only({consumer.value + 1, ++request}));
    ASSERT_TRUE(acquire_only({consumer.value, ++request}));
    // Then pairing remains closed and spent source replay is rejected.
    ASSERT_TRUE(state(State::active));
    EXPECT_NE(bluez.command("state").find("\"Pairable\": false"), std::string::npos);
    EXPECT_FALSE(acquire_only({consumer.value, request}));
    EXPECT_FALSE(control->pairing_window_open());
}
