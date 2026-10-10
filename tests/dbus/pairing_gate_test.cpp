// SPDX-License-Identifier: GPL-3.0-or-later
// Task 21 one-time pairing request-id gate: RequestAddPhone allocates a bound
// one-time id, the pairing window opens only on the same consumer's matching
// ConfirmPhonePairing, and reuse/unknown ids are typed errors. The service
// level covers the plan QA case "RequestAddPhone without matching
// ConfirmPhonePairing opens no window" and unauthorized pairing requests.
#include <aa/ipc/ControlService.hpp>
#include <aa/ipc/PairingGate.hpp>

#include "fakes.hpp"

#include <cstdint>

#include <gtest/gtest.h>

namespace {

using aa::ErrorCode;
using aa::ipc::BoundConsumer;
using aa::ipc::BusName;
using aa::ipc::ControlService;
using aa::ipc::ControlServiceConfig;
using aa::ipc::ControlServiceDeps;
using aa::ipc::PairingGate;
using aa::ipc::test::SequencePairingIds;

const BoundConsumer kConsumerA{aa::core::ConsumerId{1}, BusName{"org.custom.ConsumerA"}};
const BoundConsumer kConsumerB{aa::core::ConsumerId{2}, BusName{"org.custom.ConsumerB"}};

class CyclingPairingIds final : public aa::ipc::PairingIdSource {
public:
    explicit CyclingPairingIds(std::uint64_t period) : period_(period) {}

