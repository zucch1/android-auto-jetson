// SPDX-License-Identifier: GPL-3.0-or-later
#include "fixture.hpp"
#include <aa/app/WirelessReceiver.hpp>

TEST(WirelessProduction, UnboundSocketNeverCreatesAapSession) {
    wireless_test::Fixture f;
    f.start();
    auto phone = f.connect();
    ASSERT_TRUE(phone);
    const auto result = f.link.poll(f.coordinator);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code(), aa::ErrorCode::transport_unauthorized_peer);
    EXPECT_EQ(f.link.session(), nullptr);
    phone.value()->set_io_deadline(std::chrono::steady_clock::now() + std::chrono::seconds{1});
    EXPECT_FALSE(phone.value()->receive());
}
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
TEST(WirelessProduction, ApprovedBFirstAndDuplicateSocketsRemainUnboundAndRejected) {
    wireless_test::Fixture f;
    f.trust.set(aa::trust::PhoneIdentity{"approved-B"}, aa::trust::Decision::approved);
    f.start();
    auto b = f.connect(); ASSERT_TRUE(b);
    auto duplicate = f.connect(); ASSERT_TRUE(duplicate);
    for (int attempt = 0; attempt < 2; ++attempt) {
        const auto result = f.link.poll(f.coordinator);
        ASSERT_FALSE(result);
        EXPECT_EQ(result.error().code(), aa::ErrorCode::transport_unauthorized_peer);
        EXPECT_EQ(f.link.session(), nullptr);
    }
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
