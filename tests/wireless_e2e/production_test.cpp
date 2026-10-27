// SPDX-License-Identifier: GPL-3.0-or-later
#include "fixture.hpp"
#include <aa/app/WirelessReceiver.hpp>

TEST(WirelessProduction, WildcardAndInvalidEndpointsFailClosed) {
    wireless_test::Fixture f;
    f.snapshot.ip = "0.0.0.0";
    EXPECT_FALSE(f.link.listen(f.snapshot));
    f.snapshot.ip = "localhost";
    EXPECT_FALSE(f.link.listen(f.snapshot));
    f.snapshot.ip = "127.0.0.1";
    f.snapshot.port = 0;
    EXPECT_FALSE(f.link.listen(f.snapshot));
}
TEST(WirelessProduction, ConsumerLossDuringAcceptStopsListenerWithinOneSecond) {
    using namespace std::chrono_literals;
    wireless_test::Fixture f;
    f.start();
    const auto began = std::chrono::steady_clock::now();
    const auto result = f.link.poll(f.coordinator, [&] { f.coordinator.consumer(std::nullopt); });
    EXPECT_TRUE(result);
    EXPECT_FALSE(f.link.listening());
    EXPECT_LT(std::chrono::steady_clock::now() - began, 1s);
}
TEST(WirelessProduction, BindConflictAndNonSnapshotInterfaceCannotAdmit) {
    wireless_test::Fixture f;
    f.start();
    aa::app::WirelessLink duplicate{{f.trust, f.clock, f.diagnostics}, f.config()};
    EXPECT_FALSE(duplicate.listen(f.snapshot)); EXPECT_FALSE(duplicate.listening());
    const auto other_interface = aa::transport::TcpTransport::connect("127.0.0.2", static_cast<std::uint16_t>(f.snapshot.port));
    EXPECT_FALSE(other_interface);
    EXPECT_EQ(f.link.session(), nullptr);
}
