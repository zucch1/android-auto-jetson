// SPDX-License-Identifier: GPL-3.0-or-later
#include "confirmation_fixture.hpp"
#include <aa/app/WirelessConfirmation.hpp>
#include "../trust/syscall_faults.hpp"

namespace {
struct ComparisonClock final {
    static inline ComparisonClock* active{};
    std::chrono::steady_clock::time_point now{std::chrono::seconds{1000}};
    int reads_until_expiry{-1};
    int expiry_transitions{};
    ComparisonClock() { active = this; }
    ~ComparisonClock() { active = nullptr; }
};
}

std::chrono::steady_clock::time_point real_steady_now() noexcept
    asm("__real__ZNSt6chrono3_V212steady_clock3nowEv");
std::chrono::steady_clock::time_point wrapped_steady_now() noexcept
    asm("__wrap__ZNSt6chrono3_V212steady_clock3nowEv");
std::chrono::steady_clock::time_point wrapped_steady_now() noexcept {
    auto* clock = ComparisonClock::active;
    if (!clock) return real_steady_now();
    if (clock->reads_until_expiry == 0) {
        clock->now += std::chrono::seconds{60};
        clock->reads_until_expiry = -1;
        ++clock->expiry_transitions;
    } else if (clock->reads_until_expiry > 0) {
        --clock->reads_until_expiry;
    }
    return clock->now;
}

