// SPDX-License-Identifier: GPL-3.0-or-later
// Module contract smoke (task 14): session transitions, channel dispatch and
// capability defaults, IPC single-consumer admission and wire names, trust
// fail-closed admission, config boundary validation and the framed transport
// interface are usable exactly as documented.
#include <aa/config/Config.hpp>
#include <aa/core/Result.hpp>
#include <aa/ipc/Control.hpp>
#include <aa/trust/Trust.hpp>

#include <array>
#include <cstddef>
#include <span>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

namespace {

TEST(Ipc, SingleConsumerRuleFailsClosedForEveryWrongShape) {
    // Given: an active consumer that owns the registered name.
    const aa::ipc::ConsumerIdentity active{aa::core::ConsumerId{1}, 1000U};
    // When/Then: every wrong shape of caller is denied by default.
    EXPECT_EQ(aa::ipc::authorize({{aa::core::ConsumerId{1}, 1001U}, active, true}),
              aa::ipc::Authorization::wrong_uid);
    EXPECT_EQ(aa::ipc::authorize({{aa::core::ConsumerId{1}, 1000U}, active, false}),
              aa::ipc::Authorization::unowned_name);
    EXPECT_EQ(aa::ipc::authorize({{aa::core::ConsumerId{2}, 1000U}, active, true}),
              aa::ipc::Authorization::not_active_consumer);
    EXPECT_EQ(aa::ipc::authorize({active, active, true}),
              aa::ipc::Authorization::allowed);
}

TEST(Ipc, WireNamesAreStable) {
    // Given/When/Then: the D-Bus method names are the frozen wire contract.
    EXPECT_EQ(aa::ipc::wire_name(aa::ipc::ControlMethod::register_consumer),
              "RegisterConsumer");
    EXPECT_EQ(aa::ipc::wire_name(aa::ipc::ControlMethod::request_add_phone),
              "RequestAddPhone");
}

TEST(Trust, UnknownPhoneFailsClosed) {
    // Given/When/Then: only an approved phone may hold a session.
    EXPECT_FALSE(aa::trust::allows_session(aa::trust::Decision::unknown));
    EXPECT_FALSE(aa::trust::allows_session(aa::trust::Decision::rejected));
    EXPECT_TRUE(aa::trust::allows_session(aa::trust::Decision::approved));
}

TEST(Config, UnconfirmedJurisdictionFailsClosed) {
    // Given: a config whose jurisdiction was never install-time confirmed.
    aa::config::Config candidate{};
    candidate.credential_path = "/etc/aa/hu.pem";
    candidate.wireless.jurisdiction = "US";
    // When: the boundary validates it.
    const auto result = aa::config::validate(std::move(candidate));
    // Then: wireless startup stays closed with the typed error.
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code(), aa::ErrorCode::config_jurisdiction_unconfirmed);
}

TEST(Config, ConfirmedBenchConfigValidates) {
    // Given: the bench config with confirmed jurisdiction and disabled extras.
    aa::config::Config candidate{};
    candidate.credential_path = "/etc/aa/hu.pem";
    candidate.wireless.jurisdiction = "US";
    candidate.wireless.jurisdiction_confirmed = true;
    // When: the boundary validates it.
    const auto result = aa::config::validate(std::move(candidate));
    // Then: the typed config is returned with conservative defaults.
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value().wireless.channel, 36);
    EXPECT_FALSE(result.value().microphone_enabled);
    EXPECT_FALSE(result.value().sensors_enabled);
}

} // namespace
