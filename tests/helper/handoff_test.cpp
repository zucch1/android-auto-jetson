// SPDX-License-Identifier: GPL-3.0-or-later
#include <aa/helper/SystemHelper.hpp>
#include <gtest/gtest.h>
namespace aa::helper {
namespace {
class HandoffIds final : public IdentityLookup {
public:
    std::uint64_t start{1};
    core::Result<ReceiverIdentity> resolve(const std::string& sender) override { return ReceiverIdentity{sender, 1000, 123, start, std::string(64, 'a')}; }
};
class HandoffBackend final : public Backend {
public:
    bool live{true}, allowed{}, ready{}; int snapshots{}, denies{};
    std::uint64_t handle{123};
    std::function<void()> collecting;
    core::Result<State> apply(Action, std::uint32_t) override { return State::active; }
    void cleanup() noexcept override { live = false; }
    core::Result<State> reconnect_peer(std::uint64_t) override { return State::active; }
    core::Result<ObservedPeer> handoff() override { if (!live) return Error{ErrorCode::helper_rejected}; return ObservedPeer{handle, {"AA:BB:CC:DD:EE:FF", "00:11:22:33:44:55"}}; }
    HandoffFailure handoff_failure() const override { return HandoffFailure::none; }
    core::Result<ApSnapshot> ap_snapshot() override { ++snapshots; if (collecting) collecting(); return ApSnapshot{{"ssid", "secret", "02:ab:cd:ef:01:23", "10.42.0.1", 5277, 1}}; }
    void fail_handoff(std::uint64_t captured, HandoffFailure) override { if (captured == handle) live = false; }
    bool snapshot_current(std::uint64_t generation) override { return generation == 1; }
    core::Result<void> authorize_handoff(std::uint64_t handle, bool allow, bool listener_ready) override {
        if (handle != this->handle || !live) return Error{ErrorCode::helper_rejected};
        allowed = allow; ready = listener_ready; if (!allow) { ++denies; live = false; } return {};
    }
};
TEST(HandoffAuthority, ReconnectNeedsNoPendingAddPhoneAndOwnsExactSource) {
    HandoffIds ids; HandoffBackend backend; SystemHelper helper{ids, backend};
    ASSERT_TRUE(helper.reconnect_peer(":1.2", 9));
    ASSERT_TRUE(helper.poll_handoff(":1.2", {7, 0}));
    EXPECT_FALSE(helper.ap_credentials(":1.3", {7, 0}, 123));
    EXPECT_FALSE(helper.ap_credentials(":1.2", {8, 0}, 123));
    EXPECT_FALSE(helper.ap_credentials(":1.2", {7, 0}, 999));
    EXPECT_EQ(backend.snapshots, 0);
    EXPECT_TRUE(helper.ap_credentials(":1.2", {7, 0}, 123)); EXPECT_EQ(backend.snapshots, 1);
    EXPECT_FALSE(helper.authorize_handoff(":1.2", {8, 0}, 123, true, true));
    EXPECT_TRUE(helper.authorize_handoff(":1.2", {7, 0}, 123, true, true)); EXPECT_TRUE(backend.ready);
    ++ids.start; EXPECT_FALSE(helper.ap_credentials(":1.2", {7, 0}, 123));
}
TEST(HandoffAuthority, RetiredHandleAndDenyCannotFetchSecrets) {
    HandoffIds ids; HandoffBackend backend; SystemHelper helper{ids, backend};
    ASSERT_TRUE(helper.reconnect_peer(":1.2", 9)); ASSERT_TRUE(helper.poll_handoff(":1.2", {7, 0}));
    ASSERT_TRUE(helper.authorize_handoff(":1.2", {7, 0}, 123, false, false));
    EXPECT_EQ(backend.denies, 1); EXPECT_FALSE(helper.ap_credentials(":1.2", {7, 0}, 123));
    backend.live = true; ASSERT_TRUE(helper.invalidate(":1.2", {7, 0}));
    EXPECT_FALSE(helper.ap_credentials(":1.2", {7, 0}, 123)); EXPECT_EQ(backend.snapshots, 0);
}
TEST(HandoffAuthority, ReplacementRetiresOnlyDeadHandleAndNeedsFreshAuthorization) {
    // Given: an authorized binding whose bootstrap is denied, with admission still open.
    HandoffIds ids; HandoffBackend backend; SystemHelper helper{ids, backend};
    ASSERT_TRUE(helper.reconnect_peer(":1.2", 9)); ASSERT_TRUE(helper.poll_handoff(":1.2", {7, 0}));
    ASSERT_TRUE(helper.ap_credentials(":1.2", {7, 0}, 123));
    ASSERT_TRUE(helper.authorize_handoff(":1.2", {7, 0}, 123, false, false));
    backend.handle = 456; backend.live = true; backend.allowed = false;
    // When: a fresh accepted socket is polled without reopening admission.
    const auto replacement = helper.poll_handoff(":1.2", {7, 0});
    // Then: old authority stays invalid and the replacement needs its own allow.
    ASSERT_TRUE(replacement); EXPECT_EQ(replacement.value().handle, 456U);
    EXPECT_FALSE(helper.ap_credentials(":1.2", {7, 0}, 123)); EXPECT_FALSE(backend.allowed);
    EXPECT_TRUE(helper.ap_credentials(":1.2", {7, 0}, 456));
    EXPECT_TRUE(helper.authorize_handoff(":1.2", {7, 0}, 456, true, true)); EXPECT_TRUE(backend.allowed);
}
TEST(HandoffAuthority, DelayedOldCollectionCannotRetireReplacementOrEraseItsSnapshot) {
    // Given A's collection reenters after B has replaced its exact handle.
    HandoffIds ids; HandoffBackend backend; SystemHelper helper{ids, backend};
    ASSERT_TRUE(helper.reconnect_peer(":1.2", 9)); ASSERT_TRUE(helper.poll_handoff(":1.2", {7, 0}));
    backend.collecting = [&] {
        backend.handle = 456;
        EXPECT_TRUE(helper.poll_handoff(":1.2", {7, 0}));
    };
    // When A's delayed validation returns.
    EXPECT_FALSE(helper.ap_credentials(":1.2", {7, 0}, 123));
    backend.collecting = {};
    // Then B remains usable and cannot be denied through A.
    EXPECT_TRUE(helper.ap_credentials(":1.2", {7, 0}, 456));
    EXPECT_FALSE(helper.authorize_handoff(":1.2", {7, 0}, 123, false, false));
    EXPECT_TRUE(helper.authorize_handoff(":1.2", {7, 0}, 456, true, true));
}
}
}
