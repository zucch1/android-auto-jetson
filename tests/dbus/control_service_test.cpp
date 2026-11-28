// SPDX-License-Identifier: GPL-3.0-or-later
// Task 21 control-surface semantics: the happy start/stop/state flow with a
// minimal dummy consumer client and the single-consumer authorization matrix
// (different UID, same-UID unregistered client, unowned consumer name).
#include <aa/ipc/Contract.hpp>
#include <aa/ipc/ControlService.hpp>

#include "fakes.hpp"

#include <cstdint>
#include <utility>

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
    bool session_active{true};
    ControlService service;

    Fixture() : service(ControlServiceConfig{kSessionUid},
                        ControlServiceDeps{credentials, events, ids, phones, nullptr, {},
                                           [this] { return session_active; }}) {}
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
    EXPECT_EQ(caps.at("contract.semver"), "1.3.0");
    EXPECT_EQ(caps.at("phones.list"), "true");
    EXPECT_EQ(caps.at("pairing.outcomes"), "true");
    EXPECT_GT(stamp, 0U);
}

TEST(ControlService, RuntimeProjectionPolicyPreservesRegistrationAndChecksAuthority) {
    Fixture fixture;
    bool provisioned = false;
    aa::ipc::ProjectionPolicy policy{};
    policy.check_ready = [&]() -> aa::core::Result<void> {
        if (provisioned) return {};
        return aa::Error{ErrorCode::session_illegal_transition};
    };
    ControlService service{{kSessionUid}, {fixture.credentials, fixture.events, fixture.ids,
                                          fixture.phones, nullptr, {}, {}, &policy}};
    const BusName owner{":1.51"};
    const BusName same_uid{":1.52"};
    const BusName wrong_uid{":1.53"};
    const BusName name{"org.custom.ConsumerA"};
    fixture.credentials.add_connection(owner, kSessionUid);
    fixture.credentials.add_connection(same_uid, kSessionUid);
    fixture.credentials.add_connection(wrong_uid, kSessionUid + 1);
    fixture.credentials.set_owner(name, owner);

    EXPECT_FALSE(fixture.service.capabilities().contains("projection.mode"));
    EXPECT_EQ(service.capabilities().at("projection.mode.current"), "pairing_only");
    EXPECT_EQ(service.set_projection_mode(owner, "projection_ready").error().code(),
              ErrorCode::ipc_consumer_rejected);
    const auto registered = service.register_consumer(owner, name);
    ASSERT_TRUE(registered);
    for (const auto& outsider : {same_uid, wrong_uid}) {
        EXPECT_EQ(service.set_projection_mode(outsider, "projection_ready").error().code(),
                  ErrorCode::ipc_peer_unauthorized);
        EXPECT_EQ(service.set_transport_enabled(outsider, "wired", false).error().code(),
                  ErrorCode::ipc_peer_unauthorized);
    }
    EXPECT_FALSE(service.start_projection(owner));
    EXPECT_FALSE(service.set_projection_mode(owner, "projection_ready"));
    EXPECT_FALSE(policy.ready);
    provisioned = true;
    ASSERT_TRUE(service.set_projection_mode(owner, "projection_ready"));
    ASSERT_TRUE(service.set_projection_mode(owner, "projection_ready"));
    ASSERT_TRUE(service.set_transport_enabled(owner, "wired", false));
    EXPECT_EQ(service.capabilities().at("transport.wired.enabled"), "false");
    EXPECT_EQ(service.capabilities().at("transport.wireless.enabled"), "true");
    ASSERT_TRUE(service.start_projection(owner));
    policy.active_transport = aa::ipc::ProjectionTransport::wireless;

    ASSERT_TRUE(service.set_transport_enabled(owner, "wired", true));
    EXPECT_EQ(service.state().projection, ProjectionState::projecting);
    ASSERT_TRUE(service.set_transport_enabled(owner, "wireless", false));
    EXPECT_EQ(service.state().projection, ProjectionState::stopped);
    EXPECT_EQ(service.state().consumer, registered.value());

    ASSERT_TRUE(service.start_projection(owner));
    ASSERT_TRUE(service.set_projection_mode(owner, "pairing_only"));
    EXPECT_EQ(service.state().projection, ProjectionState::stopped);
    EXPECT_EQ(service.state().consumer, registered.value());

    ASSERT_TRUE(service.set_projection_mode(owner, "projection_ready"));
    ASSERT_TRUE(service.set_transport_enabled(owner, "wired", false));
    EXPECT_FALSE(service.start_projection(owner));
}

