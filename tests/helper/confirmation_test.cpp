// SPDX-License-Identifier: GPL-3.0-or-later
#include "confirmation_fixture.hpp"
#include "../trust/syscall_faults.hpp"

namespace {
using namespace aa;
using helper::test::HelperConfirmation;
TEST_F(HelperConfirmation, HelperFailureDoesNotClaimTrustRollbackOrPairingOpened) {
    const auto source = control.request_add_phone(caller); ASSERT_TRUE(source);
    bridge.result = Error{ErrorCode::helper_unavailable};
    EXPECT_FALSE(control.confirm_phone_pairing(caller, source.value()));
    EXPECT_EQ(bridge.confirmations, 1);
    EXPECT_TRUE(bridge.approved_at_call);
    EXPECT_EQ(store.phones().size(), 1U);
    EXPECT_FALSE(control.pairing_window_open());
    EXPECT_EQ(events.count_opened(), 0U);
    EXPECT_EQ(bridge.cancellations, 1);
}
TEST_F(HelperConfirmation, UnauthorizedAndFailedTrustNeverReachBridge) {
    const auto source = control.request_add_phone(caller); ASSERT_TRUE(source);
    EXPECT_FALSE(control.confirm_phone_pairing(rogue, source.value()));
    EXPECT_EQ(bridge.confirmations, 0);
    ASSERT_TRUE(control.cancel_phone_pairing(caller, source.value()));
    EXPECT_FALSE(control.confirm_phone_pairing(caller, source.value()));
    EXPECT_EQ(bridge.confirmations, 0);
}
TEST_F(HelperConfirmation, ShutdownAndOwnerLossNotifyLifecycle) {
    credentials.remove_owner(name);
    control.refresh_consumer_owner();
    EXPECT_EQ(bridge.teardowns, 1);
    control.shutdown();
    EXPECT_EQ(bridge.teardowns, 1);
    credentials.set_owner(name, caller);
    ASSERT_TRUE(control.register_consumer(caller, name));
    control.shutdown();
    EXPECT_EQ(bridge.teardowns, 2);
}
TEST_F(HelperConfirmation, RepeatedFailing703RetriesKeepOriginalDurabilityError) {
    const auto phone = store.approve(trust::WirelessIdentity{"old-bond", "old-adapter"}); ASSERT_TRUE(phone);
    bridge.ap = Error{ErrorCode::helper_unavailable};
    bridge.bond = Error{ErrorCode::helper_rejected};
    {
        const trust::test::faults::ScopedFault fault{trust::test::faults::Fault::dir_fsync};
        const auto result = control.forget_phone(caller, phone.value()); ASSERT_FALSE(result);
        EXPECT_EQ(result.error().code(), ErrorCode::trust_store_not_durable);
    }
    for (int retry = 0; retry < 3; ++retry) {
        const auto result = control.forget_phone(caller, phone.value()); ASSERT_FALSE(result);
        EXPECT_EQ(result.error().code(), ErrorCode::trust_store_not_durable);
        EXPECT_EQ(control.pending_revocations(), 1U);
    }
    EXPECT_EQ(bridge.ap_attempts, 4); EXPECT_EQ(bridge.bond_attempts, 4);
    bridge.ap = {}; bridge.bond = {};
    const auto completed = control.forget_phone(caller, phone.value()); ASSERT_FALSE(completed);
    EXPECT_EQ(completed.error().code(), ErrorCode::trust_store_not_durable);
    EXPECT_EQ(control.pending_revocations(), 0U);
}
TEST_F(HelperConfirmation, FailedApStillAttemptsBondAndDoesNotResendAcknowledgedBond) {
    const trust::TransportIdentity identity{trust::WirelessIdentity{"old-bond", "old-adapter"}};
    const auto phone = store.approve(identity); ASSERT_TRUE(phone);
    bridge.ap = Error{ErrorCode::helper_unavailable};
    const auto result = control.forget_phone(caller, phone.value()); ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code(), ErrorCode::helper_unavailable);
    const auto progress = control.revocation_progress(phone.value()); ASSERT_TRUE(progress);
    EXPECT_EQ(progress->ap, helper::RevocationLeg::pending);
    EXPECT_EQ(progress->bond, helper::RevocationLeg::complete);
    for (int retry = 0; retry < 3; ++retry) EXPECT_FALSE(control.forget_phone(caller, phone.value()));
    EXPECT_EQ(bridge.ap_attempts, 4); EXPECT_EQ(bridge.bond_attempts, 1);
    EXPECT_EQ(bridge.snapshots.back().identity, identity);
    bridge.ap = {};
    EXPECT_TRUE(control.forget_phone(caller, phone.value()));
    EXPECT_EQ(control.pending_revocations(), 0U);
    EXPECT_FALSE(control.forget_phone(caller, phone.value()));
    EXPECT_EQ(bridge.ap_attempts, 5); EXPECT_EQ(bridge.bond_attempts, 1);
}
TEST_F(HelperConfirmation, AcknowledgedApIsNotRepeatedDuringFailedBondRetries) {
    const auto phone = store.approve(trust::WirelessIdentity{"old-bond", "old-adapter"}); ASSERT_TRUE(phone);
    bridge.bond = Error{ErrorCode::helper_rejected};
    for (int retry = 0; retry < 4; ++retry) {
        const auto result = control.forget_phone(caller, phone.value()); ASSERT_FALSE(result);
        EXPECT_EQ(result.error().code(), ErrorCode::helper_rejected);
        const auto progress = control.revocation_progress(phone.value()); ASSERT_TRUE(progress);
        EXPECT_EQ(progress->ap, helper::RevocationLeg::complete);
        EXPECT_EQ(progress->bond, helper::RevocationLeg::pending);
    }
    EXPECT_EQ(bridge.ap_attempts, 1); EXPECT_EQ(bridge.bond_attempts, 4);
    bridge.bond = {};
    EXPECT_TRUE(control.forget_phone(caller, phone.value()));
    EXPECT_EQ(bridge.ap_attempts, 1); EXPECT_EQ(bridge.bond_attempts, 5);
}
TEST_F(HelperConfirmation, DurableIncompleteApErrorPrecedesBondError) {
    const auto phone = store.approve(trust::WirelessIdentity{"old-bond", "old-adapter"}); ASSERT_TRUE(phone);
    bridge.ap = Error{ErrorCode::timeout}; bridge.bond = Error{ErrorCode::helper_rejected};
    const auto initial = control.forget_phone(caller, phone.value()); ASSERT_FALSE(initial);
    EXPECT_EQ(initial.error().code(), ErrorCode::timeout);
    bridge.ap = {};
    const auto retried = control.forget_phone(caller, phone.value()); ASSERT_FALSE(retried);
    EXPECT_EQ(retried.error().code(), ErrorCode::helper_rejected);
}
TEST_F(HelperConfirmation, WiredAndOpaqueDeletionRequireOnlyAp) {
    for (const trust::TransportIdentity& identity : {trust::TransportIdentity{trust::WiredIdentity{"18d1", "2d00", "serial", "", "", "", "1-2"}},
                                                   trust::TransportIdentity{trust::OpaqueIdentity{"legacy-key"}}}) {
        const auto phone = store.approve(identity); ASSERT_TRUE(phone);
        EXPECT_TRUE(control.forget_phone(caller, phone.value()));
        EXPECT_EQ(bridge.snapshots.back().identity, identity);
    }
    EXPECT_EQ(bridge.ap_attempts, 2); EXPECT_EQ(bridge.bond_attempts, 0);
}
TEST_F(HelperConfirmation, PrecommitFailurePreservesTrustAndNeverReachesExternalLegs) {
    const auto phone = store.approve(trust::WirelessIdentity{"old-bond", "old-adapter"}); ASSERT_TRUE(phone);
    {
        const trust::test::faults::ScopedFault fault{trust::test::faults::Fault::renameat};
        EXPECT_FALSE(control.forget_phone(caller, phone.value()));
    }
    EXPECT_EQ(store.phones().size(), 1U); EXPECT_EQ(bridge.forgets, 0);
    EXPECT_EQ(control.pending_revocations(), 0U);
    EXPECT_TRUE(control.forget_phone(caller, phone.value()));
}
TEST_F(HelperConfirmation, PendingExactIdentityCannotBeReapprovedAndUnrelatedCandidateRemainsUsable) {
    const auto phone = store.approve(trust::WirelessIdentity{"old-bond", "old-adapter"}); ASSERT_TRUE(phone);
    bridge.bond = Error{ErrorCode::helper_unavailable};
    EXPECT_FALSE(control.forget_phone(caller, phone.value()));
    phones.note_candidate(trust::WirelessIdentity{"old-bond", "old-adapter"});
    EXPECT_FALSE(control.request_add_phone(caller));
    bridge.prepared = helper::PeerIdentity{"old-bond", "old-adapter"};
    EXPECT_FALSE(control.request_add_phone(caller)); EXPECT_TRUE(store.phones().empty());
    EXPECT_EQ(events.count_opened(), 0U);
    bridge.prepared = helper::PeerIdentity{"other-bond", "other-adapter"};
    const auto request = control.request_add_phone(caller); ASSERT_TRUE(request);
    EXPECT_TRUE(control.confirm_phone_pairing(caller, request.value()));
}
TEST_F(HelperConfirmation, CommittedForgetClosesMatchingRequestsOnceAndPreservesUnrelatedHistory) {
    const auto open = control.request_add_phone(caller); ASSERT_TRUE(open);
    ASSERT_TRUE(control.confirm_phone_pairing(caller, open.value()));
    const auto matching = control.request_add_phone(caller); ASSERT_TRUE(matching);
    phones.note_candidate(trust::WirelessIdentity{"other", "adapter"});
    const auto unrelated = control.request_add_phone(caller); ASSERT_TRUE(unrelated);
    bridge.ap = Error{ErrorCode::helper_unavailable};
    const auto phone = store.phones().front().phone_id;
    EXPECT_FALSE(control.forget_phone(caller, phone));
    EXPECT_EQ(count_closed(), 2U);
    EXPECT_FALSE(phones.has_pending(matching.value())); EXPECT_TRUE(phones.has_pending(unrelated.value()));
    EXPECT_FALSE(control.forget_phone(caller, phone)); EXPECT_EQ(count_closed(), 2U);
    EXPECT_FALSE(control.confirm_phone_pairing(caller, open.value()));
    EXPECT_TRUE(control.confirm_phone_pairing(caller, unrelated.value()));
}
TEST_F(HelperConfirmation, UnauthorizedRetryAndConsumerTeardownPreservePendingRecord) {
    const auto phone = store.approve(trust::WirelessIdentity{"old-bond", "old-adapter"}); ASSERT_TRUE(phone);
    bridge.ap = Error{ErrorCode::helper_unavailable};
    EXPECT_FALSE(control.forget_phone(caller, phone.value()));
    EXPECT_FALSE(control.forget_phone(rogue, phone.value())); EXPECT_EQ(bridge.ap_attempts, 1);
    EXPECT_TRUE(control.unregister_consumer(caller)); EXPECT_EQ(control.pending_revocations(), 1U);
    ASSERT_TRUE(control.register_consumer(caller, name));
    bridge.ap = {};
    EXPECT_TRUE(control.forget_phone(caller, phone.value())); EXPECT_EQ(bridge.bond_attempts, 1);
}
TEST_F(HelperConfirmation, MissingCommittedSnapshotIsContractFailureNotCleanupSuccess) {
    ipc::test::RecordingPhoneDirectory missing;
    ipc::ControlService broken{{1000}, {credentials, events, ids, missing, &bridge}};
    ASSERT_TRUE(broken.register_consumer(caller, name));
    const auto result = broken.forget_phone(caller, core::PhoneId{1}); ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code(), ErrorCode::helper_protocol_mismatch);
    EXPECT_EQ(bridge.forgets, 0);
}
TEST_F(HelperConfirmation, WiredOpenPairingClosesOnceDespitePendingAp) {
    phones.note_candidate(trust::WiredIdentity{"18d1", "2d00", "serial", "", "", "", "1-2"});
    const auto request = control.request_add_phone(caller); ASSERT_TRUE(request);
    ASSERT_TRUE(control.confirm_phone_pairing(caller, request.value()));
    const auto phone = store.phones().front().phone_id;
    bridge.ap = Error{ErrorCode::helper_unavailable};
    EXPECT_FALSE(control.forget_phone(caller, phone)); EXPECT_EQ(count_closed(), 1U);
    EXPECT_FALSE(control.forget_phone(caller, phone)); EXPECT_EQ(count_closed(), 1U);
    EXPECT_FALSE(control.pairing_window_open()); EXPECT_EQ(bridge.bond_attempts, 0);
}
TEST_F(HelperConfirmation, MissingHelperKeepsRequiredApPending) {
    const auto phone = store.approve(trust::OpaqueIdentity{"legacy"}); ASSERT_TRUE(phone);
    ipc::ControlService unavailable{{1000}, {credentials, events, ids, phones}};
    ASSERT_TRUE(unavailable.register_consumer(caller, name));
    const auto result = unavailable.forget_phone(caller, phone.value()); ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code(), ErrorCode::helper_unavailable);
    EXPECT_EQ(unavailable.pending_revocations(), 1U);
}
TEST_F(HelperConfirmation, FullPendingTableRejectsBeforeStoreMutation) {
    bridge.ap = Error{ErrorCode::helper_unavailable};
    for (int index = 0; index < 256; ++index) {
        const auto phone = store.approve(trust::OpaqueIdentity{"pending-" + std::to_string(index)}); ASSERT_TRUE(phone);
        EXPECT_FALSE(control.forget_phone(caller, phone.value()));
    }
    EXPECT_EQ(control.pending_revocations(), 256U);
    const auto retained = store.approve(trust::OpaqueIdentity{"retained"}); ASSERT_TRUE(retained);
    const auto full = control.forget_phone(caller, retained.value()); ASSERT_FALSE(full);
    EXPECT_EQ(full.error().code(), ErrorCode::ipc_queue_full);
    ASSERT_EQ(store.phones().size(), 1U); EXPECT_EQ(store.phones().front().phone_id, retained.value());
    EXPECT_EQ(bridge.forgets, 256);
}
}
