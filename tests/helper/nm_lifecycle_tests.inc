// SPDX-License-Identifier: GPL-3.0-or-later
// Uses the private composed NM fixture in nm_test.cpp; no production attestation.
class CompletedLifecycle : public NmDaemon {
protected:
    Clock::time_point began{Clock::now()};
    std::atomic<std::int64_t> elapsed{};
    std::unique_ptr<BluezBackend> bluetooth;
    std::unique_ptr<Backend> ap;
    std::unique_ptr<BackendRouter> router;
    std::optional<BusService> service;
    std::jthread pump;
    HandoffFailure failure{};
    Source source{1, 1};
    void SetUp() override {
        NmDaemon::SetUp();
        bluetooth = std::make_unique<BluezBackend>(address, [this] { return began + std::chrono::milliseconds{elapsed.load()}; });
        ap = std::make_unique<Backend>(Policy{"wlan-test", "US", 5277, (directory / "state").string(), ::getuid()}, address,
            private_commands((directory / "commands").string()));
        router = std::make_unique<BackendRouter>(BackendRouter::Routes{*ap, *bluetooth});
        const auto digest = read_text(directory / "receiver.sha256");
        auto created = BusService::private_bus(address, {::getuid(), digest.substr(0, 64)}, *router); ASSERT_TRUE(created);
        service.emplace(std::move(created.value()));
        pump = std::jthread{[this](const std::stop_token& stop) { while (!stop.stop_requested()) if (!service->cycle()) return; }};
        auto connection = BusClient::private_bus(address, *store); ASSERT_TRUE(connection); client = std::move(connection.value());
        acquire(); ASSERT_TRUE(state(State::active)); ASSERT_EQ(bluez.command("paired"), "ok");
    }
    void TearDown() override {
        pump.request_stop(); pump.join(); client.reset(); service.reset(); router.reset(); bluetooth.reset(); ap.reset();
        NmDaemon::TearDown();
    }
    std::uint64_t accept() {
        EXPECT_EQ(bluez.command("NewConnection"), "allowed");
        const auto peer = client->poll_bootstrap(source, failure);
        EXPECT_TRUE(peer); return peer ? peer.value().handle : 0;
    }
    void begin(std::uint64_t handle) {
        ASSERT_TRUE(client->get_ap_credentials(source, handle));
        ASSERT_TRUE(client->authorize_bootstrap(source, handle, true, true));
        EXPECT_TRUE(eventually([&] { return bluez.command("wire-read") != "open"; }));
        ASSERT_EQ(bluez.command("wire-send 00000002"), "ok");
        EXPECT_TRUE(eventually([&] { return bluez.command("wire-read") != "open"; }));
    }
    void complete(std::uint64_t handle) {
        begin(handle); ASSERT_EQ(bluez.command("wire-send 000200071800000200060800"), "ok");
        ASSERT_EQ(bluez.command("wire-read"), "closed");
    }
    void retired(std::uint64_t handle) {
        EXPECT_FALSE(client->poll_bootstrap(source, failure));
        EXPECT_FALSE(client->get_ap_credentials(source, handle));
        EXPECT_FALSE(client->authorize_bootstrap(source, handle, true, true));
    }
};
TEST_F(CompletedLifecycle, CompletedOutlivesBothDeadlinesWhileAdmissionCloses) {
    // Given a fully validated exchange and its original snapshot.
    const auto handle = accept(); const auto original = client->get_ap_credentials({1, 1}, handle); ASSERT_TRUE(original);
    complete(handle);
    // When both old lifetime limits have elapsed.
    elapsed = 61000;
    // Then authority persists, but another RFCOMM socket is not admitted.
    auto current = client->get_ap_credentials({1, 1}, handle); ASSERT_TRUE(current);
    EXPECT_TRUE(current.value() == original.value());
    const auto peer = client->poll_bootstrap({1, 1}, failure); ASSERT_TRUE(peer); EXPECT_EQ(peer.value().handle, handle);
    EXPECT_EQ(bluez.command("NewConnection"), "rejected");
    EXPECT_TRUE(client->authorize_bootstrap({1, 1}, handle, true, true));
    EXPECT_EQ(bluez.command("wire-read"), "closed");
    EXPECT_TRUE(client->authorize_bootstrap({1, 1}, handle, false, false)); retired(handle);
}
TEST_F(CompletedLifecycle, RepeatedAllowDoesNotRestartPendingOrCompletedExchange) {
    // Given an authorized exchange waiting for statuses.
    const auto handle = accept(); begin(handle);
    // When allow is repeated before and after completion.
    EXPECT_TRUE(client->authorize_bootstrap({1, 1}, handle, true, true));
    ASSERT_EQ(bluez.command("wire-send 000200071800000200060800"), "ok");
    EXPECT_EQ(bluez.command("wire-read"), "closed");
    EXPECT_TRUE(client->authorize_bootstrap({1, 1}, handle, true, true));
    // Then no new endpoint is emitted and deny can retire completed authority.
    EXPECT_EQ(bluez.command("wire-read"), "closed");
    EXPECT_TRUE(client->authorize_bootstrap({1, 1}, handle, false, false)); retired(handle);
}
class CompletedLoss : public CompletedLifecycle, public ::testing::WithParamInterface<const char*> {};
TEST_P(CompletedLoss, ObservedLossPermanentlyRetiresExactHandle) {
    // Given completed authority.
    const auto handle = accept(); complete(handle);
    ASSERT_TRUE(client->get_ap_credentials({1, 1}, handle));
    // When the captured peer loses continuity.
    ASSERT_NE(bluez.command(GetParam()), "rejected"); retired(handle);
    // Then restoring identical inventory/owner cannot revive it.
    ASSERT_EQ(bluez.command("acquire-name"), "ok"); ASSERT_EQ(bluez.command("restore"), "ok");
    ASSERT_EQ(bluez.command("paired"), "ok"); retired(handle);
}
INSTANTIATE_TEST_SUITE_P(Terminal, CompletedLoss, ::testing::Values(
    // v2.6 supersedes channel-teardown revocation with genuine Connected loss.
    "connected-false", "release-name", "remove", "unpaired", "owner-during-inventory", "recreate", "Release",
    "adapter-recreate", "bond-cycle"));
