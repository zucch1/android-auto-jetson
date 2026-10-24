// SPDX-License-Identifier: GPL-3.0-or-later
#include "control_io_fixture.hpp"
#include "phone.hpp"
#include <future>

namespace wireless_test {
struct ActiveCase { bool backpressure; ControlOperation operation; bool independent_loss{}; };
class WirelessControlActive : public testing::TestWithParam<ActiveCase> {};
TEST_P(WirelessControlActive, ActualControlRetiresActiveIo) {
    ControlIoFixture f; ASSERT_NO_FATAL_FAILURE(f.start());
    const auto scenario = GetParam();
    auto tcp = f.f.connect(); ASSERT_TRUE(tcp);
    tcp.value()->set_io_deadline(ControlIoFixture::Clock::now() + 5s);
    std::atomic<bool> active{}, finished{};
    auto phone = std::async(std::launch::async, [&] {
        walk_phone(*tcp.value(), [&] { return active.load(); }); finished = true;
    });
    bool input_sent{}, writing{};
    unsigned blocked_checkpoints{};
    bool grace_at_nine{}, invalidated{};
    bool precommit_preserved{};
    ControlIoFixture::Clock::time_point invalidated_at{};
    auto send_started = ControlIoFixture::Clock::now();
    const auto work = [&] {
        if (!f.dispatched && finished) {
            if (!scenario.backpressure || (writing && ControlIoFixture::Clock::now() - send_started >= 75ms)) {
                if (scenario.operation == ControlOperation::forget && !precommit_preserved) {
                    aa::trust::test::faults::ScopedFault fault{aa::trust::test::faults::Fault::temp_fsync};
                    const auto result = f.control.forget_phone(f.caller, f.phone);
                    EXPECT_FALSE(result); EXPECT_EQ(f.store.phones().size(), 1U);
                    EXPECT_TRUE(f.coordinator.valid_session()); EXPECT_TRUE(f.coordinator.admitting());
                    EXPECT_EQ(f.helper.revocations, 0U);
                    EXPECT_EQ(f.link.session()->state(), aa::session::State::active);
                    precommit_preserved = true;
                    return;
                }
                f.dispatch(scenario.operation);
            }
            if (writing) ++blocked_checkpoints;
        }
        f.checkpoint();
        if (f.dispatched && scenario.operation == ControlOperation::owner_loss) {
            EXPECT_FALSE(f.coordinator.admitting()); EXPECT_FALSE(f.link.listening());
            const auto elapsed = ControlIoFixture::Clock::now() - f.lost;
            if (scenario.independent_loss && !invalidated && elapsed >= 200ms) {
                EXPECT_TRUE(f.coordinator.valid_session());
                invalidated_at = ControlIoFixture::Clock::now();
                f.f.helper_live = false; invalidated = true;
            }
            if (!scenario.independent_loss && elapsed >= 9s && elapsed < 9500ms) {
                EXPECT_TRUE(f.coordinator.valid_session()); grace_at_nine = true;
            }
        }
    };
    const auto until = ControlIoFixture::Clock::now() + 5s;
    while (!finished && ControlIoFixture::Clock::now() < until) {
        const auto result = f.link.poll(f.coordinator, work);
        if (!result) break;
        if (f.link.session() && f.link.session()->state() == aa::session::State::active) {
            active = true;
            if (!f.f.audio.messages.empty() && !input_sent) {
                ASSERT_TRUE(f.link.session()->submit_input(aa::ipc::InputEvent{aa::ipc::KeyEvent{66, true}}));
                input_sent = true;
                break;
            }
        }
    }
    phone.get(); ASSERT_TRUE(finished); ASSERT_TRUE(active); ASSERT_TRUE(input_sent);
    if (scenario.backpressure) {
        ASSERT_NE(f.link.session(), nullptr);
        std::size_t sent{};
        const auto fill_until = ControlIoFixture::Clock::now() + 4s;
        while (!f.dispatched && ControlIoFixture::Clock::now() < fill_until) {
            writing = true; blocked_checkpoints = 0; send_started = ControlIoFixture::Clock::now();
            const auto result = f.link.session()->send_channel_payload(aa::protocol::ChannelRole::input,
                std::vector<std::byte>(16'000, std::byte{0x55}));
            const auto send_returned = ControlIoFixture::Clock::now();
            if (f.dispatched) {
                RecordProperty("final_send_ok", result ? "true" : "false");
                RecordProperty("final_send_after_control_ms", std::to_string(
                    std::chrono::duration_cast<std::chrono::milliseconds>(send_returned - f.lost).count()));
                if (scenario.operation == ControlOperation::owner_loss) { EXPECT_TRUE(result); }
                std::cout << "final_send_ok=" << result.has_value()
                          << " final_send_error=" << (result ? 0 : static_cast<int>(result.error().code()))
                          << " final_send_duration_ms="
                          << std::chrono::duration_cast<std::chrono::milliseconds>(send_returned - send_started).count()
                          << " final_send_after_control_ms="
                          << std::chrono::duration_cast<std::chrono::milliseconds>(send_returned - f.lost).count()
                          << " session_present_before_poll=" << (f.link.session() != nullptr) << std::endl;
            }
            writing = false;
            if (!result) break;
            sent += 16'000;
        }
        EXPECT_GT(sent, 64'000U); EXPECT_GE(blocked_checkpoints, 3U);
        EXPECT_TRUE(f.dispatched);
        std::cout << "backpressure_bytes=" << sent << " wait_checkpoints=" << blocked_checkpoints << '\n';
    }
    if (f.link.session()) (void)f.link.poll(f.coordinator, work);
    const bool grace = scenario.operation == ControlOperation::owner_loss && !scenario.independent_loss;
    if (grace) { EXPECT_TRUE(grace_at_nine); }
    if (scenario.independent_loss) {
        ASSERT_TRUE(invalidated);
        EXPECT_LT(ControlIoFixture::Clock::now() - invalidated_at, 5s);
        EXPECT_FALSE(f.link.listening()); EXPECT_FALSE(f.coordinator.admitting());
    }
    f.retired(grace);
    if (scenario.operation == ControlOperation::forget) { EXPECT_TRUE(precommit_preserved); }
    // Drain queued complete frames; failure must be peer closure, not a deadline.
    tcp.value()->set_io_deadline(ControlIoFixture::Clock::now() + 1s);
    bool closed{};
    for (unsigned frame = 0; frame < 4096; ++frame) {
        const auto result = tcp.value()->receive();
        if (!result) {
            EXPECT_NE(result.error().code(), aa::ErrorCode::timeout);
            closed = true; break;
        }
    }
    EXPECT_TRUE(closed);
}
INSTANTIATE_TEST_SUITE_P(States, WirelessControlActive, testing::Values(
    ActiveCase{false, ControlOperation::forget},
    ActiveCase{true, ControlOperation::forget},
    ActiveCase{true, ControlOperation::owner_loss},
    ActiveCase{false, ControlOperation::owner_loss},
    ActiveCase{false, ControlOperation::owner_loss, true},
    ActiveCase{false, ControlOperation::forget703}));
}
