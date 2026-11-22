// SPDX-License-Identifier: GPL-3.0-or-later
#include "../wireless_e2e/host_endpoint.hpp"
#define AA_NM_TEST_ENDPOINT wireless_test::host_endpoint().c_str()
// NOLINTNEXTLINE(bugprone-suspicious-include): reuse private NM/BlueZ process fixture.
#include "../helper/nm_test.cpp"
#include <aa/app/WirelessReceiver.hpp>
#include <aa/session/Clock.hpp>
#include "../wireless_e2e/phone.hpp"

namespace aa::helper::nm {
namespace {
#include "nm_observation.inc"
#include "cleanup_monitor.inc"
#include "fixture.inc"
#include "scenarios.inc"
#include "process.inc"
#include "consumer_fixture.inc"
#include "hardkill.inc"
#include "consumer.inc"
#include "active.inc"

TEST_F(WirelessRecovery, ReconnectRetiresThePreviousHandoffWithoutAnExplicitStop) {
    ASSERT_NO_FATAL_FAILURE(setup_admission());
    ASSERT_EQ(bluez.command("wire-read"), "open");
    ASSERT_TRUE(receiver->reconnect(*control, identity));
    EXPECT_EQ(bluez.command("wire-read"), "closed");
    EXPECT_FALSE(occupied());
    EXPECT_TRUE(client->bootstrap_wait({consumer.value + 1, 0})) << "the old source still owns the lease";
    ASSERT_EQ(bluez.command("NewConnection"), "allowed");
    const auto fresh = observe_snapshot();
    ASSERT_TRUE(fresh);
    EXPECT_NE(fresh->handle, old_handle);
}
}
}
