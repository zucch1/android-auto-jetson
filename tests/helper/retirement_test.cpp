// SPDX-License-Identifier: GPL-3.0-or-later
#include "confirmation_fixture.hpp"
#include "../trust/syscall_faults.hpp"

namespace {
using namespace aa;
using helper::test::HelperConfirmation;

TEST_F(HelperConfirmation, CommittedRetirementKeepsOnlyUnacknowledgedExactSourcesAcrossRetries) {
    // Given: two matching sources, one unrelated request, and a partial cleanup acknowledgement.
    const auto open = control.request_add_phone(caller); ASSERT_TRUE(open);
    ASSERT_TRUE(control.confirm_phone_pairing(caller, open.value()));
    const auto matching = control.request_add_phone(caller); ASSERT_TRUE(matching);
    phones.note_candidate(trust::WirelessIdentity{"other", "adapter"});
    const auto unrelated = control.request_add_phone(caller); ASSERT_TRUE(unrelated);
    const auto phone = store.phones().front().phone_id;
    bridge.retirement_limit = 1;
    // When
    const auto initial = control.forget_phone(caller, phone); ASSERT_FALSE(initial);
    // Then: completed external legs cannot hide pending cleanup or cause reissued acknowledgements.
    EXPECT_EQ(initial.error().code(), ErrorCode::helper_unavailable);
    EXPECT_EQ(bridge.cancellations, 0);
    ASSERT_EQ(bridge.retirement_batches.size(), 1U);
    const std::vector<helper::RetirementSource> expected{{core::ConsumerId{1}, open.value()},
                                                       {core::ConsumerId{1}, matching.value()}};
    EXPECT_EQ(bridge.retirement_batches.front(), expected);
    auto progress = control.revocation_progress(phone); ASSERT_TRUE(progress);
    EXPECT_EQ(progress->ap, helper::RevocationLeg::complete);
    EXPECT_EQ(progress->bond, helper::RevocationLeg::complete);
    ASSERT_EQ(progress->retirements.size(), 1U);
    EXPECT_EQ(progress->retirements.front(), expected.back());
    EXPECT_EQ(count_closed(), 2U);
    EXPECT_TRUE(phones.has_pending(unrelated.value()));
    bridge.retirement_limit = 0;
    for (int retry = 0; retry < 2; ++retry) {
        EXPECT_FALSE(control.forget_phone(caller, phone));
        EXPECT_EQ(bridge.retirement_batches.back(), progress->retirements);
        EXPECT_EQ(control.pending_revocations(), 1U);
        EXPECT_EQ(count_closed(), 2U);
    }
    bridge.retirement_limit.reset();
    EXPECT_TRUE(control.forget_phone(caller, phone));
    EXPECT_EQ(control.pending_revocations(), 0U);
    EXPECT_EQ(bridge.ap_attempts, 1); EXPECT_EQ(bridge.bond_attempts, 1);
    EXPECT_EQ(count_closed(), 2U);
    EXPECT_TRUE(phones.has_pending(unrelated.value()));
    EXPECT_FALSE(control.forget_phone(caller, phone));
    EXPECT_EQ(bridge.forgets, 4);
}

TEST_F(HelperConfirmation, Original703WinsEveryCleanupOnlyRetryAndFinalRetirement) {
    // Given
    const auto open = control.request_add_phone(caller); ASSERT_TRUE(open);
    ASSERT_TRUE(control.confirm_phone_pairing(caller, open.value()));
    const auto phone = store.phones().front().phone_id;
    bridge.retirement_limit = 0;
    // When
    {
        const trust::test::faults::ScopedFault fault{trust::test::faults::Fault::dir_fsync};
        const auto initial = control.forget_phone(caller, phone); ASSERT_FALSE(initial);
        EXPECT_EQ(initial.error().code(), ErrorCode::trust_store_not_durable);
    }
    // Then
    for (int retry = 0; retry < 3; ++retry) {
        const auto result = control.forget_phone(caller, phone); ASSERT_FALSE(result);
        EXPECT_EQ(result.error().code(), ErrorCode::trust_store_not_durable);
        EXPECT_EQ(control.pending_revocations(), 1U);
    }
    bridge.retirement_limit.reset();
    const auto finished = control.forget_phone(caller, phone); ASSERT_FALSE(finished);
    EXPECT_EQ(finished.error().code(), ErrorCode::trust_store_not_durable);
    EXPECT_EQ(control.pending_revocations(), 0U);
    EXPECT_EQ(bridge.ap_attempts, 1); EXPECT_EQ(bridge.bond_attempts, 1);
    EXPECT_EQ(count_closed(), 1U);
}

TEST_F(HelperConfirmation, AcknowledgedRetirementIsNotResentAfterConsumerChangeWithApPending) {
    // Given
    const auto open = control.request_add_phone(caller); ASSERT_TRUE(open);
    ASSERT_TRUE(control.confirm_phone_pairing(caller, open.value()));
    const auto phone = store.phones().front().phone_id;
    bridge.ap = Error{ErrorCode::helper_unavailable};
    EXPECT_FALSE(control.forget_phone(caller, phone));
    ASSERT_EQ(bridge.retirement_batches.front().size(), 1U);
    ASSERT_TRUE(control.unregister_consumer(caller));
    ASSERT_TRUE(control.register_consumer(caller, name));
    // When
    EXPECT_FALSE(control.forget_phone(caller, phone));
    // Then
    EXPECT_TRUE(bridge.retirement_batches.back().empty());
    EXPECT_EQ(control.pending_revocations(), 1U);
    bridge.ap = {};
    EXPECT_TRUE(control.forget_phone(caller, phone));
    EXPECT_TRUE(bridge.retirement_batches.back().empty());
    EXPECT_EQ(control.pending_revocations(), 0U);
    EXPECT_EQ(bridge.bond_attempts, 1);
    EXPECT_EQ(count_closed(), 1U);
}
}