    [[nodiscard]] aa::core::PairingRequestId next() override {
        return aa::core::PairingRequestId{(next_++ % period_) + 1};
    }

private:
    std::uint64_t period_;
    std::uint64_t next_{0};
};

TEST(PairingGate, RetiredIdsAreNeverReadmittedByACyclingSource) {
    // Given: more than eight ids issued and retired from a source that cycles.
    CyclingPairingIds ids{9};
    PairingGate gate(ids);
    for (std::size_t index = 0; index < 9; ++index) {
        const auto request = gate.request(kConsumerA);
        ASSERT_TRUE(request.has_value());
        ASSERT_TRUE(gate.cancel(kConsumerA, request.value()).has_value());
    }
    EXPECT_FALSE(gate.window_open());

    // When: the source re-emits the first id and a stale confirm arrives for it.
    const auto reissued = gate.request(kConsumerA);
    const auto stale_confirm = gate.confirm(kConsumerA, aa::core::PairingRequestId{1});

    // Then: lifetime one-time semantics reject both; no window ever opens.
    ASSERT_FALSE(reissued.has_value());
    EXPECT_EQ(reissued.error().code(), ErrorCode::internal);
    ASSERT_FALSE(stale_confirm.has_value());
    EXPECT_EQ(stale_confirm.error().code(), ErrorCode::ipc_consumer_rejected);
    EXPECT_FALSE(gate.window_open());
}

TEST(PairingGate, IssuedBudgetFailsClosedUntilRestart) {
    // Given: the service-lifetime admission budget fully spent by distinct
    // retired ids (the pending table is empty again).
    SequencePairingIds ids{1};
    PairingGate gate(ids);
    for (std::size_t index = 0; index < PairingGate::kIssuedBudget; ++index) {
        const auto request = gate.request(kConsumerA);
        ASSERT_TRUE(request.has_value());
        ASSERT_TRUE(gate.cancel(kConsumerA, request.value()).has_value());
    }

    // When: one more fresh id is requested.
    const auto exhausted = gate.request(kConsumerA);

    // Then: the gate fails closed with no new admissions until restart.
    ASSERT_FALSE(exhausted.has_value());
    EXPECT_EQ(exhausted.error().code(), ErrorCode::ipc_queue_full);
}

TEST(PairingGate, RequestAllocatesIdsFromTheSourceAndBindsConsumer) {
    // Given: a gate with a deterministic id source.
    SequencePairingIds ids{500};
    PairingGate gate(ids);

    // When: one request is made.
    const auto request = gate.request(kConsumerA);

    // Then: the id comes from the source and nothing is open yet.
    ASSERT_TRUE(request.has_value());
    EXPECT_EQ(request.value(), aa::core::PairingRequestId{500});
    EXPECT_FALSE(gate.window_open());
}

TEST(PairingGate, ConfirmOpensWindowOnlyForTheBoundConsumer) {
    // Given: a pending request bound to consumer A.
    SequencePairingIds ids;
    PairingGate gate(ids);
    const auto request = gate.request(kConsumerA);
    ASSERT_TRUE(request.has_value());

    // When: consumer B confirms.
    const auto denied = gate.confirm(kConsumerB, request.value());

    // Then: cross-consumer confirm is unauthorized and no window opened.
    ASSERT_FALSE(denied.has_value());
    EXPECT_EQ(denied.error().code(), ErrorCode::ipc_peer_unauthorized);
    EXPECT_FALSE(gate.window_open());

    // When: the bound consumer confirms.
    const auto opened = gate.confirm(kConsumerA, request.value());

    // Then: the bound consumer opens the window.
    ASSERT_TRUE(opened.has_value());
    EXPECT_TRUE(gate.window_open());
    EXPECT_EQ(gate.open_request(), request.value());
}

TEST(PairingGate, UnknownRequestIdIsMalformedAndSpentIdReuseIsRejected) {
    // Given: one request that is cancelled (spending its id).
    SequencePairingIds ids;
    PairingGate gate(ids);
    const auto request = gate.request(kConsumerA);
    ASSERT_TRUE(request.has_value());
    ASSERT_TRUE(gate.cancel(kConsumerA, request.value()).has_value());

    // When: the unknown id and then the spent id are confirmed.
    const auto unknown = gate.confirm(kConsumerA, aa::core::PairingRequestId{999});
    const auto reused = gate.confirm(kConsumerA, request.value());

    // Then: each misuse gets its own typed error and no window opens.
    ASSERT_FALSE(unknown.has_value());
    EXPECT_EQ(unknown.error().code(), ErrorCode::ipc_malformed_request);
    ASSERT_FALSE(reused.has_value());
    EXPECT_EQ(reused.error().code(), ErrorCode::ipc_consumer_rejected);
    EXPECT_FALSE(gate.window_open());
}

TEST(PairingGate, PendingTableIsBounded) {
    // Given: a gate filled to capacity.
    SequencePairingIds ids;
    PairingGate gate(ids);
    for (std::size_t index = 0; index < PairingGate::kMaxPending; ++index) {
        ASSERT_TRUE(gate.request(kConsumerA).has_value());
    }

    // When: one more request arrives.
    const auto overflow = gate.request(kConsumerA);

    // Then: the table rejects it with the typed queue error.
    ASSERT_FALSE(overflow.has_value());
    EXPECT_EQ(overflow.error().code(), ErrorCode::ipc_queue_full);
}

TEST(PairingGate, CloseAllClosesWindowAndReturnsEveryId) {
    // Given: two pending requests with one confirmed.
    SequencePairingIds ids;
    PairingGate gate(ids);
    const auto first = gate.request(kConsumerA);
    const auto second = gate.request(kConsumerA);
    ASSERT_TRUE(first.has_value());
    ASSERT_TRUE(second.has_value());
    ASSERT_TRUE(gate.confirm(kConsumerA, first.value()).has_value());

    // When: everything closes.
    const auto closed = gate.close_all();

    // Then: the window is shut and both ids are reported closed once.
    EXPECT_FALSE(gate.window_open());
    EXPECT_EQ(closed.size(), 2U);
    EXPECT_EQ(closed[0], first.value());
    EXPECT_EQ(closed[1], second.value());
}

TEST(PairingFlow, RequestWithoutConfirmOpensNoWindow) {
    // Given: a registered consumer.
    aa::ipc::test::FakePeerCredentials credentials;
    aa::ipc::test::RecordingEventSink events;
    SequencePairingIds ids{700};
    aa::ipc::test::RecordingPhoneDirectory phones;
    ControlService service(ControlServiceConfig{1000}, ControlServiceDeps{credentials, events, ids, phones});
    const BusName connection{":1.100"};
    const BusName consumer_name{"org.custom.ConsumerA"};
    credentials.add_connection(connection, 1000);
    credentials.set_owner(consumer_name, connection);
    ASSERT_TRUE(service.register_consumer(connection, consumer_name).has_value());

    // When: the consumer requests add-phone and never confirms.
    const auto request = service.request_add_phone(connection);

    // Then: exactly one PairingRequest id exists and NO window ever opens.
    ASSERT_TRUE(request.has_value());
    EXPECT_EQ(request.value(), aa::core::PairingRequestId{700});
    EXPECT_EQ(events.count_requested(), 1U);
    EXPECT_EQ(events.count_opened(), 0U);
    EXPECT_FALSE(service.pairing_window_open());

    // When: the same consumer confirms that same id.
    ASSERT_TRUE(service.confirm_phone_pairing(connection, request.value()).has_value());

    // Then: the window opens exactly once with the matching id.
    EXPECT_TRUE(service.pairing_window_open());
    EXPECT_EQ(events.count_opened(), 1U);
    EXPECT_EQ(events.opened_requests(), std::vector<aa::core::PairingRequestId>{request.value()});
}

TEST(PairingFlow, UnauthorizedPairingRequestsAreRejected) {
    // Given: a registered consumer and an unregistered same-UID caller.
    aa::ipc::test::FakePeerCredentials credentials;
    aa::ipc::test::RecordingEventSink events;
    SequencePairingIds ids;
    aa::ipc::test::RecordingPhoneDirectory phones;
    ControlService service(ControlServiceConfig{1000}, ControlServiceDeps{credentials, events, ids, phones});
    const BusName connection{":1.110"};
    const BusName consumer_name{"org.custom.ConsumerA"};
    const BusName interloper{":1.111"};
    credentials.add_connection(connection, 1000);
    credentials.add_connection(interloper, 1000);
    credentials.set_owner(consumer_name, connection);
    ASSERT_TRUE(service.register_consumer(connection, consumer_name).has_value());
    const auto request = service.request_add_phone(connection);
    ASSERT_TRUE(request.has_value());

    // When: the interloper confirms and cancels the pending request.
    const auto denied_confirm = service.confirm_phone_pairing(interloper, request.value());
    const auto denied_cancel = service.cancel_phone_pairing(interloper, request.value());

    // Then: every unauthorized pairing request fails and no window opens.
    ASSERT_FALSE(denied_confirm.has_value());
    EXPECT_EQ(denied_confirm.error().code(), ErrorCode::ipc_peer_unauthorized);
    ASSERT_FALSE(denied_cancel.has_value());
    EXPECT_EQ(denied_cancel.error().code(), ErrorCode::ipc_peer_unauthorized);
    EXPECT_FALSE(service.pairing_window_open());
    EXPECT_EQ(events.count_opened(), 0U);
}

TEST(PairingFlow, CancelSpendsTheIdWithoutOpeningTheWindow) {
    // Given: a registered consumer with a pending request.
    aa::ipc::test::FakePeerCredentials credentials;
    aa::ipc::test::RecordingEventSink events;
    SequencePairingIds ids;
    aa::ipc::test::RecordingPhoneDirectory phones;
    ControlService service(ControlServiceConfig{1000}, ControlServiceDeps{credentials, events, ids, phones});
    const BusName connection{":1.120"};
    const BusName consumer_name{"org.custom.ConsumerA"};
    credentials.add_connection(connection, 1000);
    credentials.set_owner(consumer_name, connection);
    ASSERT_TRUE(service.register_consumer(connection, consumer_name).has_value());
    const auto request = service.request_add_phone(connection);
    ASSERT_TRUE(request.has_value());

    // When: the consumer cancels and then tries to confirm the spent id.
    ASSERT_TRUE(service.cancel_phone_pairing(connection, request.value()).has_value());
    const auto late_confirm = service.confirm_phone_pairing(connection, request.value());

    // Then: no window ever opened and the spent id is rejected.
    EXPECT_FALSE(service.pairing_window_open());
    EXPECT_EQ(events.count_opened(), 0U);
    ASSERT_FALSE(late_confirm.has_value());
    EXPECT_EQ(late_confirm.error().code(), ErrorCode::ipc_consumer_rejected);
}

TEST(PairingFlow, ConfirmRejectsUnknownIdsAsTypedErrors) {
    // Given: a registered consumer.
    aa::ipc::test::FakePeerCredentials credentials;
    aa::ipc::test::RecordingEventSink events;
    SequencePairingIds ids;
    aa::ipc::test::RecordingPhoneDirectory phones;
    ControlService service(ControlServiceConfig{1000}, ControlServiceDeps{credentials, events, ids, phones});
    const BusName connection{":1.130"};
    const BusName consumer_name{"org.custom.ConsumerA"};
    credentials.add_connection(connection, 1000);
    credentials.set_owner(consumer_name, connection);
    ASSERT_TRUE(service.register_consumer(connection, consumer_name).has_value());

    // When: an id that was never issued is confirmed.
    const auto unknown = service.confirm_phone_pairing(connection, aa::core::PairingRequestId{42});

    // Then: it is a malformed request and no window opens.
    ASSERT_FALSE(unknown.has_value());
    EXPECT_EQ(unknown.error().code(), ErrorCode::ipc_malformed_request);
    EXPECT_FALSE(service.pairing_window_open());
}

} // namespace