namespace {
using namespace aa;
class ComparisonControl : public helper::test::HelperConfirmation {
protected:
    void SetUp() override {
        HelperConfirmation::SetUp();
        bridge.prepared = helper::PeerIdentity{"02:00:00:00:00:01", "02:00:00:00:00:AA"};
        bridge.comparison_required = true;
        bridge.challenge = helper::PairingComparison{79, 42, *bridge.prepared};
    }
    void expect_empty() {
        EXPECT_TRUE(store.phones().empty()); EXPECT_EQ(phones.pending_count(), 0U);
        trust::ApprovedPhoneStore reopened{directory / "store"}; ASSERT_TRUE(reopened.load());
        EXPECT_TRUE(reopened.phones().empty());
        EXPECT_TRUE(std::any_of(events.events.begin(), events.events.end(), [](const auto& event) {
            const auto* diagnostic = std::get_if<ipc::DiagnosticEmitted>(&event);
            return diagnostic && (diagnostic->event == "pairing_comparison_rejected" ||
                diagnostic->event == "pairing_closed" || diagnostic->event == "pairing_expired" ||
                diagnostic->event == "consumer_owner_lost" || diagnostic->event == "consumer_unregistered" ||
                diagnostic->event == "pairing_snapshot_invalidated" || diagnostic->event == "pairing_helper_failed");
        }));
    }
    core::PairingRequestId enroll() {
        const auto request = control.request_add_phone(caller);
        EXPECT_TRUE(request);
        if (!request) return {};
        EXPECT_TRUE(control.confirm_phone_pairing(caller, request.value()));
        return request.value();
    }
};
TEST_F(ComparisonControl, UnseenNumberCannotBeApprovedByEnrollmentConfirmation) {
    // Given: a pending identity and an open enrollment, but no number retrieved.
    const auto request = enroll();
    // When: the consumer tries to approve the challenge without presenting it.
    EXPECT_FALSE(control.respond_pairing_comparison(caller, request, {79, true}));
    // Then: nothing reaches the helper response seam and store semantics stay intact.
    EXPECT_EQ(bridge.responses, 0); EXPECT_TRUE(store.phones().empty());
}
TEST_F(ComparisonControl, RetrievedNumberHasSixDigitsAndBindsOriginalSnapshot) {
    const auto request = enroll();
    phones.note_candidate(trust::WirelessIdentity{"other", "adapter"});
    const auto challenge = control.pairing_comparison(caller, request);
    ASSERT_TRUE(challenge);
    EXPECT_EQ(challenge.value().at("comparison"), "000042");
    EXPECT_EQ(challenge.value().at("address"), bridge.prepared->address);
}
TEST_F(ComparisonControl, ExpiryDuringFinalConsumerReconciliationReturnsNoChallenge) {
    // Given: a live enrollment; keep the initial reconciliation and both validations live.
    ComparisonClock clock;
    const auto request = enroll();
    clock.reads_until_expiry = 3;

    // When: the final require_consumer() reads exactly the enrollment deadline.
    const auto challenge = control.pairing_comparison(caller, request);

    // Then: expiry closes enrollment without returning or presenting a challenge.
    ASSERT_FALSE(challenge);
    EXPECT_EQ(challenge.error().code(), ErrorCode::trust_rejected);
    EXPECT_EQ(clock.expiry_transitions, 1);
    EXPECT_EQ(bridge.cancellations, 1);
    EXPECT_EQ(count_closed(), 1U);
    EXPECT_EQ(control.state().consumer.value, 1U);
    EXPECT_EQ(bridge.teardowns, 0);
    EXPECT_EQ(bridge.responses, 0);
}
TEST_F(ComparisonControl, ExpiryDuringFinalConsumerReconciliationCannotApproveChallenge) {
    // Given: a presented challenge; keep initial response validation live.
    ComparisonClock clock;
    const auto request = enroll();
    ASSERT_TRUE(control.pairing_comparison(caller, request));
    clock.reads_until_expiry = 3;

    // When: the final require_consumer() expires enrollment before answering.
    const auto responded = control.respond_pairing_comparison(caller, request, {79, true});

    // Then: no acceptance reaches the helper and enrollment closes without consumer teardown.
    ASSERT_FALSE(responded);
    EXPECT_EQ(responded.error().code(), ErrorCode::trust_rejected);
    EXPECT_EQ(clock.expiry_transitions, 1);
    EXPECT_EQ(bridge.responses, 0);
    EXPECT_EQ(count_closed(), 1U);
    EXPECT_EQ(control.state().consumer.value, 1U);
    EXPECT_EQ(bridge.teardowns, 0);
}
TEST_F(ComparisonControl, ProductionWirelessConfirmationForwardsComparison) {
    // Given: the production receiver adapter and control service share the same fixture consumer.
    app::WirelessConfirmation adapter{bridge, [] { return false; }, [](core::ConsumerId) {}};
    ipc::ControlService production{{1000}, {credentials, events, ids, phones, &adapter}};
    ASSERT_TRUE(production.register_consumer(caller, name));
    const auto request = production.request_add_phone(caller);
    ASSERT_TRUE(request);
    ASSERT_TRUE(production.confirm_phone_pairing(caller, request.value()));

    // When: the consumer retrieves and accepts through the production adapter.
    const auto challenge = production.pairing_comparison(caller, request.value());
    ASSERT_TRUE(challenge);
    const auto responded = production.respond_pairing_comparison(caller, request.value(), {79, true});

    // Then: both calls reach the helper bridge exactly once.
    EXPECT_TRUE(responded);
    EXPECT_EQ(challenge.value().at("comparison"), "000042");
    EXPECT_EQ(bridge.responses, 1);
    EXPECT_TRUE(bridge.response.accept);
}
TEST_F(ComparisonControl, ExplicitHumanResponseIsBoundAndConsumedOnce) {
    const auto request = enroll();
    ASSERT_TRUE(control.pairing_comparison(caller, request));
    EXPECT_TRUE(control.respond_pairing_comparison(caller, request, {79, true}));
    EXPECT_EQ(bridge.responses, 1); EXPECT_TRUE(bridge.response.accept);
    EXPECT_EQ(bridge.response_request, request); EXPECT_EQ(bridge.response_generation.value, 1U);
    EXPECT_FALSE(control.respond_pairing_comparison(caller, request, {79, true}));
    EXPECT_EQ(bridge.responses, 1);
}
TEST_F(ComparisonControl, RogueCallerAndWrongRequestOrTokenCannotAnswer) {
    const auto request = enroll();
    EXPECT_FALSE(control.pairing_comparison(rogue, request));
    ASSERT_TRUE(control.pairing_comparison(caller, request));
    EXPECT_FALSE(control.respond_pairing_comparison(rogue, request, {79, true}));
    EXPECT_FALSE(control.respond_pairing_comparison(caller, core::PairingRequestId{request.value + 1}, {79, true}));
    EXPECT_FALSE(control.respond_pairing_comparison(caller, request, {80, true}));
    EXPECT_EQ(bridge.responses, 0);
}
TEST_F(ComparisonControl, HumanRejectionIsNotConvertedToAcceptance) {
    const auto request = enroll();
    ASSERT_TRUE(control.pairing_comparison(caller, request));
    EXPECT_TRUE(control.respond_pairing_comparison(caller, request, {79, false}));
    EXPECT_EQ(bridge.responses, 1); EXPECT_FALSE(bridge.response.accept);
}
TEST_F(ComparisonControl, PeerSwapBetweenPresentationAndResponseFailsClosed) {
    const auto request = enroll();
    ASSERT_TRUE(control.pairing_comparison(caller, request));
    bridge.challenge->peer.address = "02:00:00:00:00:02";
    EXPECT_FALSE(control.respond_pairing_comparison(caller, request, {79, true}));
    EXPECT_EQ(bridge.responses, 0); EXPECT_EQ(bridge.cancellations, 1);
}
TEST_F(ComparisonControl, WrongPeerCannotBePresented) {
    const auto request = enroll();
    bridge.challenge->peer.adapter = "02:00:00:00:00:BB";
    EXPECT_FALSE(control.pairing_comparison(caller, request));
    EXPECT_EQ(bridge.responses, 0); EXPECT_EQ(bridge.cancellations, 1);
}
TEST_F(ComparisonControl, ConsumerLossAndReregistrationCannotAnswerOldChallenge) {
    const auto request = enroll();
    ASSERT_TRUE(control.pairing_comparison(caller, request));
    credentials.remove_owner(name);
    EXPECT_FALSE(control.respond_pairing_comparison(caller, request, {79, true}));
    EXPECT_EQ(bridge.teardowns, 1);
    credentials.set_owner(name, caller);
    ASSERT_TRUE(control.register_consumer(caller, name));
    EXPECT_FALSE(control.pairing_comparison(caller, request));
    EXPECT_FALSE(control.respond_pairing_comparison(caller, request, {79, true}));
    EXPECT_EQ(bridge.responses, 0);
}
TEST_F(ComparisonControl, CancelledRequestCannotAnswerAndDiagnosticsContainNoNumber) {
    const auto request = enroll();
    ASSERT_TRUE(control.pairing_comparison(caller, request));
    ASSERT_TRUE(control.cancel_phone_pairing(caller, request));
    EXPECT_FALSE(control.respond_pairing_comparison(caller, request, {79, true}));
    EXPECT_EQ(bridge.responses, 0);
    for (const auto& event : events.events) {
        if (const auto* diagnostic = std::get_if<ipc::DiagnosticEmitted>(&event)) {
            EXPECT_NE(diagnostic->subject, 42U); EXPECT_NE(diagnostic->subject, 79U);
        }
    }
}
TEST_F(ComparisonControl, RejectionLeavesNoApprovalOrPending) {
    const auto request = enroll();
    ASSERT_TRUE(control.pairing_comparison(caller, request));
    ASSERT_TRUE(control.respond_pairing_comparison(caller, request, {79, false}));
    expect_empty();
    EXPECT_EQ(bridge.cancellations, 1); EXPECT_EQ(count_closed(), 1U);
}
TEST_F(ComparisonControl, CancellationLeavesNoApprovalOrPending) {
    const auto request = enroll();
    ASSERT_TRUE(control.cancel_phone_pairing(caller, request));
    expect_empty();
    EXPECT_EQ(bridge.cancellations, 1); EXPECT_EQ(count_closed(), 1U);
}
TEST_F(ComparisonControl, TimeoutLeavesNoApprovalOrPending) {
    ComparisonClock clock;
    enroll(); clock.now += std::chrono::seconds{61}; (void)control.state();
    expect_empty();
    EXPECT_EQ(bridge.cancellations, 1); EXPECT_EQ(count_closed(), 1U);
}
TEST_F(ComparisonControl, DisconnectLeavesNoApprovalOrPending) {
    enroll(); credentials.remove_owner(name); (void)control.state();
    expect_empty();
    EXPECT_EQ(bridge.teardowns, 1); EXPECT_EQ(count_closed(), 1U);
}
TEST_F(ComparisonControl, ReceiverRestartLeavesNoApprovalOrPending) {
    enroll(); control.shutdown();
    trust::ApprovedPhoneStore reopened{directory / "store"}; ASSERT_TRUE(reopened.load());
    EXPECT_TRUE(reopened.phones().empty()); expect_empty();
    EXPECT_EQ(bridge.teardowns, 1); EXPECT_EQ(count_closed(), 1U);
}
TEST_F(ComparisonControl, StoreReloadInvalidatesPendingEnrollment) {
    const auto request = enroll(); ASSERT_TRUE(store.load()); (void)control.state();
    expect_empty();
    EXPECT_FALSE(control.pairing_comparison(caller, request)); EXPECT_EQ(count_closed(), 1U);
}
TEST_F(ComparisonControl, HelperFailureAfterAcceptanceLeavesNoApprovalOrPending) {
    const auto request = enroll(); ASSERT_TRUE(control.pairing_comparison(caller, request));
    bridge.fail_after_response = true;
    (void)control.respond_pairing_comparison(caller, request, {79, true});
    (void)control.state();
    expect_empty();
    EXPECT_EQ(bridge.cancellations, 1); EXPECT_EQ(count_closed(), 1U);
}

TEST_F(ComparisonControl, AcceptanceWaitsForBondAcknowledgementBeforePersistence) {
    bridge.bonded = false;
    const auto request = enroll(); ASSERT_TRUE(control.pairing_comparison(caller, request));
    ASSERT_TRUE(control.respond_pairing_comparison(caller, request, {79, true}));
    EXPECT_TRUE(store.phones().empty()); EXPECT_EQ(phones.pending_count(), 1U);
    bridge.bonded = true; (void)control.state();
    ASSERT_EQ(store.phones().size(), 1U); EXPECT_EQ(phones.pending_count(), 0U);
    EXPECT_FALSE(bridge.approved_at_call);
    EXPECT_FALSE(bridge.approved_at_bond);
    trust::ApprovedPhoneStore reopened{directory / "store"}; ASSERT_TRUE(reopened.load());
    EXPECT_EQ(reopened.phones().size(), 1U);
}

TEST_F(ComparisonControl, StoreReloadBeforeComparisonOpensRetiresRequest) {
    const auto request = control.request_add_phone(caller); ASSERT_TRUE(request);
    ASSERT_TRUE(store.load()); (void)control.state();
    expect_empty(); EXPECT_EQ(bridge.cancellations, 1); EXPECT_EQ(count_closed(), 1U);
    EXPECT_FALSE(control.confirm_phone_pairing(caller, request.value()));
}
TEST_F(ComparisonControl, TimeoutBeforeComparisonOpensRetiresRequest) {
    ComparisonClock clock;
    const auto request = control.request_add_phone(caller); ASSERT_TRUE(request);
    clock.now += std::chrono::seconds{61}; (void)control.state();
    expect_empty(); EXPECT_EQ(bridge.cancellations, 1); EXPECT_EQ(count_closed(), 1U);
    EXPECT_FALSE(control.confirm_phone_pairing(caller, request.value()));
}

// P2a: the helper records the managed bond only when the receiver asks it to acknowledge, after
// every receiver-side check; a failure afterwards is retired through the tracked revocation.
class AcknowledgementOrder : public ComparisonControl {
protected:
    ComparisonClock clock;
    core::PairingRequestId request;
    void SetUp() override {
        ComparisonControl::SetUp();
        request = enroll();
        ASSERT_TRUE(control.pairing_comparison(caller, request));
    }
    void accept() { (void)control.respond_pairing_comparison(caller, request, {79, true}); }
    void expect_retired_once() {
        EXPECT_EQ(bridge.forgets, 1);
        ASSERT_EQ(bridge.snapshots.size(), 1U);
        EXPECT_FALSE(bridge.snapshots[0].phone);
        const auto peer = helper::revocation_peer(bridge.snapshots[0]);
        ASSERT_TRUE(peer); EXPECT_EQ(*peer, *bridge.prepared);
        ASSERT_EQ(bridge.retirement_batches.size(), 1U);
        ASSERT_EQ(bridge.retirement_batches[0].size(), 1U);
        EXPECT_EQ(bridge.retirement_batches[0][0].request, request);
        EXPECT_EQ(control.pending_revocations(), 0U);
        EXPECT_TRUE(store.phones().empty());
    }
};
TEST_F(AcknowledgementOrder, DeadlineBeforeAcknowledgementRecordsNothing) {
    bridge.on_bond_confirmed = [&] { clock.now += std::chrono::seconds{61}; };
    accept();
    EXPECT_EQ(bridge.acknowledgements, 0);
    EXPECT_EQ(bridge.forgets, 0); EXPECT_EQ(control.pending_revocations(), 0U);
    EXPECT_EQ(bridge.cancellations, 1); EXPECT_EQ(count_closed(), 1U); EXPECT_TRUE(store.phones().empty());
}
TEST_F(AcknowledgementOrder, ConsumerLossBeforeAcknowledgementRecordsNothing) {
    bridge.on_bond_confirmed = [&] { credentials.remove_owner(name); };
    accept();
    EXPECT_EQ(bridge.acknowledgements, 0);
    EXPECT_EQ(bridge.forgets, 0); EXPECT_EQ(control.pending_revocations(), 0U);
    EXPECT_EQ(bridge.teardowns, 1); EXPECT_TRUE(store.phones().empty());
}
TEST_F(AcknowledgementOrder, StoreReloadBeforeAcknowledgementRecordsNothing) {
    bridge.on_bond_confirmed = [&] { ASSERT_TRUE(store.load()); };
    accept();
    EXPECT_EQ(bridge.acknowledgements, 0);
    EXPECT_EQ(bridge.forgets, 0); EXPECT_EQ(control.pending_revocations(), 0U);
    EXPECT_EQ(bridge.cancellations, 1); EXPECT_EQ(count_closed(), 1U); EXPECT_TRUE(store.phones().empty());
}
TEST_F(AcknowledgementOrder, FailedAcknowledgementRetiresPossiblyRecordedBond) {
    bridge.acknowledgement = Error{ErrorCode::timeout};
    accept();
    EXPECT_EQ(bridge.acknowledgements, 1);
    expect_retired_once();
}
TEST_F(AcknowledgementOrder, AcknowledgementHappensBeforeAnyStoreWrite) {
    accept();
    EXPECT_EQ(bridge.acknowledgements, 1); EXPECT_FALSE(bridge.approved_at_acknowledgement);
    EXPECT_EQ(store.phones().size(), 1U); EXPECT_EQ(bridge.forgets, 0);
}
TEST_F(AcknowledgementOrder, DeadlineAfterAcknowledgementIsRetiredThroughRevocation) {
    bridge.on_acknowledge = [&] { clock.now += std::chrono::seconds{61}; };
    accept();
    EXPECT_EQ(bridge.acknowledgements, 1);
    expect_retired_once();
}
TEST_F(AcknowledgementOrder, ConsumerLossAfterAcknowledgementIsRetiredThroughRevocation) {
    bridge.on_acknowledge = [&] { credentials.remove_owner(name); };
    accept();
    EXPECT_EQ(bridge.acknowledgements, 1);
    expect_retired_once();
}
TEST_F(AcknowledgementOrder, StoreWriteFailureAfterAcknowledgementLeadsToTrackedRetirement) {
    // Given: the bond is acknowledged, then the trust-store write fails; the bond removal also fails.
    bridge.bond = Error{ErrorCode::helper_unavailable};
    {
        trust::test::faults::ScopedFault failing{trust::test::faults::Fault::temp_fsync};
        accept();
    }
    // Then: nothing is stored, and the failed removal stays tracked in the existing revocation record.
    EXPECT_EQ(bridge.acknowledgements, 1);
    EXPECT_TRUE(store.phones().empty()); EXPECT_EQ(phones.pending_count(), 0U);
    EXPECT_EQ(control.pending_revocations(), 1U);
    EXPECT_EQ(bridge.bond_attempts, 1);
    // And the identity cannot be enrolled again while its bond is still not retired.
    const auto blocked = control.request_add_phone(caller);
    ASSERT_FALSE(blocked); EXPECT_EQ(blocked.error().code(), ErrorCode::trust_rejected);
    // When: the removal succeeds on a later owner reconciliation (paced, not on every call).
    bridge.bond = {};
    (void)control.state();
    EXPECT_EQ(bridge.bond_attempts, 1) << "retry must be paced";
    clock.now += std::chrono::seconds{3};
    (void)control.state();
    // Then: the bond is retired, the record is cleared and the phone can enrol again.
    EXPECT_EQ(bridge.bond_attempts, 2);
    EXPECT_EQ(control.pending_revocations(), 0U);
    EXPECT_TRUE(control.request_add_phone(caller));
}

// Acknowledged-then-failed enrolments leave bond-leg records. They share the bounded retry loop of
// source-only records: a stalled helper cannot block a control call, and everything retires once
// the helper recovers.
TEST_F(ComparisonControl, StalledHelperBondRetirementsAreBoundedPerCallAndAllRetireOnRecovery) {
    using namespace std::chrono_literals;
    constexpr int kRecords = 40;
    auto fake_now = std::chrono::steady_clock::time_point{std::chrono::seconds{1000}};
    ipc::ControlService budgeted{{1000}, {credentials, events, ids, phones, &bridge, [&] { return fake_now; }}};
    ASSERT_TRUE(budgeted.register_consumer(caller, name));
    bridge.bond = Error{ErrorCode::helper_unavailable};
    bridge.acknowledgement = Error{ErrorCode::timeout};
    for (int i = 0; i < kRecords; ++i) {
        char address[18]; std::snprintf(address, sizeof address, "02:00:00:00:10:%02X", i);
        bridge.prepared = helper::PeerIdentity{address, "02:00:00:00:00:AA"};
        bridge.challenge = helper::PairingComparison{79, 42, *bridge.prepared};
        const auto request = budgeted.request_add_phone(caller); ASSERT_TRUE(request);
        ASSERT_TRUE(budgeted.confirm_phone_pairing(caller, request.value()));
        ASSERT_TRUE(budgeted.pairing_comparison(caller, request.value()));
        (void)budgeted.respond_pairing_comparison(caller, request.value(), {79, true});
    }
    ASSERT_EQ(budgeted.pending_revocations(), static_cast<std::size_t>(kRecords));
    // A stalled helper: every attempt burns one second, so one control call makes one attempt.
    fake_now += 60s;
    bridge.on_forget = [&] { fake_now += 1s; };
    int before = bridge.forgets;
    (void)budgeted.state();
    EXPECT_EQ(bridge.forgets - before, 1) << "the time budget must stop the pass after the first stalled attempt";
    // A fast-failing helper: the attempt budget bounds a pass.
    bridge.on_forget = nullptr;
    fake_now += 60s;
    before = bridge.forgets;
    (void)budgeted.state();
    EXPECT_EQ(bridge.forgets - before, static_cast<int>(ipc::ControlService::kSourceRetirementAttemptBudget));
    // Recovery: every record retires; each pass stays within the budget.
    bridge.bond = {};
    int rounds = 0;
    while (budgeted.pending_revocations() != 0 && rounds++ < 3 * kRecords) {
        fake_now += 60s;
        before = bridge.forgets;
        (void)budgeted.state();
        EXPECT_LE(bridge.forgets - before, static_cast<int>(ipc::ControlService::kSourceRetirementAttemptBudget));
    }
    EXPECT_EQ(budgeted.pending_revocations(), 0U);
    EXPECT_LE(rounds, kRecords / 4 + 3) << "the round-robin cursor must reach every record";
}

}
