// SPDX-License-Identifier: GPL-3.0-or-later
#include "confirmation_fixture.hpp"
#include "../trust/syscall_faults.hpp"

namespace {
using namespace aa;
using helper::test::HelperConfirmation;
TEST_F(HelperConfirmation, HelperFailureNeverPersistsApprovalOrOpensPairing) {
    const auto source = control.request_add_phone(caller); ASSERT_TRUE(source);
    bridge.result = Error{ErrorCode::helper_unavailable};
    EXPECT_FALSE(control.confirm_phone_pairing(caller, source.value()));
    EXPECT_EQ(bridge.confirmations, 1);
    EXPECT_FALSE(bridge.approved_at_call);
    EXPECT_TRUE(store.phones().empty());
    trust::ApprovedPhoneStore reloaded{directory / "store"};
    ASSERT_TRUE(reloaded.load());
    EXPECT_TRUE(reloaded.phones().empty());
    EXPECT_EQ(phones.pending_count(), 0U);
    EXPECT_FALSE(control.confirm_phone_pairing(caller, source.value()));
    EXPECT_FALSE(control.pairing_window_open());
    EXPECT_EQ(events.count_opened(), 0U);
    EXPECT_EQ(bridge.cancellations, 1);
}
TEST_F(HelperConfirmation, HelperSuccessPrecedesDurableApproval) {
    const auto source = control.request_add_phone(caller); ASSERT_TRUE(source);
    ASSERT_TRUE(control.confirm_phone_pairing(caller, source.value()));
    EXPECT_FALSE(bridge.approved_at_call);
    EXPECT_EQ(store.phones().size(), 1U);
    trust::ApprovedPhoneStore reloaded{directory / "store"};
    ASSERT_TRUE(reloaded.load());
    EXPECT_EQ(reloaded.phones().size(), 1U);
    EXPECT_EQ(events.count_opened(), 1U);
}
TEST_F(HelperConfirmation, PersistenceFailureAfterHelperSuccessCancelsWindow) {
    const auto source = control.request_add_phone(caller); ASSERT_TRUE(source);
    {
        const trust::test::faults::ScopedFault fault{trust::test::faults::Fault::renameat};
        EXPECT_FALSE(control.confirm_phone_pairing(caller, source.value()));
    }
    EXPECT_EQ(bridge.confirmations, 1);
    EXPECT_EQ(bridge.cancellations, 1);
    EXPECT_TRUE(store.phones().empty());
    EXPECT_EQ(events.count_opened(), 0U);
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
    const auto phone = store.approve(trust::WirelessIdentity{"02:00:00:00:00:02", "old-adapter"}); ASSERT_TRUE(phone);
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
    const trust::TransportIdentity identity{trust::WirelessIdentity{"02:00:00:00:00:02", "old-adapter"}};
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
    const auto phone = store.approve(trust::WirelessIdentity{"02:00:00:00:00:02", "old-adapter"}); ASSERT_TRUE(phone);
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
    const auto phone = store.approve(trust::WirelessIdentity{"02:00:00:00:00:02", "old-adapter"}); ASSERT_TRUE(phone);
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
    const auto phone = store.approve(trust::WirelessIdentity{"02:00:00:00:00:02", "old-adapter"}); ASSERT_TRUE(phone);
    {
        const trust::test::faults::ScopedFault fault{trust::test::faults::Fault::renameat};
        EXPECT_FALSE(control.forget_phone(caller, phone.value()));
    }
    EXPECT_EQ(store.phones().size(), 1U); EXPECT_EQ(bridge.forgets, 0);
    EXPECT_EQ(control.pending_revocations(), 0U);
    EXPECT_TRUE(control.forget_phone(caller, phone.value()));
}
TEST_F(HelperConfirmation, PendingExactIdentityCannotBeReapprovedAndUnrelatedCandidateRemainsUsable) {
    const auto phone = store.approve(trust::WirelessIdentity{"02:00:00:00:AA:02", "old-adapter"}); ASSERT_TRUE(phone);
    bridge.bond = Error{ErrorCode::helper_unavailable};
    EXPECT_FALSE(control.forget_phone(caller, phone.value()));
    phones.note_candidate(trust::WirelessIdentity{"02:00:00:00:aa:02", "old-adapter"});
    EXPECT_FALSE(control.request_add_phone(caller));
    bridge.prepared = helper::PeerIdentity{"02:00:00:00:aa:02", "old-adapter"};
    EXPECT_FALSE(control.request_add_phone(caller)); EXPECT_TRUE(store.phones().empty());
    EXPECT_EQ(events.count_opened(), 0U);
    bridge.prepared = helper::PeerIdentity{"02:00:00:00:00:05", "other-adapter"};
    const auto request = control.request_add_phone(caller); ASSERT_TRUE(request);
    EXPECT_TRUE(control.confirm_phone_pairing(caller, request.value()));
}
TEST_F(HelperConfirmation, CommittedForgetClosesMatchingRequestsOnceAndPreservesUnrelatedHistory) {
    const auto open = control.request_add_phone(caller); ASSERT_TRUE(open);
    ASSERT_TRUE(control.confirm_phone_pairing(caller, open.value()));
    const auto matching = control.request_add_phone(caller); ASSERT_TRUE(matching);
    phones.note_candidate(trust::WirelessIdentity{"02:00:00:00:00:03", "adapter"});
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
    const auto phone = store.approve(trust::WirelessIdentity{"02:00:00:00:00:02", "old-adapter"}); ASSERT_TRUE(phone);
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

namespace {
using namespace aa;
using helper::test::HelperConfirmation;
TEST_F(HelperConfirmation, CommittedApprovalKeepsHelperAndOpenedEventOnDirectorySyncFailure) {
    const auto source = control.request_add_phone(caller); ASSERT_TRUE(source);
    {
        const trust::test::faults::ScopedFault fault{trust::test::faults::Fault::dir_fsync};
        EXPECT_TRUE(control.confirm_phone_pairing(caller, source.value()));
    }
    ASSERT_EQ(store.phones().size(), 1U);
    trust::ApprovedPhoneStore reloaded{directory / "store"};
    ASSERT_TRUE(reloaded.load());
    EXPECT_EQ(reloaded.phones(), store.phones());
    EXPECT_EQ(bridge.confirmations, 1);
    EXPECT_EQ(bridge.cancellations, 0);
    EXPECT_EQ(events.count_opened(), 1U);
    EXPECT_EQ(count_closed(), 0U);
    EXPECT_TRUE(has_diagnostic("pairing_approval_not_durable"));
    EXPECT_FALSE(has_diagnostic("pairing_request_denied"));
}
TEST_F(HelperConfirmation, FailedApprovalRetirementBlocksIdentityUntilAcknowledgedRetry) {
    const auto source = control.request_add_phone(caller); ASSERT_TRUE(source);
    bridge.cancel_result = Error{ErrorCode::timeout};
    {
        const trust::test::faults::ScopedFault fault{trust::test::faults::Fault::renameat};
        EXPECT_FALSE(control.confirm_phone_pairing(caller, source.value()));
    }
    EXPECT_EQ(bridge.cancellations, 1);
    EXPECT_TRUE(store.phones().empty());
    EXPECT_EQ(events.count_opened(), 0U);
    EXPECT_EQ(count_closed(), 1U);
    EXPECT_TRUE(has_diagnostic("phone_revocation_retirement_pending"));
    EXPECT_EQ(control.pending_revocations(), 1U);
    bridge.cancel_result = {};
    bridge.retirement_limit = 0;
    // prepare() must expose the same peer: admission stays blocked while
    // the existing revocation attempt still cannot acknowledge retirement.
    bridge.prepared = helper::PeerIdentity{"02:00:00:00:00:01", "adapter"};
    EXPECT_FALSE(control.request_add_phone(caller));
    EXPECT_EQ(control.pending_revocations(), 1U);
    EXPECT_EQ(events.count_opened(), 0U);
    bridge.retirement_limit.reset();
    control.refresh_consumer_owner();
    EXPECT_EQ(control.pending_revocations(), 0U);
    ASSERT_FALSE(bridge.retirement_batches.empty());
    ASSERT_EQ(bridge.retirement_batches.back().size(), 1U);
    EXPECT_EQ(bridge.retirement_batches.back()[0].request, source.value());
    EXPECT_EQ(bridge.ap_attempts, 0);
    EXPECT_EQ(bridge.bond_attempts, 0);
    EXPECT_TRUE(control.request_add_phone(caller));
}
} // namespace

#include "../trust/fakes.hpp"
#include <cstdio>
#include <thread>

namespace {
using namespace aa;
using helper::test::HelperConfirmation;

TEST_F(HelperConfirmation, FailedExpiryCancellationBlocksIdentityUntilAcknowledgedRetry) {
    trust::test::ManualClock manual_clock;
    trust::PairingPolicy timed_phones{store, manual_clock};
    ipc::ControlService timed_control{{1000}, {credentials, events, ids, timed_phones, &bridge}};
    ASSERT_TRUE(timed_control.register_consumer(caller, name));

    bridge.prepared = helper::PeerIdentity{"02:00:00:00:00:11", "adapter"};
    const auto expired_req = timed_control.request_add_phone(caller);
    ASSERT_TRUE(expired_req);

    manual_clock.advance(core::Milliseconds{trust::PairingPolicy::kConfirmWindow.count + 1});
    bridge.cancel_result = Error{ErrorCode::timeout};
    bridge.prepared = helper::PeerIdentity{"02:00:00:00:00:12", "adapter"};
    ASSERT_TRUE(timed_control.request_add_phone(caller));
    bridge.cancel_result = {};

    EXPECT_EQ(bridge.cancellations, 1);
    EXPECT_TRUE(has_diagnostic("phone_revocation_retirement_pending"));
    EXPECT_EQ(count_closed(), 1U);
    EXPECT_EQ(timed_control.pending_revocations(), 1U);

    bridge.retirement_limit = 0;
    bridge.prepared = helper::PeerIdentity{"02:00:00:00:00:11", "adapter"};
    const auto blocked = timed_control.request_add_phone(caller);
    ASSERT_FALSE(blocked);
    EXPECT_EQ(blocked.error().code(), ErrorCode::trust_rejected);
    EXPECT_EQ(timed_control.pending_revocations(), 1U);

    bridge.retirement_limit.reset();
    timed_control.refresh_consumer_owner();
    EXPECT_EQ(timed_control.pending_revocations(), 0U);
    ASSERT_FALSE(bridge.retirement_batches.empty());
    ASSERT_EQ(bridge.retirement_batches.back().size(), 1U);
    EXPECT_EQ(bridge.retirement_batches.back()[0].request, expired_req.value());
    EXPECT_TRUE(timed_control.request_add_phone(caller));
}

TEST_F(HelperConfirmation, FailedEarlyRejectionCancellationBlocksIdentityUntilAcknowledgedRetry) {
    // Seed 1 direct policy entry + 7 control requests so PairingGate still admits
    // the 8th request, prepare() binds the peer, and pairing_requested() rejects.
    ASSERT_TRUE(phones.note_wireless_candidate(helper::PeerIdentity{"02:00:00:00:00:20", "adapter"}));
    ASSERT_TRUE(phones.pairing_requested(core::PairingRequestId{9999}));
    bridge.prepared = helper::PeerIdentity{"02:00:00:00:00:20", "adapter"};
    std::vector<core::PairingRequestId> pending;
    for (std::size_t i = 0; i < trust::PairingPolicy::kMaxPending - 1; ++i) {
        const auto req = control.request_add_phone(caller);
        ASSERT_TRUE(req);
        pending.push_back(req.value());
    }

    bridge.prepared = helper::PeerIdentity{"02:00:00:00:00:21", "adapter"};
    bridge.cancel_result = Error{ErrorCode::timeout};
    const auto rejected = control.request_add_phone(caller);
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().code(), ErrorCode::ipc_queue_full);
    EXPECT_EQ(bridge.cancellations, 1);
    EXPECT_TRUE(has_diagnostic("phone_revocation_retirement_pending"));
    EXPECT_EQ(control.pending_revocations(), 1U);

    bridge.cancel_result = {};
    bridge.retirement_limit = 0;
    bridge.prepared = helper::PeerIdentity{"02:00:00:00:00:21", "adapter"};
    const auto blocked = control.request_add_phone(caller);
    ASSERT_FALSE(blocked);
    EXPECT_EQ(blocked.error().code(), ErrorCode::trust_rejected);
    EXPECT_EQ(control.pending_revocations(), 1U);

    bridge.retirement_limit.reset();
    control.refresh_consumer_owner();
    EXPECT_EQ(control.pending_revocations(), 0U);
    ASSERT_TRUE(control.cancel_phone_pairing(caller, pending.front()));
    phones.pairing_cancelled(core::PairingRequestId{9999});
    EXPECT_TRUE(control.request_add_phone(caller));
}

TEST_F(HelperConfirmation, FailedUserCancelBlocksIdentityUntilAcknowledgedRetry) {
    bridge.prepared = helper::PeerIdentity{"02:00:00:00:00:31", "adapter"};
    const auto source = control.request_add_phone(caller);
    ASSERT_TRUE(source);

    bridge.cancel_result = Error{ErrorCode::timeout};
    ASSERT_TRUE(control.cancel_phone_pairing(caller, source.value()));
    bridge.cancel_result = {};
    EXPECT_EQ(bridge.cancellations, 1);
    EXPECT_EQ(count_closed(), 1U);
    EXPECT_TRUE(has_diagnostic("phone_revocation_retirement_pending"));
    EXPECT_EQ(control.pending_revocations(), 1U);

    bridge.retirement_limit = 0;
    const auto blocked = control.request_add_phone(caller);
    ASSERT_FALSE(blocked);
    EXPECT_EQ(blocked.error().code(), ErrorCode::trust_rejected);
    EXPECT_EQ(control.pending_revocations(), 1U);

    bridge.retirement_limit.reset();
    control.refresh_consumer_owner();
    EXPECT_EQ(control.pending_revocations(), 0U);
    ASSERT_FALSE(bridge.retirement_batches.empty());
    ASSERT_EQ(bridge.retirement_batches.back().size(), 1U);
    EXPECT_EQ(bridge.retirement_batches.back()[0].request, source.value());
    EXPECT_TRUE(control.request_add_phone(caller));
}

TEST_F(HelperConfirmation, FailedConfirmHelperErrorCancellationBlocksIdentityUntilAcknowledgedRetry) {
    bridge.prepared = helper::PeerIdentity{"02:00:00:00:00:41", "adapter"};
    const auto source = control.request_add_phone(caller);
    ASSERT_TRUE(source);

    bridge.result = Error{ErrorCode::helper_unavailable};
    bridge.cancel_result = Error{ErrorCode::timeout};
    const auto failed = control.confirm_phone_pairing(caller, source.value());
    ASSERT_FALSE(failed);
    EXPECT_EQ(failed.error().code(), ErrorCode::helper_unavailable);
    bridge.result = {};
    bridge.cancel_result = {};
    EXPECT_EQ(bridge.cancellations, 1);
    EXPECT_TRUE(has_diagnostic("phone_revocation_retirement_pending"));
    EXPECT_EQ(control.pending_revocations(), 1U);

    bridge.retirement_limit = 0;
    const auto blocked = control.request_add_phone(caller);
    ASSERT_FALSE(blocked);
    EXPECT_EQ(blocked.error().code(), ErrorCode::trust_rejected);

    bridge.retirement_limit.reset();
    control.refresh_consumer_owner();
    EXPECT_EQ(control.pending_revocations(), 0U);
    EXPECT_TRUE(control.request_add_phone(caller));
}

TEST_F(HelperConfirmation, FailedOpenPairingExpiryCancellationBlocksIdentityUntilAcknowledgedRetry) {
    auto clock_now = std::chrono::steady_clock::time_point{std::chrono::seconds{100}};
    ipc::ControlService timed{{1000}, {credentials, events, ids, phones, &bridge,
                                     [&] { return clock_now; }}};
    ASSERT_TRUE(timed.register_consumer(caller, name));

    bridge.prepared = helper::PeerIdentity{"02:00:00:00:00:51", "adapter"};
    const auto source = timed.request_add_phone(caller);
    ASSERT_TRUE(source);
    ASSERT_TRUE(timed.confirm_phone_pairing(caller, source.value()));

    bridge.expect_empty_store = false;
    bridge.cancel_result = Error{ErrorCode::timeout};
    clock_now += std::chrono::seconds{61};
    (void)timed.state();
    bridge.cancel_result = {};

    EXPECT_EQ(bridge.cancellations, 1);
    EXPECT_EQ(count_closed(), 1U);
    EXPECT_TRUE(has_diagnostic("phone_revocation_retirement_pending"));
    EXPECT_EQ(timed.pending_revocations(), 1U);

    bridge.retirement_limit = 0;
    const auto blocked = timed.request_add_phone(caller);
    ASSERT_FALSE(blocked);
    EXPECT_EQ(blocked.error().code(), ErrorCode::trust_rejected);
    EXPECT_EQ(timed.pending_revocations(), 1U);

    bridge.retirement_limit.reset();
    timed.refresh_consumer_owner();
    EXPECT_EQ(timed.pending_revocations(), 0U);
    ASSERT_FALSE(bridge.retirement_batches.empty());
    ASSERT_EQ(bridge.retirement_batches.back().size(), 1U);
    EXPECT_EQ(bridge.retirement_batches.back()[0].request, source.value());
}

TEST_F(HelperConfirmation, SourceOnlyRetirementLoopRespectsBudgetBacksOffAndRecovers) {
    auto clock_now = std::chrono::steady_clock::time_point{std::chrono::seconds{100}};
    ipc::ControlService budgeted{{1000}, {credentials, events, ids, phones, &bridge,
                                         [&] { return clock_now; }}};
    ASSERT_TRUE(budgeted.register_consumer(caller, name));

    bridge.cancel_result = Error{ErrorCode::timeout};
    bridge.retirement_limit = 0;
    for (std::size_t i = 0; i < ipc::PairingGate::kIssuedBudget; ++i) {
        char mac[18]{};
        std::snprintf(mac, sizeof(mac), "02:00:00:00:01:%02x", static_cast<unsigned>(i));
        bridge.prepared = helper::PeerIdentity{mac, "adapter"};
        const auto req = budgeted.request_add_phone(caller);
        ASSERT_TRUE(req);
        ASSERT_TRUE(budgeted.cancel_phone_pairing(caller, req.value()));
    }
    ASSERT_EQ(budgeted.pending_revocations(), ipc::PairingGate::kIssuedBudget);

    // A single control call must stay within the per-invocation attempt budget (4).
    bridge.forgets = 0;
    (void)budgeted.state();
    EXPECT_LE(bridge.forgets, static_cast<int>(ipc::ControlService::kSourceRetirementAttemptBudget));

    // Repeated calls while the helper stays down eventually back off the entire queue.
    for (int i = 0; i < 40; ++i) {
        (void)budgeted.state();
    }
    const int before_idle_call = bridge.forgets;
    (void)budgeted.state();
    EXPECT_EQ(bridge.forgets, before_idle_call);

    // Helper recovers: backed-off entries stay skipped until their deadline,
    // then bounded calls drain the entire queue.
    bridge.retirement_limit.reset();
    bridge.cancel_result = {};
    (void)budgeted.state();
    EXPECT_EQ(budgeted.pending_revocations(), ipc::PairingGate::kIssuedBudget);

    for (int round = 0; round < 32 && budgeted.pending_revocations() > 0; ++round) {
        clock_now += std::chrono::seconds{30};
        (void)budgeted.state();
    }
    EXPECT_EQ(budgeted.pending_revocations(), 0U);
}

TEST_F(HelperConfirmation, SourceOnlyRetirementLoopRespectsTimeBudgetOnStalledHelper) {
    std::chrono::seconds offset{0};
    ipc::ControlService timed{{1000}, {credentials, events, ids, phones, &bridge,
                                     [&] { return std::chrono::steady_clock::now() + offset; }}};
    ASSERT_TRUE(timed.register_consumer(caller, name));

    bridge.cancel_result = Error{ErrorCode::timeout};
    bridge.retirement_limit = 0;
    for (int i = 0; i < 8; ++i) {
        char mac[18]{};
        std::snprintf(mac, sizeof(mac), "02:00:00:00:02:%02x", static_cast<unsigned>(i));
        bridge.prepared = helper::PeerIdentity{mac, "adapter"};
        const auto req = timed.request_add_phone(caller);
        ASSERT_TRUE(req);
        ASSERT_TRUE(timed.cancel_phone_pairing(caller, req.value()));
    }
    ASSERT_EQ(timed.pending_revocations(), 8U);

    // Advance past any backoff accumulated during setup so all 8 are eligible.
    offset += std::chrono::seconds{30};
    bridge.forgets = 0;
    bridge.on_forget = [] { std::this_thread::sleep_for(std::chrono::milliseconds{260}); };
    const auto started = std::chrono::steady_clock::now();
    (void)timed.state();
    const auto elapsed = std::chrono::steady_clock::now() - started;

    // 260ms * 2 = 520ms exceeds the 500ms dispatch budget, stopping before 4 attempts.
    EXPECT_EQ(bridge.forgets, 2);
    EXPECT_LT(elapsed, std::chrono::milliseconds{900});
}
} // namespace
