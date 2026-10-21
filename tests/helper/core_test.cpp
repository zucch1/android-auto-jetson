// SPDX-License-Identifier: GPL-3.0-or-later
#include <aa/helper/SystemHelper.hpp>
#include <gtest/gtest.h>

namespace aa::helper {
namespace {
class Identities final : public IdentityLookup {
public:
    ReceiverIdentity identity{":1.2", 1000, 123, 456, std::string(64, 'a')};
    bool dead{false};
    core::Result<ReceiverIdentity> resolve(const std::string& name) override {
        if (dead) return Error{ErrorCode::helper_rejected};
        auto result = identity;
        result.unique_name = name;
        return result;
    }
};
class Effects final : public Backend {
public:
    int calls{}, cleanups{};
    bool fail{};
    core::Result<State> apply(Action, std::uint32_t) override {
        ++calls;
        return fail ? core::Result<State>{Error{ErrorCode::helper_unavailable}} : core::Result<State>{State::active};
    }
    void cleanup() noexcept override { ++cleanups; }
};
class HelperCore : public ::testing::Test {
protected:
    Identities ids;
    Effects effects;
    SystemHelper helper{ids, effects};
    SystemHelper::Time now{};
    core::Result<std::uint64_t> issue(Source source = {1, 1}, Action action = Action::acquire_ap,
                                     std::uint32_t channel = 36) {
        return helper.issue(":1.2", source, action, channel, now);
    }
};
TEST_F(HelperCore, OneUseSeparateActionsAndArgumentBinding) {
    const auto ap = issue();
    const auto pairing = issue({1, 1}, Action::open_pairing, 0);
    ASSERT_TRUE(ap); ASSERT_TRUE(pairing);
    EXPECT_NE(ap.value(), pairing.value());
    EXPECT_FALSE(helper.execute(":1.3", {Action::acquire_ap, 36, ap.value()}, now));
    EXPECT_FALSE(helper.execute(":1.2", {Action::open_pairing, 0, ap.value()}, now));
    EXPECT_FALSE(helper.execute(":1.2", {Action::acquire_ap, 40, ap.value()}, now));
    ASSERT_TRUE(helper.execute(":1.2", {Action::acquire_ap, 36, ap.value()}, now));
    EXPECT_FALSE(helper.execute(":1.2", {Action::acquire_ap, 36, ap.value()}, now));
    ASSERT_TRUE(helper.execute(":1.2", {Action::open_pairing, 0, pairing.value()}, now));
    EXPECT_EQ(effects.calls, 2);
    EXPECT_FALSE(issue());
}
TEST_F(HelperCore, ExpiryAndCancellationNeverRefundReplayHistory) {
    auto grant = issue(); ASSERT_TRUE(grant);
    EXPECT_FALSE(helper.execute(":1.2", {Action::acquire_ap, 36, grant.value()}, now + SystemHelper::kGrantLifetime));
    EXPECT_FALSE(issue());
    grant = issue({2, 2}); ASSERT_TRUE(grant);
    ASSERT_TRUE(helper.invalidate(":1.2", {2, 0}));
    EXPECT_FALSE(helper.execute(":1.2", {Action::acquire_ap, 36, grant.value()}, now));
    EXPECT_FALSE(issue({2, 2}));
}
TEST_F(HelperCore, StarttimeDigestAndDeadProcessRevalidateAndCleanLease) {
    auto grant = issue(); ASSERT_TRUE(grant);
    ASSERT_TRUE(helper.execute(":1.2", {Action::acquire_ap, 36, grant.value()}, now));
    ++ids.identity.starttime;
    EXPECT_FALSE(helper.execute(":1.2", {Action::ap_status, 0, 0}, now));
    helper.tick(now);
    EXPECT_EQ(effects.cleanups, 1);
    grant = issue({2, 2}); ASSERT_TRUE(grant);
    ids.identity.digest[0] = 'b';
    EXPECT_FALSE(helper.execute(":1.2", {Action::acquire_ap, 36, grant.value()}, now));
    ids.dead = true;
    helper.tick(now);
    EXPECT_EQ(helper.pending(), 0U);
}
TEST_F(HelperCore, StatusReleaseAndReconnectRequireBindingNotGrants) {
    EXPECT_TRUE(helper.execute(":1.2", {Action::ap_status, 0, 0}, now));
    EXPECT_TRUE(helper.execute(":1.2", {Action::release_ap, 0, 0}, now));
    EXPECT_TRUE(helper.execute(":1.2", {Action::reconnect, 0, 0}, now));
    EXPECT_FALSE(helper.execute(":1.3", {Action::reconnect, 0, 0}, now));
    ids.dead = true;
    helper.tick(now);
    EXPECT_EQ(effects.cleanups, 1);
}
TEST_F(HelperCore, BackendFailureConsumesGrantAndBusLossCleansInternally) {
    auto grant = issue(); ASSERT_TRUE(grant);
    effects.fail = true;
    EXPECT_FALSE(helper.execute(":1.2", {Action::acquire_ap, 36, grant.value()}, now));
    EXPECT_FALSE(helper.execute(":1.2", {Action::acquire_ap, 36, grant.value()}, now));
    EXPECT_EQ(effects.calls, 1);
    helper.bus_lost();
    EXPECT_EQ(effects.cleanups, 1);
    EXPECT_FALSE(issue());
}
TEST_F(HelperCore, CancellingUnrelatedSourceCannotCleanOwningLease) {
    const auto owner = issue({1, 1}); ASSERT_TRUE(owner);
    ASSERT_TRUE(helper.execute(":1.2", {Action::acquire_ap, 36, owner.value()}, now));
    const auto other = issue({2, 2}); ASSERT_TRUE(other);
    EXPECT_FALSE(helper.execute(":1.2", {Action::acquire_ap, 36, other.value()}, now));
    ASSERT_TRUE(helper.invalidate(":1.2", {2, 2}));
    EXPECT_EQ(effects.cleanups, 0);
    ASSERT_TRUE(helper.invalidate(":1.2", {1, 1}));
    EXPECT_EQ(effects.cleanups, 1);
}
TEST_F(HelperCore, PendingAndHistoryExhaustionFailClosed) {
    for (std::size_t i = 0; i < SystemHelper::kMaxPending; ++i) ASSERT_TRUE(issue({1, i + 1}));
    EXPECT_FALSE(issue({1, 100}));
    ASSERT_TRUE(helper.invalidate(":1.2", {0, 0}));
    for (std::size_t i = SystemHelper::kMaxPending; i < SystemHelper::kMaxHistory; ++i) {
        ASSERT_TRUE(issue({1, i + 1}));
        ASSERT_TRUE(helper.invalidate(":1.2", {0, 0}));
    }
    EXPECT_FALSE(issue({2, 9999}));
    EXPECT_FALSE(issue());
}
TEST(HelperBackend, ProductionNeverPretendsEffectsSucceeded) {
    UnavailableBackend backend;
    EXPECT_EQ(backend.apply(Action::ap_status, 0).value(), State::unavailable);
    EXPECT_EQ(backend.apply(Action::usb_status, 0).value(), State::unavailable);
    EXPECT_EQ(backend.apply(Action::modem_status, 0).value(), State::unavailable);
    for (auto action : {Action::acquire_ap, Action::open_pairing, Action::reconnect,
                        Action::register_profile, Action::unregister_profile, Action::release_ap})
        EXPECT_FALSE(backend.apply(action, 36));
}
}
}
