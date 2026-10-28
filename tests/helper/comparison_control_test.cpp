// SPDX-License-Identifier: GPL-3.0-or-later
#include "confirmation_fixture.hpp"

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
        bridge.challenge = helper::PairingComparison{79, 42, *bridge.prepared};
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
    // Given: an approved identity and an open enrollment, but no number retrieved.
    const auto request = enroll();
    // When: the consumer tries to approve the challenge without presenting it.
    EXPECT_FALSE(control.respond_pairing_comparison(caller, request, {79, true}));
    // Then: nothing reaches the helper response seam and store semantics stay intact.
    EXPECT_EQ(bridge.responses, 0); EXPECT_EQ(store.phones().size(), 1U);
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
}
