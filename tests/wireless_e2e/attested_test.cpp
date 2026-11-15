// SPDX-License-Identifier: GPL-3.0-or-later
#include "fixture.hpp"
#include "phone.hpp"
#include <atomic>
#include <future>

namespace {
using namespace std::chrono_literals;
namespace tr = aa::transport;

using wireless_test::walk_phone;

TEST(WirelessAttested, ListenerOutsideValidatedActivationNeverCreatesAapSession) {
    wireless_test::Fixture f;
    f.start();
    auto listener_snapshot = f.snapshot;
    listener_snapshot.generation = 2; // a listener not started from the validated activation
    ASSERT_TRUE(f.link.listen(listener_snapshot));
    auto phone = f.connect();
    ASSERT_TRUE(phone);
    const auto result = f.link.poll(f.coordinator);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code(), aa::ErrorCode::transport_unauthorized_peer);
    EXPECT_EQ(f.link.session(), nullptr);
    phone.value()->set_io_deadline(std::chrono::steady_clock::now() + 1s);
    EXPECT_FALSE(phone.value()->receive());
}

TEST(WirelessAttested, ProductionAdmissionReachesActiveAapAndWifiOverRealTls) {
    wireless_test::Fixture f;
    f.start();
    auto tcp = f.connect();
    ASSERT_TRUE(tcp);
    tcp.value()->set_io_deadline(std::chrono::steady_clock::now() + 5s);
    std::atomic<bool> finished{};
    auto phone = std::async(std::launch::async, [&] {
        walk_phone(*tcp.value());
        std::this_thread::sleep_for(11s);
        finished = true; tcp.value()->close();
    });
    bool active = false;
    bool input_sent = false;
    while (!finished.load()) {
        auto result = f.link.poll(f.coordinator);
        if (!result) {
            if (!finished.load()) ADD_FAILURE() << "link error " << static_cast<int>(result.error().code());
            break;
        }
        if (f.link.session()) active = active || f.link.session()->state() == aa::session::State::active;
        if (active && !f.audio.messages.empty() && !input_sent) {
            ASSERT_TRUE(f.link.session()->submit_input(aa::ipc::InputEvent{aa::ipc::KeyEvent{66, true}}));
            input_sent = true;
        }
        if (active && finished.load()) break;
    }
    phone.get();
    EXPECT_TRUE(active);
    EXPECT_TRUE(input_sent);
    ASSERT_EQ(f.video.messages.size(), 1U);
    ASSERT_EQ(f.audio.messages.size(), 1U);
    EXPECT_EQ(f.video.messages.front(), (std::vector<std::byte>{std::byte{0x10}, std::byte{0x20}}));
}

TEST(WirelessAttested, SilentAuthenticationExpiresWithinTenSeconds) {
    wireless_test::Fixture f;
    f.start();
    auto tcp = f.connect();
    ASSERT_TRUE(tcp);
    const auto began = std::chrono::steady_clock::now();
    ASSERT_TRUE(f.link.poll(f.coordinator));
    EXPECT_FALSE(f.link.poll(f.coordinator));
    const auto elapsed = std::chrono::steady_clock::now() - began;
    EXPECT_LT(elapsed, 10s);
    EXPECT_GT(elapsed, 9s);
}

TEST(WirelessAttested, SilentAuthenticationHelperLossAndTeardownAreBounded) {
    wireless_test::Fixture f;
    f.start();
    auto tcp = f.connect();
    ASSERT_TRUE(tcp);
    ASSERT_TRUE(f.link.poll(f.coordinator));
    const auto began = std::chrono::steady_clock::now();
    f.helper_live = false;
    EXPECT_FALSE(f.link.poll(f.coordinator));
    EXPECT_FALSE(f.link.listening());
    EXPECT_LT(std::chrono::steady_clock::now() - began, 1s);
}TEST(WirelessAttested, FirstSocketOnValidatedActivationAdmitsAndClosesAdmission) {
    wireless_test::Fixture f;
    f.start();
    auto first = f.connect(); ASSERT_TRUE(first);
    auto duplicate = f.connect(); ASSERT_TRUE(duplicate);
    const auto result = f.link.poll(f.coordinator);
    ASSERT_TRUE(result) << static_cast<int>(result.error().code());
    EXPECT_NE(f.link.session(), nullptr);
    EXPECT_FALSE(f.link.listening());
}
}
