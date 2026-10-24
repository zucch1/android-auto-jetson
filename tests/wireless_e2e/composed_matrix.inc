// SPDX-License-Identifier: GPL-3.0-or-later
class WirelessD6Admission : public NmDaemon, public ::testing::WithParamInterface<int> {};
TEST_P(WirelessD6Admission, MissingChainLinkNeverReleasesCredentialsOrChangesTrust) {
    start(); acquire(); ASSERT_TRUE(state(State::active));
    const trust::TransportIdentity identity{trust::WirelessIdentity{"02:00:00:00:00:01", "02:00:00:00:00:AA"}};
    const auto scenario = GetParam();
    if (scenario != 0) { ASSERT_TRUE(store->approve(identity)); }
    if (scenario != 1 && scenario != 2) { ASSERT_EQ(bluez.command("paired"), "ok"); }
    const auto count = store->phones().size();
    if (scenario != 2 && scenario != 3) {
        const auto incoming = bluez.command("NewConnection");
        if (scenario == 1) { EXPECT_EQ(incoming, "rejected"); }
        else { ASSERT_EQ(incoming, "allowed"); }
    }
    switch (scenario) {
    case 5: nm->active_now = false; nm->pending = false; break;
    case 6: nm->active_now = false; nm->pending = true; break;
    case 7: nm->fail_stop = true; (void)client->forgotten_v1({core::PhoneId{1},
        trust::WirelessIdentity{"02:00:00:00:00:01", "02:00:00:00:00:AA"}}, {}); break;
    case 8: nm->bad_mac = true; break;
    case 9: nm->replaced = true; break;
    case 10: boundary->wrong_channel = true; break;
    default: break;
    }
    bool listened{};
    app::HandoffEligibilityCoordinator coordinator{*store, app::handoff_operations(*client),
        {[&](const ApSnapshot&) -> core::Result<void> { listened = true; return {}; }, [] {}}};
    if (scenario != 4) coordinator.consumer(core::ConsumerId{1});
    coordinator.begin({1, 1});
    const auto until = Clock::now() + 800ms;
    while (Clock::now() < until) {
        coordinator.tick(); EXPECT_FALSE(coordinator.admitting()); std::this_thread::sleep_for(10ms);
    }
    EXPECT_FALSE(listened); EXPECT_EQ(store->phones().size(), count);
    if (scenario == 0 || scenario >= 4) {
        const auto wire = bluez.command("wire-read");
        EXPECT_TRUE(wire == "open" || wire == "closed");
    }
}
INSTANTIATE_TEST_SUITE_P(Chain, WirelessD6Admission, ::testing::Range(0, 11));

class WirelessD6Bootstrap : public CompletedLifecycle, public ::testing::WithParamInterface<const char*> {};
TEST_P(WirelessD6Bootstrap, InvalidRecordBeforeInfoCannotReleaseCredentials) {
    const auto handle = accept();
    ASSERT_TRUE(client->get_ap_credentials({1, 1}, handle));
    ASSERT_TRUE(client->authorize_bootstrap({1, 1}, handle, true, true));
    ASSERT_TRUE(eventually([&] { return bluez.command("wire-read") != "open"; }));
    ASSERT_EQ(bluez.command(GetParam()), "ok");
    std::string wire;
    ASSERT_TRUE(eventually([&] { wire = bluez.command("wire-read"); return wire != "open"; }));
    EXPECT_EQ(wire, "closed");
    retired(handle);
    EXPECT_TRUE(store->phones().empty());
}
INSTANTIATE_TEST_SUITE_P(Wire, WirelessD6Bootstrap, ::testing::Values(
    "wire-send 00000004", "wire-send 000200071800", "wire-send 10010002", "wire-send 0001000280", "wire-eof"));

TEST_F(CompletedLifecycle, WirelessD6FragmentedInfoAndCoalescedCompletion) {
    const auto handle = accept();
    ASSERT_TRUE(client->get_ap_credentials({1, 1}, handle));
    ASSERT_TRUE(client->authorize_bootstrap({1, 1}, handle, true, true));
    ASSERT_TRUE(eventually([&] { return bluez.command("wire-read") != "open"; }));
    ASSERT_EQ(bluez.command("wire-send 0000"), "ok");
    EXPECT_EQ(bluez.command("wire-read"), "open");
    ASSERT_EQ(bluez.command("wire-send 0002"), "ok");
    AawDecoder decoder; AawDecodeResult decoded;
    ASSERT_TRUE(eventually([&] {
        decoder.feed(unhex(bluez.command("wire-read"))); decoded = decoder.next();
        return !std::holds_alternative<std::monostate>(decoded);
    }));
    ASSERT_TRUE(std::holds_alternative<AawRecord>(decoded)); EXPECT_EQ(std::get<AawRecord>(decoded).id, 3);
    ASSERT_EQ(bluez.command("wire-send 000200071800000200060800"), "ok");
    ASSERT_TRUE(eventually([&] { return bluez.command("wire-read") == "closed"; }));
    EXPECT_TRUE(client->authorize_bootstrap({1, 1}, handle, true, false));
}
