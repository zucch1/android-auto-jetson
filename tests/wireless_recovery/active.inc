// SPDX-License-Identifier: GPL-3.0-or-later
struct RecoverySink final : protocol::MessageSink {
    unsigned messages{};
    core::Result<void> on_message(const protocol::MessageView&) override { ++messages; return {}; }
};
class WirelessRecoveryActive : public WirelessRecoveryConsumer {
protected:
    RecoverySink video, audio, input;
    std::atomic<bool> active{}, finished{};
    bool grace_at_nine{};
    void owner_work() {
        if (timing.t0 == Clock::time_point{}) {
            active = receiver->active();
            if (!finished) return;
            ASSERT_TRUE(active); ASSERT_TRUE(receiver->active());
            timing.t0 = Clock::now();
            if (!negative_control()) { EXPECT_TRUE(g_dbus_connection_close_sync(owner, nullptr, nullptr)); }
            timing.admission_closed = occupied() ? std::optional<long long>{} : std::optional{timing.elapsed()};
        }
        if (timing.elapsed() >= 9000 && timing.elapsed() < 9500) {
            EXPECT_TRUE(receiver->active()); grace_at_nine = receiver->active();
            EXPECT_FALSE(cleanup_monitor->first.load()); EXPECT_TRUE(nm->active_now);
        }
    }
    app::WirelessLink::Config link_config() override {
        app::WirelessLink::Config config;
        config.services = {
            {protocol::ServiceKey{99}, protocol::ChannelRole::video, protocol::VideoConfiguration{{{800, 480, 30}}}},
            {protocol::ServiceKey{100}, protocol::ChannelRole::media_audio, protocol::AudioConfiguration{{{48000, 16, 2}}}},
            {protocol::ServiceKey{104}, protocol::ChannelRole::input, protocol::ButtonConfiguration{{66}}}};
        config.sinks = {{protocol::ChannelRole::video, &video}, {protocol::ChannelRole::media_audio, &audio},
            {protocol::ChannelRole::input, &input}};
        return config;
    }
};
TEST_F(WirelessRecoveryActive, R08ConfirmedActiveOwnerLossGraceAndActualApTeardown) {
    // Given real RFCOMM bootstrap followed by TCP/AAP encapsulated TLS and confirmed ACTIVE.
    ASSERT_NO_FATAL_FAILURE(setup_admission());
    receiver->attest_for_test([&](const transport::TcpTransport&, const trust::PhoneIdentity& phone) {
        return phone.key == trust::identity_key(identity);
    });
    auto tcp = transport::TcpTransport::connect(wireless_test::host_endpoint().c_str(), 5277);
    ASSERT_TRUE(tcp); tcp.value()->set_io_deadline(Clock::now() + 5s);
    std::atomic<bool> release{};
    auto phone = std::async(std::launch::async, [&] {
        wireless_test::walk_phone(*tcp.value(), [&] { return active.load(); });
        finished = true;
        const auto until = Clock::now() + 20s;
        while (!release && Clock::now() < until) std::this_thread::sleep_for(10ms);
        tcp.value()->close();
    });
    bool input_sent{};
    const bool established = eventually([&] {
        (void)receiver->poll(*control, [this] { owner_work(); });
        if (active && audio.messages && !input_sent) {
            EXPECT_TRUE(receiver->session_for_test()->submit_input(ipc::InputEvent{ipc::KeyEvent{66, true}}));
            input_sent = true;
        }
        return HasFailure() || (finished.load() && timing.t0 != Clock::time_point{});
    }, 10s);
    if (!established || HasFailure()) { release = true; phone.get(); FAIL() << "TLS ACTIVE setup failed"; }
    // Owner loss and grace observation run inside blocking I/O, as in composed TLS.
    ASSERT_TRUE(active); ASSERT_NE(timing.t0, Clock::time_point{});
    while (timing.elapsed() < 15000) {
        if (!receiver->active()) {
            timing.grace_expiry_observed = timing.elapsed(); timing.link_retired = timing.elapsed(); break;
        }
        (void)receiver->poll(*control, [this] { owner_work(); });
    }
    release = true; phone.get();
    // Then session grace and the actual helper cleanup request both meet [10s,11s).
    EXPECT_TRUE(grace_at_nine); ASSERT_TRUE(timing.admission_closed);
    EXPECT_LE(*timing.admission_closed, 1000); ASSERT_TRUE(timing.grace_expiry_observed);
    EXPECT_GE(*timing.grace_expiry_observed, 10000); EXPECT_LT(*timing.grace_expiry_observed, 11000);
    ASSERT_TRUE(cleanup_monitor->first.load());
    timing.teardown_started = std::chrono::duration_cast<std::chrono::milliseconds>(
        Clock::time_point{Clock::duration{cleanup_monitor->first.load()}} - timing.t0).count();
    EXPECT_GE(*timing.teardown_started, 10000); EXPECT_LT(*timing.teardown_started, 11000);
    const auto cleanup_deadline = std::min(*timing.teardown_started + 5000, 15000LL);
    ASSERT_TRUE(eventually([&] { return nm_identity.inactive(address) && !boundary->table; },
        std::chrono::milliseconds{std::max(0LL, cleanup_deadline - timing.elapsed())}));
    timing.ap_inactive = timing.elapsed(); EXPECT_LE(*timing.ap_inactive, cleanup_deadline);
    timing.terminal_recovery = timing.elapsed(); EXPECT_LE(*timing.terminal_recovery, 15000);
    EXPECT_EQ(video.messages, 1U); EXPECT_EQ(audio.messages, 1U);
    EXPECT_EQ(control->state().consumer.value, 0U); EXPECT_FALSE(occupied());
    EXPECT_EQ(nm->starts, timing.old_activation);
    RecordProperty("terminal_state", "confirmed ACTIVE grace expired; old AP independently inactive; fail-closed");
    RecordProperty("fresh_acquisition_applicability", "not applicable: consumer absent, no reacquisition authorized");
}
TEST_F(WirelessRecoveryActive, R11ConnectedLossFullProjectionReconnect) {
    // Given completed composed admission for the approved phone.
    ASSERT_NO_FATAL_FAILURE(setup_admission());
    receiver->attest_for_test([&](const transport::TcpTransport&, const trust::PhoneIdentity& phone) {
        return phone.key == trust::identity_key(identity);
    });
    // When the BlueZ link drops and returns (R01-style loss).
    timing.t0 = Clock::now(); ASSERT_EQ(bluez.command(negative_control() ? "connected-true" : "connected-false"), "ok");
    ASSERT_NO_FATAL_FAILURE(closed());
    ASSERT_EQ(bluez.command("connected-true"), "ok"); ASSERT_NO_FATAL_FAILURE(stays_retired());
    // Then explicit reconnect, fresh RFCOMM bootstrap, TCP and encapsulated TLS reach confirmed ACTIVE.
    ASSERT_NO_FATAL_FAILURE(recover_ap());
    auto tcp = transport::TcpTransport::connect(wireless_test::host_endpoint().c_str(), 5277);
    ASSERT_TRUE(tcp); tcp.value()->set_io_deadline(Clock::now() + 5s);
    auto phone = std::async(std::launch::async, [&] {
        wireless_test::walk_phone(*tcp.value(), [&] { return active.load(); });
        finished = true;
        tcp.value()->close();
    });
    bool input_sent{}, projection_active{};
    const bool established = eventually([&] {
        (void)receiver->poll(*control, [this] { active = receiver->active(); });
        if (active && audio.messages && !input_sent) {
            EXPECT_TRUE(receiver->session_for_test()->submit_input(ipc::InputEvent{ipc::KeyEvent{66, true}}));
            input_sent = true;
        }
        projection_active = projection_active || (active && video.messages && audio.messages);
        return HasFailure() || finished.load();
    }, 15s);
    timing.terminal_recovery = timing.elapsed();
    phone.get(); ASSERT_TRUE(established); ASSERT_FALSE(HasFailure());
    EXPECT_TRUE(projection_active); EXPECT_TRUE(input_sent);
    EXPECT_EQ(video.messages, 1U); EXPECT_EQ(audio.messages, 1U);
    EXPECT_LE(*timing.terminal_recovery, 15000);
    RecordProperty("terminal_state", "fresh bootstrap, TCP and encapsulated AAP TLS confirmed ACTIVE; video/audio/input delivered");
    RecordProperty("fresh_confirmed_active", projection_active ? "true" : "false");
    RecordProperty("video_delivered", video.messages == 1 ? "true" : "false");
    RecordProperty("audio_delivered", audio.messages == 1 ? "true" : "false");
    RecordProperty("input_delivered", input_sent ? "true" : "false");
    RecordProperty("attestation", "test-only; production unbound TCP admission remains rejected");
}
