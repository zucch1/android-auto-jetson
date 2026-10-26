// SPDX-License-Identifier: GPL-3.0-or-later
class WirelessRecoveryKill : public WirelessRecoveryProcess {
protected:
    void hard_kill(bool hold_activation, int client_delay = 0) {
        ASSERT_NO_FATAL_FAILURE(setup_admission());
        // NM's client-bound shutdown may be unavailable. Its absence is never AP-off.
        nm->fail_stop = hold_activation; nm->client_stop_delay_ms = client_delay;
        timing.t0 = Clock::now();
        if (!negative_control()) { ASSERT_EQ(::kill(daemon.pid, SIGKILL), 0); }
        ASSERT_NO_FATAL_FAILURE(closed()); EXPECT_LE(*timing.link_retired, 5000);
        ASSERT_EQ(::waitpid(daemon.pid, nullptr, 0), daemon.pid); daemon.pid = 0;
        if (client_delay) {
            const auto acknowledged = nm_identity.activation(address); ASSERT_TRUE(acknowledged);
            EXPECT_EQ(acknowledged->state, 3U); EXPECT_EQ(acknowledged->connection, active);
            EXPECT_FALSE(nm_identity.inactive(address));
            RecordProperty("client_bound_shutdown_ack", "client loss scheduled State3 deactivation; interface retained; not AP-off");
        }
        const bool off = eventually([&] { return nm_identity.inactive(address); }, 3s);
        EXPECT_EQ(off, !hold_activation);
        EXPECT_EQ(nm->starts, timing.old_activation);
        EXPECT_TRUE(boundary->table);
        RecordProperty("cleanup_nm", off ?
            "old activation State4; Device.ActiveConnection=/; original NM/device incarnation" :
            "unknown: client-bound shutdown did not complete; cleanup-unverified");
        RecordProperty("cleanup_nft", "owned table retained; not proof of quarantine or AP-off");
        RecordProperty("cleanup_bluez", "profile record retained in double; pairing closed; old RFCOMM EOF");
        EXPECT_EQ(bluez.command("wire-read"), "closed");
        if (off) {
            timing.ap_inactive = timing.elapsed(); EXPECT_LE(*timing.ap_inactive, 5000);
            RecordProperty("hard_kill_cleanup", "pass: independently observed State4 and Device.ActiveConnection=/ in original NM/device incarnation");
        } else {
            RecordProperty("hard_kill_cleanup", "BLOCKED: cleanup-unverified / operator-supervised recovery required");
            RecordProperty("no_ap_leak_verified", "false");
        }
        daemon.start({std::string{AA_HELPER_BUILD} + "/aa-system-helper", "--private-bus", address,
            directory.string(), "--combined", (directory / "commands").string()});
        ASSERT_TRUE(eventually([&] { return client->status(Action::ap_status).has_value(); }));
        if (off) {
            ASSERT_NO_FATAL_FAILURE(recover_process());
            RecordProperty("protocol_result", "pass: fresh admission after helper respawn");
        } else {
            // Respawn's initial inactive status is not old activation cleanup evidence.
            ASSERT_NO_FATAL_FAILURE(stays_retired());
            timing.terminal_recovery = timing.elapsed(); EXPECT_LE(*timing.terminal_recovery, 15000);
            RecordProperty("protocol_result", "pass: fail-closed after helper respawn; no reacquisition attempted with unverified old AP");
        }
        nm->fail_stop = false; nm->client_stop_delay_ms = 0;
    }
};
TEST_F(WirelessRecoveryKill, R07ClientBoundShutdownHasIndependentNmProof) {
    // Given composed admission; when actual SIGKILL/respawn occurs; then fresh admission and old AP proof are separate.
    ASSERT_NO_FATAL_FAILURE(hard_kill(false, 2000));
}
TEST_F(WirelessRecoveryKill, R07UnverifiedShutdownIsExplicitlyBlockedNotPassed) {
    // Given NM cannot complete client-bound shutdown; when SIGKILL/respawn occurs; then only protocol fail-closed passes.
    ASSERT_NO_FATAL_FAILURE(hard_kill(true));
}