TEST_F(CompletedLifecycle, CompletedChannelTeardownPreservesPinnedAuthority) {
    // Given a successful real RFCOMM exchange and its pinned activation snapshot.
    const auto handle = accept(); const auto original = client->get_ap_credentials({1, 1}, handle); ASSERT_TRUE(original);
    complete(handle);
    // When BlueZ reports the helper's normal post-completion channel teardown.
    ASSERT_EQ(bluez.command("RequestDisconnection"), "allowed");
    // Then the exact handle, snapshot and completed allow/no-listener no-op survive.
    const auto peer = client->poll_bootstrap({1, 1}, failure); ASSERT_TRUE(peer);
    EXPECT_EQ(peer.value().handle, handle); EXPECT_EQ(failure, HandoffFailure::none);
    const auto current = client->get_ap_credentials({1, 1}, handle); ASSERT_TRUE(current);
    EXPECT_TRUE(current.value() == original.value());
    EXPECT_TRUE(client->authorize_bootstrap({1, 1}, handle, true, false));
    EXPECT_EQ(bluez.command("wire-read"), "closed");
}
TEST_F(CompletedLifecycle, CompletedConnectedLossRequiresFreshBootstrap) {
    // Given completed authority with the bond intact throughout both loss forms.
    auto handle = accept(); complete(handle);
    std::uint64_t generation{1};
    for (const auto* loss : {"connected-false", "connected-invalidated"}) {
        // When link continuity is lost and then reports connected again.
        ASSERT_EQ(bluez.command(loss), "ok"); retired(handle);
        ASSERT_EQ(bluez.command("connected-true"), "ok"); retired(handle);
        EXPECT_EQ(bluez.command("NewConnection"), "rejected");
        // Then only a fresh grant and accepted bootstrap can create new authority.
        client->cancel(core::ConsumerId{generation}, core::PairingRequestId{generation});
        ++generation; source = {generation, generation};
        acquire(generation); ASSERT_TRUE(state(State::active));
        const auto fresh = accept(); ASSERT_NE(fresh, handle);
        EXPECT_FALSE(client->get_ap_credentials(source, handle));
        handle = fresh; complete(handle);
    }
}
TEST_F(CompletedLifecycle, PendingChannelOrConnectedLossRetiresExchange) {
    std::uint64_t generation{1};
    for (const auto* loss : {"RequestDisconnection", "connected-false", "connected-invalidated"}) {
        // Given a real pending exchange that has already delivered credentials.
        const auto handle = accept(); begin(handle);
        // When channel teardown or genuine link loss occurs before completion.
        ASSERT_EQ(bluez.command(loss), std::string_view{loss} == "RequestDisconnection" ? "allowed" : "ok");
        // Then no authority or socket survives, and Connected=true cannot revive it.
        retired(handle); EXPECT_EQ(failure, HandoffFailure::eligibility);
        EXPECT_EQ(bluez.command("wire-read"), "closed");
        ASSERT_EQ(bluez.command("connected-true"), "ok"); retired(handle);
        client->cancel(core::ConsumerId{generation}, core::PairingRequestId{generation});
        ++generation; source = {generation, generation};
        acquire(generation); ASSERT_TRUE(state(State::active));
    }
}
class CompletedApLoss : public CompletedLifecycle, public ::testing::WithParamInterface<int> {};
TEST_P(CompletedApLoss, ChangedSnapshotCannotTransmitOrRevive) {
    // Given completed authority pinned to the original AP.
    const auto handle = accept(); complete(handle); ASSERT_TRUE(client->get_ap_credentials({1, 1}, handle));
    // When AP generation, endpoint, BSSID, channel or owner changes.
    switch (GetParam()) {
    case 0: ap->cleanup(); break;
    case 1: nm->changed_ip = true; break;
    case 2: nm->changed_mac = true; break;
    case 3: boundary->wrong_channel = true; break;
    case 4: nm->release_owner_at_address = nm->address_reads + 1; break;
    }
    EXPECT_FALSE(client->get_ap_credentials({1, 1}, handle));
    // Then restore never revives the retired handoff.
    nm->changed_ip = false; nm->changed_mac = false; boundary->wrong_channel = false; retired(handle);
}
TEST_P(CompletedApLoss, ChangedSnapshotRejectsCompletedConfirmation) {
    // Given completed authority pinned to the original AP.
    const auto handle = accept(); complete(handle); ASSERT_TRUE(client->get_ap_credentials({1, 1}, handle));
    ASSERT_TRUE(client->authorize_bootstrap({1, 1}, handle, true, false));
    // When the AP changes before the next completed confirmation.
    switch (GetParam()) {
    case 0: ap->cleanup(); break;
    case 1: nm->changed_ip = true; break;
    case 2: nm->changed_mac = true; break;
    case 3: boundary->wrong_channel = true; break;
    case 4: nm->release_owner_at_address = nm->address_reads + 1; break;
    }
    // Then confirmation is not renewing evidence and the handle stays retired after restore.
    EXPECT_FALSE(client->authorize_bootstrap({1, 1}, handle, true, false));
    nm->changed_ip = false; nm->changed_mac = false; boundary->wrong_channel = false; retired(handle);
}
INSTANTIATE_TEST_SUITE_P(Terminal, CompletedApLoss, ::testing::Range(0, 5));
class FailedReplacement : public CompletedLifecycle, public ::testing::WithParamInterface<const char*> {};
TEST_P(FailedReplacement, FailedFreshBootstrapCannotInheritCompletedPredecessor) {
    // Given completed A replaced by accepted, explicitly authorized B.
    const auto a = accept(); complete(a); const auto b = accept(); ASSERT_NE(a, b);
    EXPECT_FALSE(client->get_ap_credentials({1, 1}, a));
    EXPECT_FALSE(client->authorize_bootstrap({1, 1}, a, false, false));
    // When B fails (including success statuses with terminal partial trailing data).
    const std::string_view scenario{GetParam()};
    if (scenario == "write") {
        ASSERT_TRUE(client->get_ap_credentials({1, 1}, b));
        ASSERT_EQ(bluez.command("wire-shutdown-read"), "ok");
        ASSERT_TRUE(client->authorize_bootstrap({1, 1}, b, true, true));
    } else {
        begin(b);
        if (scenario == "receive-boundary") {
            const auto wire = "wire-send 0002000718000ffa0006080012f51f" + std::string(8170, '0') + "00";
            ASSERT_EQ(bluez.command(wire), "ok");
        } else if (scenario == "timeout") elapsed = 10000;
        else { ASSERT_EQ(bluez.command(GetParam()), "ok"); }
    }
    if (scenario != "receive-boundary") { EXPECT_EQ(bluez.command("wire-read"), "closed"); }
    ASSERT_TRUE(eventually([&] { return !client->poll_bootstrap({1, 1}, failure); }));
    if (scenario == "write") { EXPECT_EQ(failure, HandoffFailure::write); }
    if (scenario == "timeout") { EXPECT_EQ(failure, HandoffFailure::timeout); }
    if (scenario == "receive-boundary") { EXPECT_EQ(failure, HandoffFailure::truncated); }
    // Then neither handle is usable; no fallback to A.
    retired(b); EXPECT_FALSE(client->get_ap_credentials({1, 1}, a));
}
INSTANTIATE_TEST_SUITE_P(Terminal, FailedReplacement, ::testing::Values(
    "wire-eof", "wire-send 000200071801", "wire-send 0001000780", "wire-send 00020007180000020006080000",
    "write", "timeout", "receive-boundary"));
