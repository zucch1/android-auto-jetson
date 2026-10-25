// SPDX-License-Identifier: GPL-3.0-or-later
TEST_F(WirelessRecoveryConsumer, R09NewOwnerGenerationRetiresOldAdmission) {
    // Given actual private-bus owner, live receiver and completed RFCOMM admission.
    ASSERT_NO_FATAL_FAILURE(setup_admission());
    const auto old_consumer = consumer;
    // Keep an actual async refresh in flight at replacement; never inject a fabricated source.
    nm->delay_address_ms = 100;
    const auto reads = nm->address_reads.load();
    auto stale = std::async(std::launch::async, [&] {
        return client->get_ap_credentials({old_consumer.value, 0}, old_handle);
    });
    ASSERT_TRUE(eventually([&] { return nm->address_reads > reads; }, 1s));
    // When the real owner connection disappears and a new connection owns its name.
    timing.t0 = Clock::now();
    if (!negative_control()) { ASSERT_NO_FATAL_FAILURE(lose_owner()); }
    ASSERT_NO_FATAL_FAILURE(closed());
    (void)stale.get(); nm->delay_address_ms = 0;
    ASSERT_TRUE(eventually([&] { pump(); return inactive_observed(); }, 5s));
    timing.ap_inactive = timing.elapsed(); EXPECT_LE(*timing.ap_inactive, 5000);
    ASSERT_NO_FATAL_FAILURE(own_name());
    const auto registered = control->register_consumer(caller, name); ASSERT_TRUE(registered);
    consumer = registered.value(); EXPECT_NE(consumer, old_consumer);
    ASSERT_TRUE(control->start_projection(caller));
    EXPECT_FALSE(receiver->begin(*control, {old_consumer.value, 0}));
    ASSERT_NO_FATAL_FAILURE(stays_retired());
    // Then only the new live generation owns fresh AP and handoff credentials.
    ASSERT_NO_FATAL_FAILURE(recover_ap());
    const auto current = observe_snapshot(); ASSERT_TRUE(current);
    EXPECT_FALSE(client->get_ap_credentials({old_consumer.value, 0}, current->handle));
    EXPECT_FALSE(control->pairing_window_open()); EXPECT_EQ(events.count_opened(), 0U);
    record_cleanup_observations("old activation State4; Device.ActiveConnection=/; original NM/device incarnation",
        "old owned table removed; fresh activation owns current table");
}
