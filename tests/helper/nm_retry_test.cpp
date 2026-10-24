// SPDX-License-Identifier: GPL-3.0-or-later
// NOLINTNEXTLINE(bugprone-suspicious-include): reuse the private NM wire fixture.
#include "nm_test.cpp"

namespace aa::helper::nm {
namespace {
class NmRetry : public NmDaemon {};
TEST_F(NmRetry, PersistentImmediateRemovalFailureIsPacedAndShutdownStaysPrompt) {
    // Given a live private AP whose firewall removal fails immediately forever.
    auto backend = std::make_unique<Backend>(
        Policy{"wlan-test", "US", 5277, (directory / "state").string(), ::getuid()}, address,
        private_commands((directory / "commands").string()));
    ASSERT_TRUE(backend->apply(Action::acquire_ap, 36));
    ASSERT_TRUE(eventually([&] { return backend->apply(Action::ap_status, 0).value() == State::active; }));
    boundary->block_remove = true;
    ASSERT_FALSE(backend->revoke(7));
    // When the persistent failure is observed over a 350ms rate-measurement window.
    const auto before = boundary->calls.load();
    std::promise<void> window;
    EXPECT_EQ(window.get_future().wait_for(350ms), std::future_status::timeout);
    const auto attempts = boundary->calls.load() - before;
    // Then retries retain >=100ms pacing, while rejected jobs and shutdown are prompt.
    EXPECT_LE(attempts, 5);
    const auto request = Clock::now();
    EXPECT_FALSE(backend->ap_snapshot());
    EXPECT_LT(Clock::now() - request, 100ms);
    const auto shutdown = Clock::now();
    backend.reset();
    EXPECT_LT(Clock::now() - shutdown, 100ms);
    std::cout << "retry_attempts=" << attempts << '\n';
}
}
}
