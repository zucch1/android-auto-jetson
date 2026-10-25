// SPDX-License-Identifier: GPL-3.0-or-later
#include "../wireless_e2e/host_endpoint.hpp"
#define AA_NM_TEST_ENDPOINT wireless_test::host_endpoint().c_str()
// NOLINTNEXTLINE(bugprone-suspicious-include): reuse private NM/BlueZ process fixture.
#include "../helper/nm_test.cpp"
#include <aa/app/WirelessReceiver.hpp>
#include <aa/session/Clock.hpp>
#ifdef AA_WIRELESS_TEST_ATTESTATION
#include "../wireless_e2e/phone.hpp"
#endif

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
#ifdef AA_WIRELESS_TEST_ATTESTATION
#include "active.inc"
#endif
}
}
