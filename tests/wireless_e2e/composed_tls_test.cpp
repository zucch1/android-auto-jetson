// SPDX-License-Identifier: GPL-3.0-or-later
#include "secret_fixture.hpp"
#include "phone.hpp"
#include <aa/app/WirelessConfirmation.hpp>

namespace aa::helper::nm {
namespace {
class WirelessComposedTls : public SecretDaemon {};
TEST_F(WirelessComposedTls, ApprovedReconnectBootstrapTlsAndAuthorizedSinks) {
    start(); acquire(99); ASSERT_TRUE(state(State::active));
    const trust::TransportIdentity identity{trust::WirelessIdentity{"02:00:00:00:00:01", "02:00:00:00:00:AA"}};
    ASSERT_TRUE(store->approve(identity));
    ASSERT_EQ(bluez.command("paired"), "ok");
    session::SteadyClock clock;
    SecretDiagnostics diagnostic_sink;
    diagnostics::Diagnostics diagnostics{diagnostic_sink};
    wireless_test::RecordingSink video, audio, input;
    app::WirelessLink::Config config;
    config.services = {
        {protocol::ServiceKey{99}, protocol::ChannelRole::video, protocol::VideoConfiguration{{{800, 480, 30}}}},
        {protocol::ServiceKey{100}, protocol::ChannelRole::media_audio, protocol::AudioConfiguration{{{48000, 16, 2}}}},
        {protocol::ServiceKey{104}, protocol::ChannelRole::input, protocol::ButtonConfiguration{{66}}}};
    config.sinks = {{protocol::ChannelRole::video, &video}, {protocol::ChannelRole::media_audio, &audio},
                    {protocol::ChannelRole::input, &input}};
    app::WirelessLink link{{*store, clock, diagnostics}, std::move(config)};
    std::optional<ApSnapshot> snapshot;
    app::HandoffEligibilityCoordinator coordinator{*store, app::handoff_operations(*client),
        {[&](const ApSnapshot& ap) { snapshot = ap; return link.listen(ap); }, [&] { link.close_admission(); }}};
    app::WirelessConfirmation confirmation{*client,
        [&] { return link.session() && link.session()->state() == session::State::active; },
        [&](core::ConsumerId) { coordinator.consumer(std::nullopt); }};
    trust::SteadyClock trust_clock;
    trust::PairingPolicy phones{*store, trust_clock};
    ipc::test::FakePeerCredentials peers;
    ipc::test::RecordingEventSink events;
    ipc::test::SequencePairingIds ids;
    ipc::ControlService control{{::getuid()}, {peers, events, ids, phones, &confirmation}};
    const ipc::BusName caller{":1.900"}, name{"org.custom.Consumer"}, rogue{":1.901"};
    peers.add_connection(caller, ::getuid()); peers.set_owner(name, caller);
    peers.add_connection(rogue, ::getuid() + 1);
    EXPECT_FALSE(control.start_projection(caller));
    EXPECT_FALSE(control.register_consumer(rogue, name));
    const auto consumer = control.register_consumer(caller, name); ASSERT_TRUE(consumer);
    ASSERT_TRUE(control.start_projection(caller));
    ASSERT_TRUE(client->reconnect(identity));
    ASSERT_EQ(bluez.command("NewConnection"), "allowed");
    coordinator.consumer(consumer.value()); coordinator.begin({consumer.value().value, 0});
    ASSERT_TRUE(eventually([&] { coordinator.tick(); return coordinator.admitting(); }, 10s))
        << "epoch=" << coordinator.epoch() << " listener_started=" << snapshot.has_value();
    ASSERT_TRUE(snapshot); ASSERT_TRUE(link.listening());
    EXPECT_TRUE(snapshot->password == sentinel);
    { const std::lock_guard lock{nm->mutex}; EXPECT_TRUE(nm->seen_psk == sentinel); }
    aaw::WifiStartRequest endpoint;
    auto bootstrap = std::async(std::launch::async, [&] {
    const auto read_record = [&](AawRecord& record) {
        AawDecoder decoder;
        return eventually([&] {
            decoder.feed(unhex(bluez.command("wire-read")));
            auto decoded = decoder.next();
            if (const auto* result = std::get_if<AawRecord>(&decoded)) { record = *result; return true; }
            return false;
        });
    };
    AawRecord endpoint_record;
    ASSERT_TRUE(read_record(endpoint_record)); ASSERT_EQ(endpoint_record.id, 1);
    ASSERT_TRUE(endpoint.ParseFromArray(endpoint_record.payload.data(), static_cast<int>(endpoint_record.payload.size())));
    EXPECT_EQ(endpoint.ip_address(), snapshot->ip); EXPECT_EQ(endpoint.port(), snapshot->port);
    ASSERT_EQ(bluez.command("wire-send 00000002"), "ok");
    AawRecord info_record;
    ASSERT_TRUE(read_record(info_record)); ASSERT_EQ(info_record.id, 3);
    aaw::WifiInfoResponse info;
    ASSERT_TRUE(info.ParseFromArray(info_record.payload.data(), static_cast<int>(info_record.payload.size())));
    EXPECT_TRUE(info.password() == snapshot->password); EXPECT_TRUE(info.ssid() == snapshot->ssid);
    EXPECT_EQ(info.bssid(), snapshot->bssid); EXPECT_EQ(info.security_mode(), 8);
    ASSERT_EQ(bluez.command("wire-send 000200071800000200060800"), "ok");
    ASSERT_TRUE(eventually([&] { return bluez.command("wire-read") == "closed"; }));
    });
    ASSERT_TRUE(eventually([&] { coordinator.tick(); return bootstrap.wait_for(0ms) == std::future_status::ready; }, 5s));
    bootstrap.get(); ASSERT_FALSE(HasFatalFailure());
    link.attest_for_test([&](const transport::TcpTransport&, const trust::PhoneIdentity& phone) {
        return phone.key == trust::identity_key(identity);
    });
    auto tcp = transport::TcpTransport::connect(endpoint.ip_address().c_str(), static_cast<std::uint16_t>(endpoint.port()));
    ASSERT_TRUE(tcp); tcp.value()->set_io_deadline(Clock::now() + 5s);
    std::atomic<bool> finished{}, active{}, release_phone{};
    auto phone = std::async(std::launch::async, [&] {
        wireless_test::walk_phone(*tcp.value(), [&] { return active.load(); });
        finished = true;
        const auto until = Clock::now() + 15s;
        while (!release_phone && Clock::now() < until) std::this_thread::sleep_for(10ms);
        tcp.value()->close();
    });
    bool input_sent{}, grace_at_nine{}, scanned_active{};
    std::optional<Clock::time_point> consumer_lost;
    const auto began = Clock::now();
    while (Clock::now() - began < 20s) {
        const auto result = link.poll(coordinator, [&] {
            if (finished && !consumer_lost) {
                coordinator.tick(true);
                consumer_lost = Clock::now(); peers.remove_owner(name);
            }
            const auto state = control.state();
            coordinator.consumer(state.consumer.value ? std::optional{state.consumer} : std::nullopt);
            if (consumer_lost) {
                EXPECT_FALSE(coordinator.admitting());
                if (Clock::now() - *consumer_lost >= 9s && Clock::now() - *consumer_lost < 9500ms) {
                    EXPECT_TRUE(coordinator.valid_session()); grace_at_nine = true;
                }
            }
        });
        if (!result) { EXPECT_TRUE(finished.load()) << static_cast<int>(result.error().code()); break; }
        if (link.session() && link.session()->state() == session::State::active) {
            active = true; EXPECT_TRUE(coordinator.bootstrap_completed());
            if (!scanned_active) {
                EXPECT_FALSE(diagnostic_sink.recorded.empty());
                surfaces(diagnostic_sink, control, events); scanned_active = true;
            }
            if (!audio.messages.empty() && !input_sent) {
                ASSERT_TRUE(link.session()->submit_input(ipc::InputEvent{ipc::KeyEvent{66, true}})); input_sent = true;
            }
        }
    }
    release_phone = true;
    phone.get();
    EXPECT_TRUE(active); EXPECT_TRUE(input_sent);
    ASSERT_TRUE(consumer_lost); EXPECT_TRUE(grace_at_nine);
    const auto grace_elapsed = Clock::now() - *consumer_lost;
    RecordProperty("consumer_grace_ms", std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(grace_elapsed).count()));
    EXPECT_GE(grace_elapsed, 10s); EXPECT_LT(grace_elapsed, 11s);
    EXPECT_FALSE(coordinator.valid_session()); EXPECT_FALSE(link.listening());
    std::cout << "consumer_grace_ms=" << std::chrono::duration_cast<std::chrono::milliseconds>(grace_elapsed).count() << '\n';
    ASSERT_EQ(video.messages.size(), 1U); ASSERT_EQ(audio.messages.size(), 1U);
    EXPECT_EQ(video.messages.front(), (std::vector<std::byte>{std::byte{0x10}, std::byte{0x20}}));
    EXPECT_EQ(audio.messages.front(), (std::vector<std::byte>{std::byte{0x30}, std::byte{0x40}}));
    std::cout << "composed_tls_active_ms=" << std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - began).count() << '\n';
    RecordProperty("projection_and_grace_ms", std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - began).count()));
    link.stop(); coordinator.retire(); confirmation.flush(true);
    EXPECT_TRUE(scanned_active); surfaces(diagnostic_sink, control, events);
}
TEST_F(WirelessComposedTls, SnapshotDenialBeforeReleaseDoesNotDiscloseProvisionedPassword) {
    // Given: the composed helper loaded the sentinel and a real consumer owns admission.
    start(); acquire(99); ASSERT_TRUE(state(State::active));
    const trust::TransportIdentity identity{trust::WirelessIdentity{"02:00:00:00:00:01", "02:00:00:00:00:AA"}};
    ASSERT_TRUE(store->approve(identity)); ASSERT_EQ(bluez.command("paired"), "ok");
    SecretDiagnostics diagnostic_sink;
    diagnostics::Diagnostics diagnostics{diagnostic_sink};
    session::SteadyClock clock;
    app::WirelessLink link{{*store, clock, diagnostics}, {}};
    auto operations = app::handoff_operations(*client);
    auto authorize = std::move(operations.authorize);
    std::atomic<bool> fenced{};
    operations.authorize = [&](Source source, std::uint64_t handle, bool allow) {
        if (allow) { nm->changed_mac = true; fenced = true; }
        return authorize(source, handle, allow);
    };
    app::HandoffEligibilityCoordinator coordinator{*store, std::move(operations),
        {[&](const ApSnapshot& ap) {
            EXPECT_TRUE(ap.password == sentinel); return link.listen(ap);
        }, [&] { link.close_admission(); }}};
    app::WirelessConfirmation confirmation{*client, [] { return false; },
        [&](core::ConsumerId) { coordinator.consumer(std::nullopt); }};
    trust::SteadyClock trust_clock;
    trust::PairingPolicy phones{*store, trust_clock};
    ipc::test::FakePeerCredentials peers;
    ipc::test::RecordingEventSink events;
    ipc::test::SequencePairingIds ids;
    ipc::ControlService control{{::getuid()}, {peers, events, ids, phones, &confirmation}};
    const ipc::BusName caller{":1.900"}, name{"org.custom.Consumer"};
    peers.add_connection(caller, ::getuid()); peers.set_owner(name, caller);
    const auto consumer = control.register_consumer(caller, name); ASSERT_TRUE(consumer);
    ASSERT_TRUE(control.start_projection(caller));
    ASSERT_TRUE(client->reconnect(identity)); ASSERT_EQ(bluez.command("NewConnection"), "allowed");
    // When: the BSSID changes after collection but before credential release.
    coordinator.consumer(consumer.value()); coordinator.begin({consumer.value().value, 0});
    ASSERT_TRUE(eventually([&] { coordinator.tick(); return fenced && !link.listening(); }, 5s));
    // Then: no RFCOMM endpoint/credential bytes escaped, and all observed surfaces are clean.
    EXPECT_EQ(bluez.command("wire-read"), "closed");
    EXPECT_FALSE(link.listening()); EXPECT_FALSE(coordinator.admitting());
    HandoffFailure failure{};
    EXPECT_FALSE(client->poll_bootstrap({consumer.value().value, 0}, failure));
    EXPECT_EQ(failure, HandoffFailure::snapshot);
    surfaces(diagnostic_sink, control, events);
    peers.remove_owner(name); EXPECT_EQ(control.state().consumer.value, 0U);
    link.stop(); coordinator.retire(); surfaces(diagnostic_sink, control, events);
}
}
}
