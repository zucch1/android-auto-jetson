// SPDX-License-Identifier: GPL-3.0-or-later
// Task 27 pairing-window expiry (QA: "pairing request with no
// ConfirmPhonePairing within 60 s is rejected and not persisted"): the window
// is driven by the injected clock so no test sleeps; a confirm just inside the
// window approves, one past it is rejected with session_pairing_timeout and
// leaves the store untouched.
#include <aa/trust/PairingPolicy.hpp>

#include "fakes.hpp"

#include <gtest/gtest.h>

namespace {

namespace trust = aa::trust;
namespace test = aa::trust::test;
using aa::ErrorCode;

TEST(PairingExpiry, ConfirmJustInsideWindowApproves) {
    test::TempStoreDir dir;
    trust::ApprovedPhoneStore store{dir.file()};
    ASSERT_TRUE(store.load().has_value());
    test::ManualClock clock;
    trust::PairingPolicy policy{store, clock};

    policy.note_candidate(trust::TransportIdentity{test::kWired});
    const aa::core::PairingRequestId request{1};
    ASSERT_TRUE(policy.pairing_requested(request).has_value());

    clock.advance(trust::PairingPolicy::kConfirmWindow);
    const auto approved = policy.pairing_confirmed(request);
    ASSERT_TRUE(approved.has_value());
    EXPECT_EQ(store.lookup(test::identity_of(trust::TransportIdentity{test::kWired})),
              trust::Decision::approved);
}

TEST(PairingExpiry, ConfirmAfterWindowIsRejectedAndNotPersisted) {
    test::TempStoreDir dir;
    trust::ApprovedPhoneStore store{dir.file()};
    ASSERT_TRUE(store.load().has_value());
    test::ManualClock clock;
    trust::PairingPolicy policy{store, clock};

    policy.note_candidate(trust::TransportIdentity{test::kWired});
    const aa::core::PairingRequestId request{2};
    ASSERT_TRUE(policy.pairing_requested(request).has_value());

    clock.advance(aa::core::Milliseconds{
        trust::PairingPolicy::kConfirmWindow.count + 1});
    const auto expired = policy.pairing_confirmed(request);

    ASSERT_FALSE(expired.has_value());
    EXPECT_EQ(expired.error().code(), ErrorCode::session_pairing_timeout);
    EXPECT_TRUE(store.phones().empty());
    EXPECT_FALSE(std::filesystem::exists(dir.file()));
    EXPECT_FALSE(policy.has_pending(request));
}

TEST(PairingExpiry, ExpiredRequestIsNeverPersistedByASweep) {
    test::TempStoreDir dir;
    trust::ApprovedPhoneStore store{dir.file()};
    ASSERT_TRUE(store.load().has_value());
    test::ManualClock clock;
    trust::PairingPolicy policy{store, clock};

    policy.note_candidate(trust::TransportIdentity{test::kWired});
    const aa::core::PairingRequestId request{3};
    ASSERT_TRUE(policy.pairing_requested(request).has_value());

    clock.advance(aa::core::Milliseconds{
        trust::PairingPolicy::kConfirmWindow.count + 1});
    const auto expired_ids = policy.reap_expired();

    ASSERT_EQ(expired_ids.size(), 1U);
    EXPECT_EQ(expired_ids[0], request);
    EXPECT_FALSE(policy.has_pending(request));
    EXPECT_TRUE(store.phones().empty());
    EXPECT_FALSE(std::filesystem::exists(dir.file()));
}

TEST(PairingExpiry, WindowIsPerRequestAndIndependent) {
    test::TempStoreDir dir;
    trust::ApprovedPhoneStore store{dir.file()};
    ASSERT_TRUE(store.load().has_value());
    test::ManualClock clock;
    trust::PairingPolicy policy{store, clock};

    policy.note_candidate(trust::TransportIdentity{test::kWired});
    const aa::core::PairingRequestId early{10};
    ASSERT_TRUE(policy.pairing_requested(early).has_value());
    clock.advance(aa::core::Milliseconds{30'000});
    policy.note_candidate(trust::TransportIdentity{test::kWireless});
    const aa::core::PairingRequestId late{20};
    ASSERT_TRUE(policy.pairing_requested(late).has_value());

    clock.advance(aa::core::Milliseconds{40'000});
    const auto expired_confirm = policy.pairing_confirmed(early);
    ASSERT_FALSE(expired_confirm.has_value());
    EXPECT_EQ(expired_confirm.error().code(), ErrorCode::session_pairing_timeout);

    const auto fresh_confirm = policy.pairing_confirmed(late);
    ASSERT_TRUE(fresh_confirm.has_value());
    EXPECT_EQ(store.lookup(test::identity_of(trust::TransportIdentity{test::kWireless})),
              trust::Decision::approved);
    EXPECT_EQ(store.lookup(test::identity_of(trust::TransportIdentity{test::kWired})),
              trust::Decision::unknown);
}

TEST(PairingExpiry, CancelledRequestLeavesNoResidue) {
    test::TempStoreDir dir;
    trust::ApprovedPhoneStore store{dir.file()};
    ASSERT_TRUE(store.load().has_value());
    test::ManualClock clock;
    trust::PairingPolicy policy{store, clock};

    policy.note_candidate(trust::TransportIdentity{test::kWired});
    const aa::core::PairingRequestId request{30};
    ASSERT_TRUE(policy.pairing_requested(request).has_value());
    policy.pairing_cancelled(request);
    EXPECT_TRUE(policy.reap_expired().empty());
    EXPECT_TRUE(store.phones().empty());
}

} // namespace