TEST(ControlService, StopProjectionRetiresStandaloneWiredLinkWithoutPriorStart) {
    Fixture fixture;
    bool link_live = true;
    ControlService service{{kSessionUid}, {fixture.credentials, fixture.events, fixture.ids,
                                          fixture.phones, nullptr, {}, {}, nullptr,
                                          [&] { return std::exchange(link_live, false); }}};
    const BusName owner{":1.61"};
    const BusName name{"org.custom.ConsumerA"};
    fixture.credentials.add_connection(owner, kSessionUid);
    fixture.credentials.set_owner(name, owner);

    EXPECT_FALSE(service.stop_projection(owner));
    EXPECT_TRUE(link_live);
    ASSERT_TRUE(service.register_consumer(owner, name));
    ASSERT_TRUE(service.stop_projection(owner));
    EXPECT_FALSE(link_live);
    EXPECT_EQ(service.state().projection, ProjectionState::stopped);
    EXPECT_FALSE(service.stop_projection(owner));
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
    ASSERT_FALSE(forgotten.has_value());
    EXPECT_EQ(forgotten.error().code(), ErrorCode::helper_protocol_mismatch);
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

TEST(ControlService, SameNameReplacementReconcilesAbandonedOwner) {
    // Given: an incumbent whose consumer name is taken over by a replacement
    // connection (same-name ownership transfer, distinct from the removed-owner
    // variant above).
    Fixture fixture;
    const BusName incumbent{":1.120"};
    const BusName replacement{":1.121"};
    const BusName consumer_name{"org.custom.ConsumerA"};
    fixture.credentials.add_connection(incumbent, kSessionUid);
    fixture.credentials.add_connection(replacement, kSessionUid);
    fixture.credentials.set_owner(consumer_name, incumbent);
    ASSERT_TRUE(fixture.service.register_consumer(incumbent, consumer_name).has_value());
    fixture.credentials.set_owner(consumer_name, replacement);

    // When: the replacement registers the same consumer name.
    const auto registered = fixture.service.register_consumer(replacement, consumer_name);

    // Then: the abandoned incumbent is torn down and the replacement registers
    // under a fresh consumer id.
    ASSERT_TRUE(registered.has_value());
    EXPECT_EQ(fixture.service.state().consumer, registered.value());
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

TEST(ControlService, RequestIdrRecoveryRequiresProjectingActiveConsumer) {
    Fixture fixture;
    const BusName connection{":1.109"};
    const BusName consumer_name{"org.custom.ConsumerA"};
    const BusName intruder{":1.110"};
    const BusName intruder_name{"org.custom.ConsumerB"};
    fixture.credentials.add_connection(connection, kSessionUid);
    fixture.credentials.set_owner(consumer_name, connection);
    fixture.credentials.add_connection(intruder, kSessionUid);
    fixture.credentials.set_owner(intruder_name, intruder);

    const auto unregistered = fixture.service.request_idr_recovery(connection);
    ASSERT_FALSE(unregistered.has_value());
    EXPECT_EQ(unregistered.error().code(), ErrorCode::ipc_consumer_rejected);
    EXPECT_FALSE(fixture.service.consume_idr_recovery_request());

    ASSERT_TRUE(fixture.service.register_consumer(connection, consumer_name).has_value());
    const auto while_stopped = fixture.service.request_idr_recovery(connection);
    ASSERT_FALSE(while_stopped.has_value());
    EXPECT_EQ(while_stopped.error().code(), ErrorCode::session_illegal_transition);
    EXPECT_FALSE(fixture.service.consume_idr_recovery_request());

    ASSERT_TRUE(fixture.service.start_projection(connection).has_value());
    const auto unauthorized = fixture.service.request_idr_recovery(intruder);
    ASSERT_FALSE(unauthorized.has_value());
    EXPECT_EQ(unauthorized.error().code(), ErrorCode::ipc_peer_unauthorized);
    EXPECT_FALSE(fixture.service.consume_idr_recovery_request());

    fixture.session_active = false;
    const auto event_count = fixture.events.events.size();
    const auto no_session = fixture.service.request_idr_recovery(connection);
    ASSERT_FALSE(no_session);
    EXPECT_EQ(no_session.error().code(), ErrorCode::session_illegal_transition);
    EXPECT_FALSE(fixture.service.consume_idr_recovery_request());
    EXPECT_EQ(fixture.events.events.size(), event_count);
    fixture.session_active = true;
    ASSERT_TRUE(fixture.service.request_idr_recovery(connection).has_value());
    EXPECT_TRUE(fixture.service.consume_idr_recovery_request());
    EXPECT_FALSE(fixture.service.consume_idr_recovery_request());

    ASSERT_TRUE(fixture.service.request_idr_recovery(connection).has_value());
    ASSERT_TRUE(fixture.service.stop_projection(connection).has_value());
    EXPECT_FALSE(fixture.service.consume_idr_recovery_request());
}

TEST(ControlService, ListPhonesRequiresActiveConsumerAndForwardsDirectory) {
    Fixture fixture;
    const BusName owner{":1.130"};
    const BusName owner_name{"org.custom.ConsumerA"};
    const BusName other{":1.131"};
    const BusName stranger{":1.132"};
    fixture.credentials.add_connection(owner, kSessionUid);
    fixture.credentials.add_connection(other, kSessionUid);
    fixture.credentials.add_connection(stranger, kSessionUid + 1);
    fixture.credentials.set_owner(owner_name, owner);

    const auto unregistered = fixture.service.list_phones(owner);
    ASSERT_FALSE(unregistered.has_value());
    EXPECT_EQ(unregistered.error().code(), ErrorCode::ipc_consumer_rejected);

    ASSERT_TRUE(fixture.service.register_consumer(owner, owner_name).has_value());
    const auto wrong_uid = fixture.service.list_phones(stranger);
    ASSERT_FALSE(wrong_uid.has_value());
    EXPECT_EQ(wrong_uid.error().code(), ErrorCode::ipc_peer_unauthorized);
    const auto same_uid_other = fixture.service.list_phones(other);
    ASSERT_FALSE(same_uid_other.has_value());
    EXPECT_EQ(same_uid_other.error().code(), ErrorCode::ipc_peer_unauthorized);

    fixture.phones.listed_phones = std::vector<std::map<std::string, std::string>>{
        {{"phone_id", "1"},
         {"label", "id-0123456789abcdef"},
         {"transport", "wired"},
         {"strength", "serial-backed"}},
        {{"phone_id", "2"},
         {"label", "id-fedcba9876543210"},
         {"transport", "wireless"},
         {"strength", "bond-backed"}},
    };
    const auto listed = fixture.service.list_phones(owner);
    ASSERT_TRUE(listed.has_value());
    ASSERT_EQ(listed.value().size(), 2U);
    EXPECT_EQ(listed.value()[0].at("phone_id"), "1");
    EXPECT_EQ(listed.value()[1].at("transport"), "wireless");

    fixture.phones.listed_phones = aa::Error{ErrorCode::trust_store_io};
    const auto unreadable = fixture.service.list_phones(owner);
    ASSERT_FALSE(unreadable.has_value());
    EXPECT_EQ(unreadable.error().code(), ErrorCode::trust_store_io);
}

TEST(ControlService, GetPairingOutcomeTracksLifecycleAuthorizationAndTerminalSticky) {
    Fixture fixture;
    const BusName owner{":1.140"};
    const BusName owner_name{"org.custom.ConsumerA"};
    const BusName other{":1.141"};
    const BusName stranger{":1.142"};
    fixture.credentials.add_connection(owner, kSessionUid);
    fixture.credentials.add_connection(other, kSessionUid);
    fixture.credentials.add_connection(stranger, kSessionUid + 1);
    fixture.credentials.set_owner(owner_name, owner);

    EXPECT_EQ(fixture.service.pairing_outcome(owner, aa::core::PairingRequestId{1}).error().code(),
              ErrorCode::ipc_consumer_rejected);

    ASSERT_TRUE(fixture.service.register_consumer(owner, owner_name).has_value());
    EXPECT_EQ(fixture.service.pairing_outcome(owner, aa::core::PairingRequestId{}).error().code(),
              ErrorCode::ipc_malformed_request);
    EXPECT_EQ(fixture.service.pairing_outcome(owner, aa::core::PairingRequestId{999}).error().code(),
              ErrorCode::ipc_malformed_request);

    const auto approved_req = fixture.service.request_add_phone(owner);
    ASSERT_TRUE(approved_req.has_value());
    EXPECT_EQ(fixture.service.pairing_outcome(stranger, approved_req.value()).error().code(),
              ErrorCode::ipc_peer_unauthorized);
    EXPECT_EQ(fixture.service.pairing_outcome(other, approved_req.value()).error().code(),
              ErrorCode::ipc_peer_unauthorized);

    const auto pending = fixture.service.pairing_outcome(owner, approved_req.value());
    ASSERT_TRUE(pending.has_value());
    EXPECT_EQ(pending.value(), "pending");

    ASSERT_TRUE(fixture.service.confirm_phone_pairing(owner, approved_req.value()).has_value());
    const auto approved = fixture.service.pairing_outcome(owner, approved_req.value());
    ASSERT_TRUE(approved.has_value());
    EXPECT_EQ(approved.value(), "approved");

    // Post-approval window closure must not overwrite the terminal "approved" outcome.
    ASSERT_TRUE(fixture.service.cancel_phone_pairing(owner, approved_req.value()).has_value());
    EXPECT_EQ(fixture.service.pairing_outcome(owner, approved_req.value()).value(), "approved");

    const auto cancelled_req = fixture.service.request_add_phone(owner);
    ASSERT_TRUE(cancelled_req.has_value());
    ASSERT_TRUE(fixture.service.cancel_phone_pairing(owner, cancelled_req.value()).has_value());
    EXPECT_EQ(fixture.service.pairing_outcome(owner, cancelled_req.value()).value(), "cancelled");

    const auto expired_req = fixture.service.request_add_phone(owner);
    ASSERT_TRUE(expired_req.has_value());
    fixture.phones.reapable.push_back(expired_req.value());
    EXPECT_EQ(fixture.service.pairing_outcome(owner, expired_req.value()).value(), "expired");

    const auto teardown_req = fixture.service.request_add_phone(owner);
    ASSERT_TRUE(teardown_req.has_value());
    ASSERT_TRUE(fixture.service.unregister_consumer(owner).has_value());

    // A new consumer cannot query the previous consumer's request id.
    const BusName next_name{"org.custom.ConsumerB"};
    fixture.credentials.set_owner(next_name, other);
    ASSERT_TRUE(fixture.service.register_consumer(other, next_name).has_value());
    EXPECT_EQ(fixture.service.pairing_outcome(other, approved_req.value()).error().code(),
              ErrorCode::ipc_peer_unauthorized);
    EXPECT_EQ(fixture.service.pairing_outcome(other, teardown_req.value()).error().code(),
              ErrorCode::ipc_peer_unauthorized);

    // Confirming with a failing directory yields "failed" and emits PairingCompleted.
    const auto failed_req = fixture.service.request_add_phone(other);
    ASSERT_TRUE(failed_req.has_value());
    fixture.phones.confirm_result = aa::Error{ErrorCode::trust_store_io};
    EXPECT_FALSE(fixture.service.confirm_phone_pairing(other, failed_req.value()).has_value());
    EXPECT_EQ(fixture.service.pairing_outcome(other, failed_req.value()).value(), "failed");

    std::size_t completed_count = 0;
    for (const auto& event : fixture.events.events) {
        if (const auto* completed = std::get_if<aa::ipc::PairingCompleted>(&event)) {
            if (completed->request == approved_req.value()) {
                EXPECT_EQ(completed->outcome, "approved");
                ++completed_count;
            }
            if (completed->request == teardown_req.value()) {
                EXPECT_EQ(completed->outcome, "cancelled");
                ++completed_count;
            }
            if (completed->request == failed_req.value()) {
                EXPECT_EQ(completed->outcome, "failed");
                ++completed_count;
            }
        }
    }
    EXPECT_EQ(completed_count, 3U);
}

} // namespace
