// SPDX-License-Identifier: GPL-3.0-or-later
#include "control_io_fixture.hpp"

namespace wireless_test {
enum class IoState { accepting, silent_auth, partial_frame };
struct IoCase { IoState state; ControlOperation operation; };
class WirelessControlIo : public testing::TestWithParam<IoCase> {};
TEST_P(WirelessControlIo, ActualControlRetiresStalledLink) {
    // Given: an eligible listener, optionally with an attested silent/partial peer.
    ControlIoFixture f; ASSERT_NO_FATAL_FAILURE(f.start());
    const auto scenario = GetParam();
    SocketPeer peer;
    unsigned checkpoints{};
    bool armed{};
    const auto work = [&] {
        if (armed && ++checkpoints == 3) f.dispatch(scenario.operation);
        f.checkpoint();
    };
    if (scenario.state != IoState::accepting) {
        peer.fd = aa::transport::socket_util::connect_socket("127.0.0.1", static_cast<std::uint16_t>(f.f.snapshot.port));
        ASSERT_GE(peer.fd, 0);
        ASSERT_TRUE(f.link.poll(f.coordinator, work)); ASSERT_NE(f.link.session(), nullptr);
        EXPECT_FALSE(f.link.session()->authenticated());
        if (scenario.state == IoState::partial_frame) {
            const std::array<unsigned char, 2> header{0, 3};
            ASSERT_EQ(::send(peer.fd, header.data(), header.size(), MSG_NOSIGNAL), 2);
        }
    }
    // When: dispatch through the real control/store path from an I/O checkpoint,
    // not the poll entry checkpoint (accept/read must actually be in progress).
    armed = true;
    const auto deadline = ControlIoFixture::Clock::now() + 2s;
    while (!f.dispatched && ControlIoFixture::Clock::now() < deadline) (void)f.link.poll(f.coordinator, work);
    // Then: cancellation includes listener retirement <=1s from eligibility loss.
    ASSERT_TRUE(f.dispatched); EXPECT_GE(checkpoints, 3U);
    EXPECT_LT(ControlIoFixture::Clock::now() - f.lost, 1s);
    f.retired();
    if (peer.fd >= 0) peer.expect_closed();
    aa::trust::ApprovedPhoneStore reloaded{f.store.path()}; ASSERT_TRUE(reloaded.load());
    EXPECT_EQ(reloaded.phones().empty(), scenario.operation == ControlOperation::forget);
}
INSTANTIATE_TEST_SUITE_P(States, WirelessControlIo, testing::Values(
    IoCase{IoState::accepting, ControlOperation::forget},
    IoCase{IoState::accepting, ControlOperation::owner_loss},
    IoCase{IoState::silent_auth, ControlOperation::forget},
    IoCase{IoState::silent_auth, ControlOperation::owner_loss},
    IoCase{IoState::partial_frame, ControlOperation::forget},
    IoCase{IoState::partial_frame, ControlOperation::owner_loss}));
}
