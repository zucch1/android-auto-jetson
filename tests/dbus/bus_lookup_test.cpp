// SPDX-License-Identifier: GPL-3.0-or-later
// Task 21 real-bus qualification (runs on a private bus via dbus-run-session):
// the production BusPeerLookup answers real connection Unix UIDs and name
// ownership from the bus daemon, and the full start/stop/state flow runs
// against a dummy consumer holding a real bus name. The different-UID case
// needs a foreign Unix UID the private bus cannot produce and is covered by
// the deterministic double in control_service_test.cpp.
#include <aa/ipc/BusPeerLookup.hpp>
#include <aa/ipc/ControlService.hpp>

#include "../../src/ipc/dbus_adapter/BusConnection.hpp"
#include "fakes.hpp"

#include <unistd.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

#include <gtest/gtest.h>

namespace {

using aa::ErrorCode;
using aa::ipc::BusName;
using aa::ipc::BusPeerLookup;
using aa::ipc::ControlService;
using aa::ipc::ControlServiceConfig;
using aa::ipc::ControlServiceDeps;
using aa::ipc::ProjectionState;
using aa::ipc::test::RecordingEventSink;
using aa::ipc::test::RecordingPhoneDirectory;
using aa::ipc::test::SequencePairingIds;
namespace bus = aa::ipc::dbus_adapter;

constexpr const char* kConsumerName = "org.custom.AndroidAutoReceiverTest.Consumer";

struct RealBusFixture final {
    BusPeerLookup lookup{};
    std::optional<bus::BusConnection> consumer{};
    RecordingEventSink events{};
    SequencePairingIds ids{};
    RecordingPhoneDirectory phones{};
    std::unique_ptr<ControlService> service;
};

[[nodiscard]] bool owns_test_name(bus::BusConnection& connection) {
    const auto outcome = connection.request_name(BusName{kConsumerName});
    return outcome.has_value();
}

constexpr int kDaemonSyncAttempts = 1000;

// Synchronizes on the daemon's own GetNameOwner answers (one real bus round
// trip per attempt — never a fixed sleep): true once the name is unowned.
[[nodiscard]] bool wait_until_unowned(BusPeerLookup& lookup, const BusName& name) {
    for (int attempt = 0; attempt < kDaemonSyncAttempts; ++attempt) {
        if (!lookup.name_owner(name).has_value()) {
            return true;
        }
    }
    return false;
}

TEST(BusLookup, PrivateBusReportsRealUnixUidAndNameOwnership) {
    // Given: a lookup on the private bus.
    BusPeerLookup lookup;
    ASSERT_TRUE(lookup.connected());

    // When: the daemon's own registered name and a real connection are probed.
    const auto daemon_uid = lookup.unix_uid(BusName{"org.freedesktop.DBus"});
    const auto daemon_owner = lookup.name_owner(BusName{"org.freedesktop.DBus"});
    const auto connected = bus::BusConnection::connect_session();
    ASSERT_TRUE(connected.has_value());
    const auto own_uid = lookup.unix_uid(connected.value().unique_name());

    // Then: both report the real session user and a real owner (the daemon
    // reports its own well-known name for itself; client connections report
    // unique names, proven in RequestNameBindsTheWellKnownNameToTheConnection).
    ASSERT_TRUE(daemon_uid.has_value());
    EXPECT_EQ(daemon_uid.value(), static_cast<std::uint32_t>(::getuid()));
    ASSERT_TRUE(daemon_owner.has_value());
    EXPECT_FALSE(daemon_owner.value().value.empty());
    const auto owner_uid = lookup.unix_uid(daemon_owner.value());
    ASSERT_TRUE(owner_uid.has_value());
    EXPECT_EQ(owner_uid.value(), static_cast<std::uint32_t>(::getuid()));
    ASSERT_TRUE(own_uid.has_value());
    EXPECT_EQ(own_uid.value(), static_cast<std::uint32_t>(::getuid()));
}

TEST(BusLookup, UnownedNamesFailClosed) {
    // Given: a lookup on the private bus.
    BusPeerLookup lookup;
    ASSERT_TRUE(lookup.connected());

    // When: an unowned well-known name is probed for owner and uid.
    const auto owner = lookup.name_owner(BusName{"org.custom.NobodyHome"});
    const auto uid = lookup.unix_uid(BusName{"org.custom.NobodyHome"});

    // Then: both are typed rejections, never default values.
    ASSERT_FALSE(owner.has_value());
    EXPECT_EQ(owner.error().code(), ErrorCode::ipc_peer_unauthorized);
    ASSERT_FALSE(uid.has_value());
    EXPECT_EQ(uid.error().code(), ErrorCode::ipc_peer_unauthorized);
}

TEST(BusLookup, RequestNameBindsTheWellKnownNameToTheConnection) {
    // Given: two real bus connections.
    auto first = bus::BusConnection::connect_session();
    auto second = bus::BusConnection::connect_session();
    ASSERT_TRUE(first.has_value());
    ASSERT_TRUE(second.has_value());
    BusPeerLookup lookup;
    ASSERT_TRUE(lookup.connected());

    // When: the first connection takes the test consumer name.
    ASSERT_TRUE(owns_test_name(first.value()));
    const auto owner = lookup.name_owner(BusName{kConsumerName});
    const auto denied = second.value().request_name(BusName{kConsumerName});

    // Then: the bus reports the first connection as owner and refuses the
    // second (already-owned) request.
    ASSERT_TRUE(owner.has_value());
    EXPECT_EQ(owner.value(), first.value().unique_name());
    ASSERT_FALSE(denied.has_value());
}

TEST(BusFlow, RealBusHappyStartStopStateFlowWithDummyConsumer) {
    // Given: a consumer connection owning its name on the private bus.
    RealBusFixture fixture;
    ASSERT_TRUE(fixture.lookup.connected());
    auto connected = bus::BusConnection::connect_session();
    ASSERT_TRUE(connected.has_value());
    fixture.consumer = std::move(connected.value());
    ASSERT_TRUE(owns_test_name(*fixture.consumer));
    fixture.service = std::make_unique<ControlService>(
        ControlServiceConfig{static_cast<std::uint32_t>(::getuid())},
        ControlServiceDeps{fixture.lookup, fixture.events, fixture.ids, fixture.phones});
    aa::ipc::test::DummyConsumerClient client(*fixture.service, fixture.consumer->unique_name(),
                                              BusName{kConsumerName});

    // When: the dummy consumer registers, starts, stops and unregisters.
    const auto registered = client.register_self();
    ASSERT_TRUE(registered.has_value()) << "uid/ownership lookup must pass on the real bus";
    ASSERT_TRUE(client.start().has_value());
    const auto projecting = client.state();
    ASSERT_TRUE(client.stop().has_value());
    const auto done = client.state();
    ASSERT_TRUE(client.unregister_self().has_value());

    // Then: the contract state tracks the flow and metadata is real.
    EXPECT_EQ(projecting.projection, ProjectionState::projecting);
    EXPECT_EQ(done.projection, ProjectionState::stopped);
    EXPECT_EQ(fixture.service->state().consumer, aa::core::ConsumerId{});
    EXPECT_EQ(fixture.service->capabilities().at("contract.semver"), "1.0.0");
    EXPECT_GT(fixture.service->ping(), 0U);
}

TEST(BusFlow, SameUidUnregisteredSecondClientIsRejectedOnRealBus) {
    // Given: a registered consumer and a second same-UID connection.
    RealBusFixture fixture;
    ASSERT_TRUE(fixture.lookup.connected());
    auto first_conn = bus::BusConnection::connect_session();
    auto second_conn = bus::BusConnection::connect_session();
    ASSERT_TRUE(first_conn.has_value());
    ASSERT_TRUE(second_conn.has_value());
    fixture.consumer = std::move(first_conn.value());
    ASSERT_TRUE(owns_test_name(*fixture.consumer));
    fixture.service = std::make_unique<ControlService>(
        ControlServiceConfig{static_cast<std::uint32_t>(::getuid())},
        ControlServiceDeps{fixture.lookup, fixture.events, fixture.ids, fixture.phones});
    ASSERT_TRUE(fixture.service
                    ->register_consumer(fixture.consumer->unique_name(), BusName{kConsumerName})
                    .has_value());

    // When: the second client drives projection and pairing.
    const auto denied_start = fixture.service->start_projection(second_conn.value().unique_name());
    const auto denied_add = fixture.service->request_add_phone(second_conn.value().unique_name());

    // Then: the unregistered client is rejected on both paths.
    ASSERT_FALSE(denied_start.has_value());
    EXPECT_EQ(denied_start.error().code(), ErrorCode::ipc_peer_unauthorized);
    ASSERT_FALSE(denied_add.has_value());
    EXPECT_EQ(denied_add.error().code(), ErrorCode::ipc_peer_unauthorized);
}

TEST(BusFlow, ConsumerNameOwnedByAnotherConnectionIsRejectedOnRealBus) {
    // Given: the test consumer name owned by one connection.
    RealBusFixture fixture;
    ASSERT_TRUE(fixture.lookup.connected());
    auto first_conn = bus::BusConnection::connect_session();
    auto second_conn = bus::BusConnection::connect_session();
    ASSERT_TRUE(first_conn.has_value());
    ASSERT_TRUE(second_conn.has_value());
    fixture.consumer = std::move(first_conn.value());
    ASSERT_TRUE(owns_test_name(*fixture.consumer));
    fixture.service = std::make_unique<ControlService>(
        ControlServiceConfig{static_cast<std::uint32_t>(::getuid())},
        ControlServiceDeps{fixture.lookup, fixture.events, fixture.ids, fixture.phones});

    // When: the second connection claims the foreign name.
    const auto denied = fixture.service->register_consumer(second_conn.value().unique_name(),
                                                           BusName{kConsumerName});

    // Then: registration fails closed on real name ownership.
    ASSERT_FALSE(denied.has_value());
    EXPECT_EQ(denied.error().code(), ErrorCode::ipc_peer_unauthorized);
}

TEST(BusFlow, DisconnectedConsumerCanBeReplaced) {
    // Given: a registered consumer whose connection dies without unregistering.
    RealBusFixture fixture;
    ASSERT_TRUE(fixture.lookup.connected());
    ASSERT_TRUE(wait_until_unowned(fixture.lookup, BusName{kConsumerName}));
    auto first_conn = bus::BusConnection::connect_session();
    ASSERT_TRUE(first_conn.has_value());
    fixture.consumer = std::move(first_conn.value());
    ASSERT_TRUE(owns_test_name(*fixture.consumer));
    fixture.service = std::make_unique<ControlService>(
        ControlServiceConfig{static_cast<std::uint32_t>(::getuid())},
        ControlServiceDeps{fixture.lookup, fixture.events, fixture.ids, fixture.phones});
    ASSERT_TRUE(fixture.service
                    ->register_consumer(fixture.consumer->unique_name(), BusName{kConsumerName})
                    .has_value());
    const auto abandoned = fixture.consumer->unique_name();
    fixture.consumer.reset();

    // When: the daemon reports the name unowned and a new connection registers.
    ASSERT_TRUE(wait_until_unowned(fixture.lookup, BusName{kConsumerName}));
    auto second_conn = bus::BusConnection::connect_session();
    ASSERT_TRUE(second_conn.has_value());
    fixture.consumer = std::move(second_conn.value());
    ASSERT_TRUE(owns_test_name(*fixture.consumer));
    const auto replacement = fixture.service->register_consumer(fixture.consumer->unique_name(),
                                                                BusName{kConsumerName});

    // Then: the abandoned registration is torn down and the replacement gets a
    // fresh consumer id.
    ASSERT_TRUE(replacement.has_value());
    EXPECT_EQ(replacement.value(), aa::core::ConsumerId{2});
    EXPECT_EQ(fixture.service->state().consumer, replacement.value());
    std::size_t owner_lost_diagnostics = 0;
    for (const auto& event : fixture.events.events) {
        if (const auto* diagnostic = std::get_if<aa::ipc::DiagnosticEmitted>(&event)) {
            if (diagnostic->event == "consumer_owner_lost") {
                ++owner_lost_diagnostics;
                EXPECT_EQ(diagnostic->subject, 1U);
            }
        }
    }
    EXPECT_EQ(owner_lost_diagnostics, 1U);
    EXPECT_NE(fixture.consumer->unique_name(), abandoned);
}

TEST(BusFlow, ReleasedConsumerNameRevokesRegistration) {
    // Given: a registered consumer that releases its name but stays connected.
    RealBusFixture fixture;
    ASSERT_TRUE(fixture.lookup.connected());
    ASSERT_TRUE(wait_until_unowned(fixture.lookup, BusName{kConsumerName}));
    auto connected = bus::BusConnection::connect_session();
    ASSERT_TRUE(connected.has_value());
    fixture.consumer = std::move(connected.value());
    ASSERT_TRUE(owns_test_name(*fixture.consumer));
    fixture.service = std::make_unique<ControlService>(
        ControlServiceConfig{static_cast<std::uint32_t>(::getuid())},
        ControlServiceDeps{fixture.lookup, fixture.events, fixture.ids, fixture.phones});
    ASSERT_TRUE(fixture.service
                    ->register_consumer(fixture.consumer->unique_name(), BusName{kConsumerName})
                    .has_value());
    ASSERT_TRUE(fixture.consumer->release_name(BusName{kConsumerName}).has_value());

    // When: the daemon reports the name unowned and the registration reconciles.
    ASSERT_TRUE(wait_until_unowned(fixture.lookup, BusName{kConsumerName}));
    fixture.service->refresh_consumer_owner();

    // Then: authority is revoked while the connection itself is still alive.
    EXPECT_EQ(fixture.service->state().consumer, aa::core::ConsumerId{});
    std::size_t owner_lost_diagnostics = 0;
    for (const auto& event : fixture.events.events) {
        if (const auto* diagnostic = std::get_if<aa::ipc::DiagnosticEmitted>(&event)) {
            if (diagnostic->event == "consumer_owner_lost") {
                ++owner_lost_diagnostics;
            }
        }
    }
    EXPECT_EQ(owner_lost_diagnostics, 1U);
}

TEST(BusFlow, ConsumerNameTransferRequiresFreshRegistration) {
    // Given: a registered consumer with a pending pairing request and a second
    // live connection that takes the consumer name over.
    RealBusFixture fixture;
    ASSERT_TRUE(fixture.lookup.connected());
    ASSERT_TRUE(wait_until_unowned(fixture.lookup, BusName{kConsumerName}));
    auto first_conn = bus::BusConnection::connect_session();
    auto second_conn = bus::BusConnection::connect_session();
    ASSERT_TRUE(first_conn.has_value());
    ASSERT_TRUE(second_conn.has_value());
    fixture.consumer = std::move(first_conn.value());
    ASSERT_TRUE(owns_test_name(*fixture.consumer));
    fixture.service = std::make_unique<ControlService>(
        ControlServiceConfig{static_cast<std::uint32_t>(::getuid())},
        ControlServiceDeps{fixture.lookup, fixture.events, fixture.ids, fixture.phones});
    ASSERT_TRUE(fixture.service
                    ->register_consumer(fixture.consumer->unique_name(), BusName{kConsumerName})
                    .has_value());
    const auto request = fixture.service->request_add_phone(fixture.consumer->unique_name());
    ASSERT_TRUE(request.has_value());
    ASSERT_TRUE(fixture.consumer->release_name(BusName{kConsumerName}).has_value());
    ASSERT_TRUE(second_conn.value().request_name(BusName{kConsumerName}).has_value());
    bool transferred = false;
    for (int attempt = 0; attempt < kDaemonSyncAttempts && !transferred; ++attempt) {
        const auto current = fixture.lookup.name_owner(BusName{kConsumerName});
        transferred = current.has_value() && current.value() == second_conn.value().unique_name();
    }
    ASSERT_TRUE(transferred);

    // When: the transferred connection drives projection and pairing.
    const auto started =
        fixture.service->start_projection(second_conn.value().unique_name());
    const auto confirmed = fixture.service->confirm_phone_pairing(
        second_conn.value().unique_name(), request.value());

    // Then: the transfer grants no inherited authority and no inherited
    // pairing; only a fresh registration (a NEW consumer id) may drive control.
    ASSERT_FALSE(started.has_value());
    EXPECT_EQ(started.error().code(), ErrorCode::ipc_consumer_rejected);
    ASSERT_FALSE(confirmed.has_value());
    EXPECT_EQ(confirmed.error().code(), ErrorCode::ipc_consumer_rejected);
    const auto fresh = fixture.service->register_consumer(second_conn.value().unique_name(),
                                                          BusName{kConsumerName});
    ASSERT_TRUE(fresh.has_value());
    EXPECT_EQ(fresh.value(), aa::core::ConsumerId{2});
    const auto stale = fixture.service->confirm_phone_pairing(second_conn.value().unique_name(),
                                                              request.value());
    ASSERT_FALSE(stale.has_value());
    EXPECT_EQ(stale.error().code(), ErrorCode::ipc_consumer_rejected);
}

} // namespace
