// SPDX-License-Identifier: GPL-3.0-or-later
// Pairing identity binding and post-admission trust re-confirmation (task 17
// followup). A confirm is bound to the exact pending authorized identity (a
// mismatched approval is refused and never opens a transport), and the admitted
// identity is re-confirmed from the TrustStore before negotiating/active so a
// revocation after discovery fails closed. Terminal cleanup releases the
// admitted identity so a session restarts clean.

#include "Fakes.hpp"

#include <aa/session/Lifecycle.hpp>

#include <gtest/gtest.h>

namespace {

namespace ss = aa::session;
namespace test = aa::session::test;
using aa::Error;
using aa::ErrorCode;

TEST(PairingIdentity, MismatchedConfirmPairingIsRejectedAndDoesNotOpen) {
    // Given: pairing authorized for A with external approval landed in the store.
    test::Harness h;
    ASSERT_TRUE(h.drive_to(ss::State::discovering));
    ASSERT_TRUE(h.lc.authorize_pairing(test::kUnknownId).has_value());
    h.trust.set(test::kApprovedId, aa::trust::Decision::approved);
    // When: confirmation arrives for B, not the pending A.
    const auto result = h.lc.confirm_pairing(test::kApprovedId);
    // Then: it is refused, the session holds, and no transport is opened.
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code(), ErrorCode::session_unknown_phone);
    EXPECT_EQ(h.lc.state(), ss::State::pairing);
    EXPECT_EQ(h.transport.open_calls(), 0);
}

TEST(PairingIdentity, MatchingConfirmPairingConnectsPendingIdentity) {
    // Given: pairing authorized for A and A approved externally.
    test::Harness h;
    ASSERT_TRUE(h.drive_to(ss::State::discovering));
    ASSERT_TRUE(h.lc.authorize_pairing(test::kUnknownId).has_value());
    h.trust.set(test::kUnknownId, aa::trust::Decision::approved);
    // When: confirmation for the exact pending identity arrives.
    const auto result = h.lc.confirm_pairing(test::kUnknownId);
    // Then: it connects and opens the transport.
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(h.lc.state(), ss::State::connecting);
    EXPECT_EQ(h.transport.open_calls(), 1);
}

TEST(PairingIdentity, TrustRevokedBeforeTransportReadyFailsClosed) {
    // Given: a connected approved phone whose trust is then revoked.
    test::Harness h;
    ASSERT_TRUE(h.drive_to(ss::State::connecting));
    h.trust.set(test::kApprovedId, aa::trust::Decision::unknown);
    // When: the session tries to move into negotiating.
    const auto result = h.lc.apply(ss::Event::transport_ready);
    // Then: the re-confirmation fails closed and the session never negotiates.
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code(), ErrorCode::session_unknown_phone);
    EXPECT_EQ(h.lc.state(), ss::State::failed);
}

TEST(PairingIdentity, TrustRevokedBeforeNegotiationSucceededFailsClosed) {
    // Given: a negotiating approved phone whose trust is then revoked.
    test::Harness h;
    ASSERT_TRUE(h.drive_to(ss::State::negotiating));
    h.trust.set(test::kApprovedId, aa::trust::Decision::rejected);
    // When: the session tries to become active.
    const auto result = h.lc.apply(ss::Event::negotiation_succeeded);
    // Then: the re-confirmation fails closed and the session never activates.
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code(), ErrorCode::trust_rejected);
    EXPECT_EQ(h.lc.state(), ss::State::failed);
}

TEST(PairingIdentity, TrustRevokedBeforeRecoverFailsClosed) {
    // Given: a degraded approved phone whose trust is then revoked.
    test::Harness h;
    ASSERT_TRUE(h.drive_to(ss::State::degraded));
    h.trust.set(test::kApprovedId, aa::trust::Decision::unknown);
    // When: the session tries to recover to active.
    const auto result = h.lc.apply(ss::Event::recover);
    // Then: the re-confirmation fails closed and the session never re-activates.
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code(), ErrorCode::session_unknown_phone);
    EXPECT_EQ(h.lc.state(), ss::State::failed);
}

TEST(PairingIdentity, TerminalCleanupAllowsCleanRestart) {
    // Given: a session that reaches active then fails terminally.
    test::Harness h;
    ASSERT_TRUE(h.drive_to(ss::State::active));
    ASSERT_TRUE(h.lc.fail(Error{ErrorCode::internal}).has_value());
    ASSERT_TRUE(h.lc.apply(ss::Event::reset).has_value());
    // When: the controller restarts with a fresh discovery and connect.
    ASSERT_TRUE(h.lc.apply(ss::Event::start_discovery).has_value());
    const auto result = h.lc.phone_discovered(test::kApprovedId);
    // Then: terminal cleanup left no stale admission blocking the new session.
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(h.lc.state(), ss::State::connecting);
}

} // namespace