TEST_F(CompletedLifecycle, SourceTeardownRetiresCompletedButPreservesUnrelatedObservation) {
    // Given completed A and an unrelated pending observation bound to B.
    const auto handle = accept(); complete(handle); ASSERT_TRUE(client->get_ap_credentials({1, 1}, handle));
    ASSERT_TRUE(client->prepare(core::ConsumerId{2}, core::PairingRequestId{2}));
    // When A's exact source is invalidated.
    client->cancel(core::ConsumerId{1}, core::PairingRequestId{1}); retired(handle);
    // Then B remains usable for a fresh pairing grant/admission.
    EXPECT_TRUE(client->confirmed(core::ConsumerId{2}, core::PairingRequestId{2}));
}
TEST_F(CompletedLifecycle, ReceiverLossCannotRestoreCompletedHandle) {
    // Given completed authority owned by the current receiver connection.
    const auto handle = accept(); complete(handle); ASSERT_TRUE(client->get_ap_credentials({1, 1}, handle));
    // When that connection disappears and an identical executable reconnects.
    client.reset(); ASSERT_TRUE(eventually([&] { return !nm->active_now; }, 5000ms));
    auto connection = BusClient::private_bus(address, *store); ASSERT_TRUE(connection); client = std::move(connection.value());
    acquire(2); ASSERT_TRUE(state(State::active));
    // Then inventory, receiver executable and restored AP do not revive the handle.
    EXPECT_FALSE(client->get_ap_credentials({2, 2}, handle));
    EXPECT_FALSE(client->poll_bootstrap({2, 2}, failure));
}
TEST_F(CompletedLifecycle, PrecommitPreservesCompletedBut703RetiresIt) {
    // Given a completed link and the real store/control path.
    const auto handle = accept(); complete(handle);
    const trust::TransportIdentity identity{trust::WirelessIdentity{"02:00:00:00:00:01", "02:00:00:00:00:AA"}};
    const auto phone = store->approve(identity); ASSERT_TRUE(phone);
    trust::SteadyClock clock; trust::PairingPolicy phones{*store, clock};
    ipc::test::FakePeerCredentials peers; ipc::test::RecordingEventSink events; ipc::test::SequencePairingIds ids;
    ipc::ControlService control{{::getuid()}, {peers, events, ids, phones, client.get()}};
    const ipc::BusName caller{":1.900"}, name{"org.custom.Consumer"};
    peers.add_connection(caller, ::getuid()); peers.set_owner(name, caller); ASSERT_TRUE(control.register_consumer(caller, name));
    // When deletion fails before commit, the authority must remain usable.
    {
        const trust::test::faults::ScopedFault fault{trust::test::faults::Fault::renameat};
        const auto result = control.forget_phone(caller, phone.value()); ASSERT_FALSE(result);
        EXPECT_EQ(result.error().code(), ErrorCode::trust_store_io);
    }
    EXPECT_TRUE(client->get_ap_credentials({1, 1}, handle));
    // Then a committed-but-not-durable deletion (703) still retires authority.
    {
        const trust::test::faults::ScopedFault fault{trust::test::faults::Fault::dir_fsync};
        const auto result = control.forget_phone(caller, phone.value()); ASSERT_FALSE(result);
        EXPECT_EQ(result.error().code(), ErrorCode::trust_store_not_durable);
    }
    retired(handle); EXPECT_TRUE(store->phones().empty());
}
