// SPDX-License-Identifier: GPL-3.0-or-later
// Task 27 pairing policy: the trust store changes ONLY after a confirm for the
// same one-time request id, approve-once grants reconnect eligibility, forget
// revokes it, and an unknown/unbound request never reaches the store.
#include <aa/trust/PairingPolicy.hpp>

#include "fakes.hpp"

#include <gtest/gtest.h>

namespace {

namespace trust = aa::trust;
namespace test = aa::trust::test;
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

} // namespace
