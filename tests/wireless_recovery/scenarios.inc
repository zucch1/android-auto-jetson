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
    RecordProperty("cleanup_observations_applicability", "old-AP shutdown not claimed; existing activation may be reused");
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
    RecordProperty("cleanup_observations_applicability", "old-AP shutdown not claimed; BlueZ owner-loss fail-closed authority only");
}
TEST_F(WirelessRecovery, R03NmTeardownClosesAdmissionAndCleansOwnedRules) {
    // Given completed admission and owned AP resources.
    ASSERT_NO_FATAL_FAILURE(setup_admission()); ASSERT_TRUE(boundary->table);
    // When teardown acknowledgement precedes asynchronous NM completion.
    nm->deactivate_delay_ms = 2000;
    timing.t0 = Clock::now();
    if (!negative_control()) { ASSERT_TRUE(client->call({Operation::teardown_access_point, 0}, core::Milliseconds{1000}, {})); }
    ASSERT_NO_FATAL_FAILURE(closed());
    const auto acknowledged = nm_identity.activation(address); ASSERT_TRUE(acknowledged);
    EXPECT_EQ(acknowledged->state, 3U); EXPECT_EQ(acknowledged->connection, active);
    EXPECT_FALSE(nm_identity.inactive(address));
    RecordProperty("deactivation_ack", "DeactivateConnection acknowledged; State3 and ActiveConnection retained; not AP-off");
    // Then only State4 plus interface detachment counts as old-AP inactivity.
    ASSERT_TRUE(eventually([&] { pump(); return inactive_observed(); }, 5s));
    timing.ap_inactive = timing.elapsed(); EXPECT_LE(*timing.ap_inactive, 5000);
    timing.terminal_recovery = timing.elapsed();
    EXPECT_EQ(nm->deactivations, 1); nm->deactivate_delay_ms = 0;
    record_cleanup_observations("old activation State4; Device.ActiveConnection=/; original NM/device incarnation",
        "owned table absent");
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
    record_cleanup_observations("old activation State4; Device.ActiveConnection=/; original NM/device incarnation",
        "old owned table removed; fresh activation owns current table");
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
    RecordProperty("cleanup_observations_applicability", "not applicable: synthetic peer rejected before admission");
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
    // And after one approved reconnect plus fresh acquisition, handoff is bound
    // to the acquiring source with no second reconnect resetting the lease:
    // a zero-confirmation source is rejected, the acquiring source admits.
    const auto source = acquire_recovery_ap(); ASSERT_FALSE(HasFailure());
    ASSERT_NO_FATAL_FAILURE(rejects_unbound_handoff());
    ASSERT_NO_FATAL_FAILURE(admit(source, false));
    EXPECT_FALSE(control->pairing_window_open());
}
