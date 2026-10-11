// SPDX-License-Identifier: GPL-3.0-or-later
// Task 21 control-surface semantics: the happy start/stop/state flow with a
// minimal dummy consumer client and the single-consumer authorization matrix
// (different UID, same-UID unregistered client, unowned consumer name).
#include <aa/ipc/Contract.hpp>
#include <aa/ipc/ControlService.hpp>

#include "fakes.hpp"

#include <cstdint>

#include <gtest/gtest.h>

namespace {

using aa::ErrorCode;
using aa::ipc::BusName;
using aa::ipc::ControlService;
using aa::ipc::ControlServiceConfig;
using aa::ipc::ControlServiceDeps;
using aa::ipc::ProjectionState;
using aa::ipc::Viewport;

constexpr std::uint32_t kSessionUid = 1000;

struct Fixture final {
    aa::ipc::test::FakePeerCredentials credentials{};
    aa::ipc::test::RecordingEventSink events{};
    aa::ipc::test::SequencePairingIds ids{};
    aa::ipc::test::RecordingPhoneDirectory phones{};
    ControlService service;

    Fixture() : service(ControlServiceConfig{kSessionUid},
                        ControlServiceDeps{credentials, events, ids, phones}) {}
};

TEST(ControlService, DummyConsumerRunsHappyStartStopStateFlow) {
    // Given: a fixture where one consumer owns its registered name.
    Fixture fixture;
    const BusName connection{":1.10"};
    const BusName consumer_name{"org.custom.ConsumerA"};
    fixture.credentials.add_connection(connection, kSessionUid);
    fixture.credentials.set_owner(consumer_name, connection);
    aa::ipc::test::DummyConsumerClient client(fixture.service, connection, consumer_name);

    // When: the client registers, starts, observes, stops and unregisters.
    const auto registered = client.register_self();
    ASSERT_TRUE(registered.has_value());
    const auto started = client.start();
    ASSERT_TRUE(started.has_value());
    const auto projecting = client.state();
    const auto stopped = client.stop();
    ASSERT_TRUE(stopped.has_value());
    const auto done = client.state();
    const auto unregistered = client.unregister_self();
    ASSERT_TRUE(unregistered.has_value());

    // Then: state tracks projection and registration exactly.
    EXPECT_EQ(projecting.projection, ProjectionState::projecting);
    EXPECT_EQ(projecting.consumer, registered.value());
    EXPECT_EQ(done.projection, ProjectionState::stopped);
    EXPECT_EQ(done.consumer, registered.value());
    EXPECT_EQ(fixture.service.state().consumer, aa::core::ConsumerId{});
}

TEST(ControlService, StateCapabilitiesAndPingStayOpenToAnyCaller) {
    // Given: a fixture with no registered consumer.
    Fixture fixture;

    // When: an unknown caller reads status.
    const auto snapshot = fixture.service.state();
    const auto caps = fixture.service.capabilities();
    const auto stamp = fixture.service.ping();

    // Then: status is open and reports the contract metadata.
    EXPECT_EQ(snapshot.projection, ProjectionState::stopped);
    EXPECT_EQ(snapshot.consumer, aa::core::ConsumerId{});
    EXPECT_EQ(caps.at("contract.interface"), aa::ipc::kContractInterface);
    EXPECT_EQ(caps.at("contract.semver"), "1.0.0");
    EXPECT_GT(stamp, 0U);
}

TEST(ControlService, ProjectionTransitionsEmitStateChangedEvents) {
    // Given: a registered consumer.
    Fixture fixture;
    const BusName connection{":1.11"};
    const BusName consumer_name{"org.custom.ConsumerA"};
    fixture.credentials.add_connection(connection, kSessionUid);
    fixture.credentials.set_owner(consumer_name, connection);
    ASSERT_TRUE(fixture.service.register_consumer(connection, consumer_name).has_value());

    // When: projection starts and stops.
    ASSERT_TRUE(fixture.service.start_projection(connection).has_value());
    ASSERT_TRUE(fixture.service.stop_projection(connection).has_value());

    // Then: three StateChanged events (register, start, stop) carry the state.
    std::size_t state_events = 0;
    for (const auto& event : fixture.events.events) {
        if (const auto* changed = std::get_if<aa::ipc::StateChanged>(&event)) {
            ++state_events;
            EXPECT_EQ(changed->consumer, aa::core::ConsumerId{1});
        }
    }
    EXPECT_EQ(state_events, 3U);
}

TEST(ControlService, IllegalProjectionTransitionsAreTypedErrors) {
    // Given: a registered consumer not projecting.
    Fixture fixture;
    const BusName connection{":1.12"};
    const BusName consumer_name{"org.custom.ConsumerA"};
    fixture.credentials.add_connection(connection, kSessionUid);
    fixture.credentials.set_owner(consumer_name, connection);
    ASSERT_TRUE(fixture.service.register_consumer(connection, consumer_name).has_value());

    // When: stopping while stopped, then starting twice.
    const auto early_stop = fixture.service.stop_projection(connection);
    ASSERT_TRUE(fixture.service.start_projection(connection).has_value());
    const auto double_start = fixture.service.start_projection(connection);

    // Then: both misuse calls fail with the typed transition error.
    ASSERT_FALSE(early_stop.has_value());
    EXPECT_EQ(early_stop.error().code(), ErrorCode::session_illegal_transition);
    ASSERT_FALSE(double_start.has_value());
    EXPECT_EQ(double_start.error().code(), ErrorCode::session_illegal_transition);
}

TEST(ControlService, SecondRegistrationIsRejectedUntilUnregister) {
    // Given: one registered consumer.
    Fixture fixture;
    const BusName first{":1.20"};
    const BusName first_name{"org.custom.ConsumerA"};
    const BusName second{":1.21"};
    const BusName second_name{"org.custom.ConsumerB"};
    fixture.credentials.add_connection(first, kSessionUid);
    fixture.credentials.add_connection(second, kSessionUid);
    fixture.credentials.set_owner(first_name, first);
    fixture.credentials.set_owner(second_name, second);
    ASSERT_TRUE(fixture.service.register_consumer(first, first_name).has_value());

    // When: the second client registers before and after the first leaves.
    const auto denied = fixture.service.register_consumer(second, second_name);
    ASSERT_TRUE(fixture.service.unregister_consumer(first).has_value());
    const auto allowed = fixture.service.register_consumer(second, second_name);

    // Then: exclusivity holds exactly while a consumer is active.
    ASSERT_FALSE(denied.has_value());
    EXPECT_EQ(denied.error().code(), ErrorCode::ipc_consumer_rejected);
    EXPECT_TRUE(allowed.has_value());
}

TEST(ControlService, UnregisterStopsProjectionAndClosesPairing) {
    // Given: a projecting consumer with one pending pairing request.
    Fixture fixture;
    const BusName connection{":1.30"};
    const BusName consumer_name{"org.custom.ConsumerA"};
    fixture.credentials.add_connection(connection, kSessionUid);
    fixture.credentials.set_owner(consumer_name, connection);
    ASSERT_TRUE(fixture.service.register_consumer(connection, consumer_name).has_value());
    ASSERT_TRUE(fixture.service.start_projection(connection).has_value());
    const auto request = fixture.service.request_add_phone(connection);
    ASSERT_TRUE(request.has_value());

    // When: the consumer unregisters.
    ASSERT_TRUE(fixture.service.unregister_consumer(connection).has_value());

    // Then: projection stopped, pairing closed, registration gone.
    EXPECT_EQ(fixture.service.state().projection, ProjectionState::stopped);
    EXPECT_EQ(fixture.service.state().consumer, aa::core::ConsumerId{});
    bool saw_close = false;
    for (const auto& event : fixture.events.events) {
        if (const auto* closed = std::get_if<aa::ipc::PairingClosed>(&event)) {
            saw_close = closed->request == request.value();
        }
    }
    EXPECT_TRUE(saw_close);
}

TEST(ControlService, SetDisplayViewportStoresBoundedGeometry) {
    // Given: a registered consumer.
    Fixture fixture;
    const BusName connection{":1.40"};
    const BusName consumer_name{"org.custom.ConsumerA"};
    fixture.credentials.add_connection(connection, kSessionUid);
    fixture.credentials.set_owner(consumer_name, connection);
    ASSERT_TRUE(fixture.service.register_consumer(connection, consumer_name).has_value());

    // When: a valid viewport is set and then an out-of-range one.
    const auto accepted = fixture.service.set_display_viewport(connection, Viewport{1280, 720, 160});
    const auto rejected = fixture.service.set_display_viewport(connection, Viewport{0, 720, 160});

    // Then: the bounded viewport is stored and the invalid one is typed.
    ASSERT_TRUE(accepted.has_value());
    ASSERT_TRUE(fixture.service.viewport().has_value());
    EXPECT_EQ(fixture.service.viewport()->width, 1280);
    ASSERT_FALSE(rejected.has_value());
    EXPECT_EQ(rejected.error().code(), ErrorCode::invalid_argument);
}

TEST(ControlService, ForgetPhoneForwardsToDirectory) {
    // Given: a registered consumer.
    Fixture fixture;
    const BusName connection{":1.41"};
    const BusName consumer_name{"org.custom.ConsumerA"};
    fixture.credentials.add_connection(connection, kSessionUid);
    fixture.credentials.set_owner(consumer_name, connection);
    ASSERT_TRUE(fixture.service.register_consumer(connection, consumer_name).has_value());

    // When: a phone id is forgotten and then an unset id.
    const auto forgotten = fixture.service.forget_phone(connection, aa::core::PhoneId{7});
    const auto invalid = fixture.service.forget_phone(connection, aa::core::PhoneId{});

    // Then: the directory saw the id and the unset id is typed invalid.
    ASSERT_TRUE(forgotten.has_value());
    ASSERT_EQ(fixture.phones.forgotten.size(), 1U);
    EXPECT_EQ(fixture.phones.forgotten.front(), aa::core::PhoneId{7});
    ASSERT_FALSE(invalid.has_value());
    EXPECT_EQ(invalid.error().code(), ErrorCode::invalid_argument);
}

TEST(AccessControl, DifferentUidClientIsRejected) {
    // Given: a consumer registered by the session user.
    Fixture fixture;
    const BusName owner{":1.50"};
    const BusName consumer_name{"org.custom.ConsumerA"};
    fixture.credentials.add_connection(owner, kSessionUid);
    fixture.credentials.set_owner(consumer_name, owner);
    ASSERT_TRUE(fixture.service.register_consumer(owner, consumer_name).has_value());

    // When: a different-UID client tries to register and to drive projection.
    const BusName stranger{":1.51"};
    const BusName stranger_name{"org.custom.ConsumerB"};
    fixture.credentials.add_connection(stranger, kSessionUid + 1);
    fixture.credentials.set_owner(stranger_name, stranger);
    const auto denied_register = fixture.service.register_consumer(stranger, stranger_name);
    const auto denied_start = fixture.service.start_projection(stranger);

    // Then: both fail closed with the peer-unauthorized error.
    ASSERT_FALSE(denied_register.has_value());
    EXPECT_EQ(denied_register.error().code(), ErrorCode::ipc_peer_unauthorized);
    ASSERT_FALSE(denied_start.has_value());
    EXPECT_EQ(denied_start.error().code(), ErrorCode::ipc_peer_unauthorized);
}

TEST(AccessControl, SameUidUnregisteredSecondClientIsRejected) {
    // Given: an active consumer and a second same-UID connection that owns
    // nothing the consumer registered.
    Fixture fixture;
    const BusName owner{":1.60"};
    const BusName consumer_name{"org.custom.ConsumerA"};
    fixture.credentials.add_connection(owner, kSessionUid);
    fixture.credentials.set_owner(consumer_name, owner);
    ASSERT_TRUE(fixture.service.register_consumer(owner, consumer_name).has_value());
    const BusName second{":1.61"};
    fixture.credentials.add_connection(second, kSessionUid);

    // When: the second client drives projection and pairing.
    const auto denied_start = fixture.service.start_projection(second);
    const auto denied_add = fixture.service.request_add_phone(second);

    // Then: the unregistered client is rejected on every control path.
    ASSERT_FALSE(denied_start.has_value());
    EXPECT_EQ(denied_start.error().code(), ErrorCode::ipc_peer_unauthorized);
    ASSERT_FALSE(denied_add.has_value());
    EXPECT_EQ(denied_add.error().code(), ErrorCode::ipc_peer_unauthorized);
}

TEST(AccessControl, ConsumerNameOwnedBySomeoneElseIsRejectedAtRegistration) {
    // Given: a well-known name owned by another connection.
    Fixture fixture;
    const BusName caller{":1.70"};
    const BusName foreign{"org.custom.ConsumerOwnedElsewhere"};
    fixture.credentials.add_connection(caller, kSessionUid);
    fixture.credentials.set_owner(foreign, BusName{":1.71"});

    // When: the caller tries to register that foreign name.
    const auto denied = fixture.service.register_consumer(caller, foreign);

    // Then: registration fails closed on name ownership.
    ASSERT_FALSE(denied.has_value());
    EXPECT_EQ(denied.error().code(), ErrorCode::ipc_peer_unauthorized);
}

TEST(AccessControl, OperationsWithoutRegisteredConsumerAreRejected) {
    // Given: a fixture with no consumer.
    Fixture fixture;
    const BusName caller{":1.80"};
    fixture.credentials.add_connection(caller, kSessionUid);

    // When: control methods are called with nothing registered.
    const auto start = fixture.service.start_projection(caller);
    const auto add_phone = fixture.service.request_add_phone(caller);

    // Then: both are typed consumer-rejected errors.
    ASSERT_FALSE(start.has_value());
    EXPECT_EQ(start.error().code(), ErrorCode::ipc_consumer_rejected);
    ASSERT_FALSE(add_phone.has_value());
    EXPECT_EQ(add_phone.error().code(), ErrorCode::ipc_consumer_rejected);
}

TEST(AccessControl, UnknownConnectionFailsClosed) {
    // Given: a fixture whose lookup knows no caller.
    Fixture fixture;

    // When: an unknown connection tries to register.
    const auto denied =
        fixture.service.register_consumer(BusName{":1.99"}, BusName{"org.custom.ConsumerA"});

    // Then: the missing peer credential is a rejection, never a default.
    ASSERT_FALSE(denied.has_value());
    EXPECT_EQ(denied.error().code(), ErrorCode::ipc_peer_unauthorized);
}

TEST(ControlService, OwnerLossStopsProjectionClosesAllRequestsAndClearsRegistration) {
    // Given: a projecting consumer holding a viewport, with one confirmed
    // pairing window and one pending pairing request.
    Fixture fixture;
    const BusName connection{":1.100"};
    const BusName consumer_name{"org.custom.ConsumerA"};
    fixture.credentials.add_connection(connection, kSessionUid);
    fixture.credentials.set_owner(consumer_name, connection);
    ASSERT_TRUE(fixture.service.register_consumer(connection, consumer_name).has_value());
    ASSERT_TRUE(fixture.service.start_projection(connection).has_value());
    ASSERT_TRUE(
        fixture.service.set_display_viewport(connection, Viewport{1280, 720, 160}).has_value());
    const auto pending = fixture.service.request_add_phone(connection);
    const auto confirmed = fixture.service.request_add_phone(connection);
    ASSERT_TRUE(pending.has_value());
    ASSERT_TRUE(confirmed.has_value());
    ASSERT_TRUE(fixture.service.confirm_phone_pairing(connection, confirmed.value()).has_value());
    ASSERT_TRUE(fixture.service.pairing_window_open());

    // When: the consumer name loses its owner and lookup-on-use reconciles.
    fixture.credentials.remove_owner(consumer_name);
    fixture.service.refresh_consumer_owner();

    // Then: projection stops for the old consumer, registration and viewport
    // clear, the window closes, and each request id closes exactly once.
    const auto snapshot = fixture.service.state();
    EXPECT_EQ(snapshot.projection, ProjectionState::stopped);
    EXPECT_EQ(snapshot.consumer, aa::core::ConsumerId{});
    EXPECT_FALSE(fixture.service.viewport().has_value());
    EXPECT_FALSE(fixture.service.pairing_window_open());
    std::size_t pending_closes = 0;
    std::size_t confirmed_closes = 0;
    bool saw_stopped_for_old = false;
    bool saw_cleared_state = false;
    std::size_t owner_lost_diagnostics = 0;
    for (const auto& event : fixture.events.events) {
        if (const auto* closed = std::get_if<aa::ipc::PairingClosed>(&event)) {
            if (closed->request == pending.value()) {
                ++pending_closes;
            }
            if (closed->request == confirmed.value()) {
                ++confirmed_closes;
            }
        }
        if (const auto* changed = std::get_if<aa::ipc::StateChanged>(&event)) {
            if (changed->projection == ProjectionState::stopped &&
                changed->consumer == aa::core::ConsumerId{}) {
                saw_cleared_state = true;
            }
            if (changed->projection == ProjectionState::stopped &&
                changed->consumer != aa::core::ConsumerId{}) {
                saw_stopped_for_old = true;
            }
        }
        if (const auto* diagnostic = std::get_if<aa::ipc::DiagnosticEmitted>(&event)) {
            if (diagnostic->event == "consumer_owner_lost") {
                ++owner_lost_diagnostics;
                EXPECT_EQ(diagnostic->severity, aa::ipc::DiagnosticSeverity::warning);
            }
        }
    }
    EXPECT_EQ(pending_closes, 1U);
    EXPECT_EQ(confirmed_closes, 1U);
    EXPECT_TRUE(saw_stopped_for_old);
    EXPECT_TRUE(saw_cleared_state);
    EXPECT_EQ(owner_lost_diagnostics, 1U);
}

TEST(ControlService, OwnerRefreshIsIdempotent) {
    // Given: a registered consumer whose name just lost its owner.
    Fixture fixture;
    const BusName connection{":1.101"};
    const BusName consumer_name{"org.custom.ConsumerA"};
    fixture.credentials.add_connection(connection, kSessionUid);
    fixture.credentials.set_owner(consumer_name, connection);
    ASSERT_TRUE(fixture.service.register_consumer(connection, consumer_name).has_value());
    fixture.credentials.remove_owner(consumer_name);

    // When: reconciliation runs repeatedly and again with no registration.
    fixture.service.refresh_consumer_owner();
    const auto after_first = fixture.events.events.size();
    fixture.service.refresh_consumer_owner();
    fixture.service.refresh_consumer_owner();

    // Then: the teardown happened exactly once and later refreshes are no-ops.
    EXPECT_EQ(fixture.events.events.size(), after_first);
    std::size_t owner_lost_diagnostics = 0;
    for (const auto& event : fixture.events.events) {
        if (const auto* diagnostic = std::get_if<aa::ipc::DiagnosticEmitted>(&event)) {
            if (diagnostic->event == "consumer_owner_lost") {
                ++owner_lost_diagnostics;
            }
        }
    }
    EXPECT_EQ(owner_lost_diagnostics, 1U);
    EXPECT_EQ(fixture.service.state().consumer, aa::core::ConsumerId{});
}

TEST(ControlService, ReplacementRegistrationReconcilesAbandonedOwner) {
    // Given: an incumbent that abandoned its consumer name (abrupt loss).
    Fixture fixture;
    const BusName first{":1.102"};
    const BusName first_name{"org.custom.ConsumerA"};
    const BusName second{":1.103"};
    const BusName second_name{"org.custom.ConsumerB"};
    fixture.credentials.add_connection(first, kSessionUid);
    fixture.credentials.add_connection(second, kSessionUid);
    fixture.credentials.set_owner(first_name, first);
    fixture.credentials.set_owner(second_name, second);
    ASSERT_TRUE(fixture.service.register_consumer(first, first_name).has_value());
    fixture.credentials.remove_owner(first_name);

    // When: a replacement registers without any prior status read or refresh.
    const auto replacement = fixture.service.register_consumer(second, second_name);

    // Then: the abandoned owner is reconciled and the replacement gets a new
    // consumer id; nothing of the incumbent is inherited.
    ASSERT_TRUE(replacement.has_value());
    EXPECT_EQ(replacement.value(), aa::core::ConsumerId{2});
    EXPECT_EQ(fixture.service.state().consumer, replacement.value());
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

TEST(ControlService, LiveOwnerStillBlocksReplacement) {
    // Given: a live incumbent that still owns its registered name.
    Fixture fixture;
    const BusName first{":1.104"};
    const BusName first_name{"org.custom.ConsumerA"};
    const BusName second{":1.105"};
    const BusName second_name{"org.custom.ConsumerB"};
    fixture.credentials.add_connection(first, kSessionUid);
    fixture.credentials.add_connection(second, kSessionUid);
    fixture.credentials.set_owner(first_name, first);
    fixture.credentials.set_owner(second_name, second);
    ASSERT_TRUE(fixture.service.register_consumer(first, first_name).has_value());

    // When: a second same-UID consumer tries to replace it.
    const auto denied = fixture.service.register_consumer(second, second_name);

    // Then: exclusivity still holds and the incumbent keeps its registration.
    ASSERT_FALSE(denied.has_value());
    EXPECT_EQ(denied.error().code(), ErrorCode::ipc_consumer_rejected);
    EXPECT_EQ(fixture.service.state().consumer, aa::core::ConsumerId{1});
}

TEST(ControlService, OwnerLookupFailureRevokesAuthority) {
    // Given: a registered consumer holding a viewport.
    Fixture fixture;
    const BusName connection{":1.106"};
    const BusName consumer_name{"org.custom.ConsumerA"};
    fixture.credentials.add_connection(connection, kSessionUid);
    fixture.credentials.set_owner(consumer_name, connection);
    ASSERT_TRUE(fixture.service.register_consumer(connection, consumer_name).has_value());
    ASSERT_TRUE(
        fixture.service.set_display_viewport(connection, Viewport{1280, 720, 160}).has_value());

    // When: the ownership lookup turns unverifiable and reconciliation runs.
    fixture.credentials.fail_owner_lookup(consumer_name);
    fixture.service.refresh_consumer_owner();

    // Then: authority is revoked closed, and even a recovered lookup never
    // auto-restores the old registration.
    EXPECT_EQ(fixture.service.state().consumer, aa::core::ConsumerId{});
    EXPECT_FALSE(fixture.service.viewport().has_value());
    fixture.credentials.restore_owner_lookup(consumer_name);
    fixture.credentials.set_owner(consumer_name, connection);
    fixture.service.refresh_consumer_owner();
    const auto denied = fixture.service.start_projection(connection);
    ASSERT_FALSE(denied.has_value());
    EXPECT_EQ(denied.error().code(), ErrorCode::ipc_consumer_rejected);
    EXPECT_EQ(fixture.service.state().consumer, aa::core::ConsumerId{});
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

TEST(ControlService, OwnerLossDoesNotResetPairingIdHistory) {
    // Given: a consumer pairing through an id source that always reissues id
    // 1, with that id pending when the owner vanishes.
    aa::ipc::test::FakePeerCredentials credentials{};
    aa::ipc::test::RecordingEventSink events{};
    aa::ipc::test::CyclingPairingIds ids{1};
    aa::ipc::test::RecordingPhoneDirectory phones{};
    ControlService service{ControlServiceConfig{kSessionUid},
                           ControlServiceDeps{credentials, events, ids, phones}};
    const BusName first{":1.107"};
    const BusName first_name{"org.custom.ConsumerA"};
    const BusName second{":1.108"};
    const BusName second_name{"org.custom.ConsumerB"};
    credentials.add_connection(first, kSessionUid);
    credentials.add_connection(second, kSessionUid);
    credentials.set_owner(first_name, first);
    credentials.set_owner(second_name, second);
    ASSERT_TRUE(service.register_consumer(first, first_name).has_value());
    const auto request = service.request_add_phone(first);
    ASSERT_TRUE(request.has_value());
    ASSERT_EQ(request.value(), aa::core::PairingRequestId{1});
    credentials.remove_owner(first_name);
    service.refresh_consumer_owner();
    ASSERT_TRUE(service.register_consumer(second, second_name).has_value());

    // When: the new consumer confirms the spent id and then requests again
    // from the repeating id source.
    const auto confirmed = service.confirm_phone_pairing(second, request.value());
    const auto reissued = service.request_add_phone(second);

    // Then: the spent id stays spent (typed rejection, never a fresh unknown-id
    // parse) and the cycling source cannot reissue it after teardown.
    ASSERT_FALSE(confirmed.has_value());
    EXPECT_EQ(confirmed.error().code(), ErrorCode::ipc_consumer_rejected);
    ASSERT_FALSE(reissued.has_value());
    EXPECT_EQ(reissued.error().code(), ErrorCode::internal);
}

} // namespace
