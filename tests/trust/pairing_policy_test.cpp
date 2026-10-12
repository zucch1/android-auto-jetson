// SPDX-License-Identifier: GPL-3.0-or-later
// Task 27 pairing policy: the trust store changes ONLY after a confirm for the
// same one-time request id, approve-once grants reconnect eligibility, forget
// revokes it, and an unknown/unbound request never reaches the store.
#include <aa/trust/PairingPolicy.hpp>

#include "fakes.hpp"
#include "syscall_faults.hpp"

#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

namespace {

namespace trust = aa::trust;
namespace test = aa::trust::test;
namespace faults = aa::trust::test::faults;
using aa::ErrorCode;

TEST(PairingPolicy, ApproveOnceThenReconnectAndForget) {
    test::TempStoreDir dir;
    trust::ApprovedPhoneStore store{dir.file()};
    ASSERT_TRUE(store.load().has_value());
    test::ManualClock clock;
    trust::PairingPolicy policy{store, clock};

    policy.note_candidate(trust::TransportIdentity{test::kWired});
    const aa::core::PairingRequestId request{500};
    ASSERT_TRUE(policy.pairing_requested(request).has_value());

    const auto approved = policy.pairing_confirmed(request);
    ASSERT_TRUE(approved.has_value());

    const auto identity = test::identity_of(trust::TransportIdentity{test::kWired});
    EXPECT_EQ(store.lookup(identity), trust::Decision::approved);
    EXPECT_TRUE(trust::allows_session(store.lookup(identity)));

    ASSERT_TRUE(policy.forget(approved.value()).has_value());
    EXPECT_EQ(store.lookup(identity), trust::Decision::unknown);
    EXPECT_FALSE(trust::allows_session(store.lookup(identity)));
}

TEST(PairingPolicy, RequestWithoutConfirmLeavesStoreUntouched) {
    test::TempStoreDir dir;
    trust::ApprovedPhoneStore store{dir.file()};
    ASSERT_TRUE(store.load().has_value());
    test::ManualClock clock;
    trust::PairingPolicy policy{store, clock};

    policy.note_candidate(trust::TransportIdentity{test::kWired});
    ASSERT_TRUE(policy.pairing_requested(aa::core::PairingRequestId{1}).has_value());

    EXPECT_TRUE(store.phones().empty());
    EXPECT_FALSE(std::filesystem::exists(dir.file()));
}

TEST(PairingPolicy, ConfirmOfUnboundRequestIsRejected) {
    test::TempStoreDir dir;
    trust::ApprovedPhoneStore store{dir.file()};
    ASSERT_TRUE(store.load().has_value());
    test::ManualClock clock;
    trust::PairingPolicy policy{store, clock};

    policy.note_candidate(trust::TransportIdentity{test::kWired});
    const auto confirmed = policy.pairing_confirmed(aa::core::PairingRequestId{999});

    ASSERT_FALSE(confirmed.has_value());
    EXPECT_EQ(confirmed.error().code(), ErrorCode::trust_unknown_phone);
    EXPECT_TRUE(store.phones().empty());
}

TEST(PairingPolicy, RequestWithoutCandidateFailsClosed) {
    test::TempStoreDir dir;
    trust::ApprovedPhoneStore store{dir.file()};
    ASSERT_TRUE(store.load().has_value());
    test::ManualClock clock;
    trust::PairingPolicy policy{store, clock};

    const auto requested = policy.pairing_requested(aa::core::PairingRequestId{1});

    ASSERT_FALSE(requested.has_value());
    EXPECT_EQ(requested.error().code(), ErrorCode::trust_unknown_phone);
    EXPECT_TRUE(store.phones().empty());
}

TEST(PairingPolicy, CancelledPairingNeverReachesTheStore) {
    test::TempStoreDir dir;
    trust::ApprovedPhoneStore store{dir.file()};
    ASSERT_TRUE(store.load().has_value());
    test::ManualClock clock;
    trust::PairingPolicy policy{store, clock};

    policy.note_candidate(trust::TransportIdentity{test::kWired});
    const aa::core::PairingRequestId request{11};
    ASSERT_TRUE(policy.pairing_requested(request).has_value());
    policy.pairing_cancelled(request);

    const auto confirmed = policy.pairing_confirmed(request);
    ASSERT_FALSE(confirmed.has_value());
    EXPECT_EQ(confirmed.error().code(), ErrorCode::trust_unknown_phone);
    EXPECT_TRUE(store.phones().empty());
}

TEST(PairingPolicy, CandidateSwapAfterRequestDoesNotChangeApprovedIdentity) {
    test::TempStoreDir dir;
    trust::ApprovedPhoneStore store{dir.file()};
    ASSERT_TRUE(store.load().has_value());
    test::ManualClock clock;
    trust::PairingPolicy policy{store, clock};

    policy.note_candidate(trust::TransportIdentity{test::kWired});
    const aa::core::PairingRequestId request{21};
    ASSERT_TRUE(policy.pairing_requested(request).has_value());
    policy.note_candidate(trust::TransportIdentity{test::kWireless});

    ASSERT_TRUE(policy.pairing_confirmed(request).has_value());
    EXPECT_EQ(store.lookup(test::identity_of(trust::TransportIdentity{test::kWired})),
              trust::Decision::approved);
    EXPECT_EQ(store.lookup(test::identity_of(trust::TransportIdentity{test::kWireless})),
              trust::Decision::unknown);
}

TEST(PairingPolicy, DuplicateRequestBindingIsRejected) {
    test::TempStoreDir dir;
    trust::ApprovedPhoneStore store{dir.file()};
    ASSERT_TRUE(store.load().has_value());
    test::ManualClock clock;
    trust::PairingPolicy policy{store, clock};

    policy.note_candidate(trust::TransportIdentity{test::kWired});
    const aa::core::PairingRequestId request{31};
    ASSERT_TRUE(policy.pairing_requested(request).has_value());
    policy.note_candidate(trust::TransportIdentity{test::kWireless});

    const auto duplicate = policy.pairing_requested(request);
    ASSERT_FALSE(duplicate.has_value());
    EXPECT_EQ(duplicate.error().code(), ErrorCode::trust_rejected);
}

TEST(PairingPolicy, EachRequestIdApprovesOnlyOnce) {
    test::TempStoreDir dir;
    trust::ApprovedPhoneStore store{dir.file()};
    ASSERT_TRUE(store.load().has_value());
    test::ManualClock clock;
    trust::PairingPolicy policy{store, clock};

    policy.note_candidate(trust::TransportIdentity{test::kWired});
    const aa::core::PairingRequestId request{41};
    ASSERT_TRUE(policy.pairing_requested(request).has_value());
    ASSERT_TRUE(policy.pairing_confirmed(request).has_value());

    const auto again = policy.pairing_confirmed(request);
    ASSERT_FALSE(again.has_value());
    EXPECT_EQ(again.error().code(), ErrorCode::trust_unknown_phone);
    EXPECT_EQ(store.phones().size(), 1U);
}

TEST(PairingPolicy, ForgetInvalidatesPendingForTheSameIdentityOnly) {
    test::TempStoreDir dir;
    trust::ApprovedPhoneStore store{dir.file()};
    ASSERT_TRUE(store.load().has_value());
    test::ManualClock clock;
    trust::PairingPolicy policy{store, clock};

    policy.note_candidate(trust::TransportIdentity{test::kWired});
    const aa::core::PairingRequestId approve_request{500};
    ASSERT_TRUE(policy.pairing_requested(approve_request).has_value());
    const auto approved = policy.pairing_confirmed(approve_request);
    ASSERT_TRUE(approved.has_value());
    const aa::core::PhoneId saved{approved.value()};

    ASSERT_TRUE(policy.pairing_requested(aa::core::PairingRequestId{501}).has_value());
    ASSERT_TRUE(policy.pairing_requested(aa::core::PairingRequestId{502}).has_value());
    policy.note_candidate(trust::TransportIdentity{test::kWireless});
    const aa::core::PairingRequestId unrelated{503};
    ASSERT_TRUE(policy.pairing_requested(unrelated).has_value());

    ASSERT_TRUE(policy.forget(saved).has_value());

    EXPECT_EQ(policy.take_invalidated_pairings(),
              (std::vector<aa::core::PairingRequestId>{aa::core::PairingRequestId{501},
                                                       aa::core::PairingRequestId{502}}));
    EXPECT_TRUE(policy.take_invalidated_pairings().empty());
    EXPECT_EQ(policy.pending_count(), 1U);

    const auto stale_first = policy.pairing_confirmed(aa::core::PairingRequestId{501});
    ASSERT_FALSE(stale_first.has_value());
    EXPECT_EQ(stale_first.error().code(), ErrorCode::trust_unknown_phone);
    const auto stale_second = policy.pairing_confirmed(aa::core::PairingRequestId{502});
    ASSERT_FALSE(stale_second.has_value());
    EXPECT_EQ(stale_second.error().code(), ErrorCode::trust_unknown_phone);

    ASSERT_TRUE(policy.pairing_confirmed(unrelated).has_value());

    const auto identity_a = test::identity_of(trust::TransportIdentity{test::kWired});
    const auto identity_b = test::identity_of(trust::TransportIdentity{test::kWireless});
    EXPECT_EQ(store.lookup(identity_a), trust::Decision::unknown);
    EXPECT_EQ(store.lookup(identity_b), trust::Decision::approved);

    trust::ApprovedPhoneStore reloaded{dir.file()};
    ASSERT_TRUE(reloaded.load().has_value());
    EXPECT_EQ(reloaded.lookup(identity_a), trust::Decision::unknown);
    EXPECT_EQ(reloaded.lookup(identity_b), trust::Decision::approved);
}

TEST(PairingPolicy, ForgetMatchesPendingByCanonicalKeyAcrossIdentityForms) {
    test::TempStoreDir dir;
    std::filesystem::create_directories(dir.file().parent_path());
    const std::string key = trust::identity_key(trust::TransportIdentity{test::kWired});
    {
        std::ofstream out{dir.file()};
        out << "{\"schema_version\":2,\"next_phone_id\":2,\"phones\":["
               "{\"phone_id\":1,\"identity\":{\"kind\":\"opaque\",\"key\":\""
            << key << "\"}}]}";
    }
    ASSERT_EQ(::chmod(dir.file().c_str(), 0600), 0);

    trust::ApprovedPhoneStore store{dir.file()};
    ASSERT_TRUE(store.load().has_value());
    test::ManualClock clock;
    trust::PairingPolicy policy{store, clock};

    policy.note_candidate(trust::TransportIdentity{test::kWired});
    const aa::core::PairingRequestId request{600};
    ASSERT_TRUE(policy.pairing_requested(request).has_value());

    ASSERT_TRUE(policy.forget(aa::core::PhoneId{1}).has_value());

    EXPECT_EQ(policy.take_invalidated_pairings(),
              std::vector<aa::core::PairingRequestId>{request});
    EXPECT_EQ(policy.pending_count(), 0U);

    trust::ApprovedPhoneStore reloaded{dir.file()};
    ASSERT_TRUE(reloaded.load().has_value());
    EXPECT_EQ(reloaded.lookup(test::identity_of(trust::TransportIdentity{test::kWired})),
              trust::Decision::unknown);
}

TEST(PairingPolicy, ForgetPreReplacementFailureLeavesPendingIntact) {
    test::TempStoreDir dir;
    trust::ApprovedPhoneStore store{dir.file()};
    ASSERT_TRUE(store.load().has_value());
    test::ManualClock clock;
    trust::PairingPolicy policy{store, clock};

    policy.note_candidate(trust::TransportIdentity{test::kWired});
    const aa::core::PairingRequestId approve_request{510};
    ASSERT_TRUE(policy.pairing_requested(approve_request).has_value());
    const auto approved = policy.pairing_confirmed(approve_request);
    ASSERT_TRUE(approved.has_value());
    const aa::core::PhoneId saved{approved.value()};
    ASSERT_TRUE(policy.pairing_requested(aa::core::PairingRequestId{511}).has_value());

    {
        const faults::ScopedFault fault{faults::Fault::renameat};
        const auto failed = policy.forget(saved);
        ASSERT_FALSE(failed.has_value());
        EXPECT_EQ(failed.error().code(), ErrorCode::trust_store_io);
    }

    EXPECT_EQ(policy.pending_count(), 1U);
    EXPECT_TRUE(policy.take_invalidated_pairings().empty());
    EXPECT_EQ(store.lookup(test::identity_of(trust::TransportIdentity{test::kWired})),
              trust::Decision::approved);
    ASSERT_TRUE(policy.pairing_confirmed(aa::core::PairingRequestId{511}).has_value());
}

TEST(PairingPolicy, ForgetNotDurableStillInvalidatesAndKeepsRevocation) {
    test::TempStoreDir dir;
    trust::ApprovedPhoneStore store{dir.file()};
    ASSERT_TRUE(store.load().has_value());
    test::ManualClock clock;
    trust::PairingPolicy policy{store, clock};

    policy.note_candidate(trust::TransportIdentity{test::kWired});
    const aa::core::PairingRequestId approve_request{520};
    ASSERT_TRUE(policy.pairing_requested(approve_request).has_value());
    const auto approved = policy.pairing_confirmed(approve_request);
    ASSERT_TRUE(approved.has_value());
    const aa::core::PhoneId saved{approved.value()};
    ASSERT_TRUE(policy.pairing_requested(aa::core::PairingRequestId{521}).has_value());
    ASSERT_TRUE(policy.pairing_requested(aa::core::PairingRequestId{522}).has_value());

    {
        const faults::ScopedFault fault{faults::Fault::dir_fsync};
        const auto not_durable = policy.forget(saved);
        ASSERT_FALSE(not_durable.has_value());
        EXPECT_EQ(not_durable.error().code(), ErrorCode::trust_store_not_durable);
    }

    EXPECT_EQ(policy.pending_count(), 0U);
    EXPECT_EQ(policy.take_invalidated_pairings().size(), 2U);
    EXPECT_EQ(store.lookup(test::identity_of(trust::TransportIdentity{test::kWired})),
              trust::Decision::unknown);

    trust::ApprovedPhoneStore reloaded{dir.file()};
    ASSERT_TRUE(reloaded.load().has_value());
    EXPECT_EQ(reloaded.lookup(test::identity_of(trust::TransportIdentity{test::kWired})),
              trust::Decision::unknown);
}

TEST(PairingPolicy, ForgetUnknownOrZeroPhoneInvalidatesNothing) {
    test::TempStoreDir dir;
    trust::ApprovedPhoneStore store{dir.file()};
    ASSERT_TRUE(store.load().has_value());
    test::ManualClock clock;
    trust::PairingPolicy policy{store, clock};

    policy.note_candidate(trust::TransportIdentity{test::kWired});
    ASSERT_TRUE(policy.pairing_requested(aa::core::PairingRequestId{530}).has_value());

    const auto unknown = policy.forget(aa::core::PhoneId{42});
    ASSERT_FALSE(unknown.has_value());
    EXPECT_EQ(unknown.error().code(), ErrorCode::trust_unknown_phone);
    const auto zero = policy.forget(aa::core::PhoneId{});
    ASSERT_FALSE(zero.has_value());
    EXPECT_EQ(zero.error().code(), ErrorCode::invalid_argument);

    EXPECT_EQ(policy.pending_count(), 1U);
    EXPECT_TRUE(policy.take_invalidated_pairings().empty());
    ASSERT_TRUE(policy.pairing_confirmed(aa::core::PairingRequestId{530}).has_value());
}

TEST(PairingPolicy, ForgetReusesPendingCapacity) {
    test::TempStoreDir dir;
    trust::ApprovedPhoneStore store{dir.file()};
    ASSERT_TRUE(store.load().has_value());
    test::ManualClock clock;
    trust::PairingPolicy policy{store, clock};

    policy.note_candidate(trust::TransportIdentity{test::kWired});
    const aa::core::PairingRequestId approve_request{540};
    ASSERT_TRUE(policy.pairing_requested(approve_request).has_value());
    const auto approved = policy.pairing_confirmed(approve_request);
    ASSERT_TRUE(approved.has_value());
    const aa::core::PhoneId saved{approved.value()};

    std::uint64_t id = 541;
    for (std::size_t index = 0; index < trust::PairingPolicy::kMaxPending; ++index) {
        ASSERT_TRUE(policy.pairing_requested(aa::core::PairingRequestId{id++}).has_value());
    }
    const auto blocked = policy.pairing_requested(aa::core::PairingRequestId{id});
    ASSERT_FALSE(blocked.has_value());
    EXPECT_EQ(blocked.error().code(), ErrorCode::ipc_queue_full);

    ASSERT_TRUE(policy.forget(saved).has_value());
    EXPECT_EQ(policy.take_invalidated_pairings().size(), trust::PairingPolicy::kMaxPending);
    EXPECT_EQ(policy.pending_count(), 0U);
    EXPECT_TRUE(policy.pairing_requested(aa::core::PairingRequestId{id}).has_value());
}

TEST(PairingPolicy, FreshRequestAfterForgetApprovesAgainAndPersists) {
    test::TempStoreDir dir;
    trust::ApprovedPhoneStore store{dir.file()};
    ASSERT_TRUE(store.load().has_value());
    test::ManualClock clock;
    trust::PairingPolicy policy{store, clock};

    policy.note_candidate(trust::TransportIdentity{test::kWired});
    const aa::core::PairingRequestId approve_request{550};
    ASSERT_TRUE(policy.pairing_requested(approve_request).has_value());
    const auto approved = policy.pairing_confirmed(approve_request);
    ASSERT_TRUE(approved.has_value());
    const aa::core::PhoneId saved{approved.value()};

    ASSERT_TRUE(policy.forget(saved).has_value());
    EXPECT_EQ(store.lookup(test::identity_of(trust::TransportIdentity{test::kWired})),
              trust::Decision::unknown);
    ASSERT_TRUE(policy.candidate().has_value());

    ASSERT_TRUE(policy.pairing_requested(aa::core::PairingRequestId{551}).has_value());
    const auto reapproved = policy.pairing_confirmed(aa::core::PairingRequestId{551});
    ASSERT_TRUE(reapproved.has_value());
    EXPECT_EQ(reapproved.value(), aa::core::PhoneId{2});
    EXPECT_EQ(store.lookup(test::identity_of(trust::TransportIdentity{test::kWired})),
              trust::Decision::approved);

    trust::ApprovedPhoneStore reloaded{dir.file()};
    ASSERT_TRUE(reloaded.load().has_value());
    EXPECT_EQ(reloaded.lookup(test::identity_of(trust::TransportIdentity{test::kWired})),
              trust::Decision::approved);
}

} // namespace
