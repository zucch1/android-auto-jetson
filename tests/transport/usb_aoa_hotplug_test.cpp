// SPDX-License-Identifier: GPL-3.0-or-later
// Hotplug and liveness tests for the AOA endpoint (task 28): a detach mid-I/O
// fails the in-flight operation with a typed error and never crashes or spins;
// stale handles are not reused; two detach/attach cycles recover consistently;
// a read with no peer is genuinely blocked (kernel-observed) and honours the
// cancellation bound (stale_state / hung_commands / repeated_interrupts).
//
// All assertions read the test-owned shared observation block (UsbAoaFakes
// LinkState/PipeState), never fixture objects that the pipeline may destroy.

#include "../src/transport/usb_adapter/UsbAoaEndpoint.hpp"

#include "PendingIo.hpp"
#include "UsbAoaFakes.hpp"

#include <aa/core/Cancellation.hpp>
#include <aa/core/Error.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <memory>
#include <thread>

#include <gtest/gtest.h>
#include <sys/syscall.h>

namespace {

namespace tr = aa::transport;
namespace ua = aa::transport::usb_adapter;
namespace fx = aa::test::usb_aoa;
using aa::Error;
using aa::ErrorCode;

tr::WiredAdmission admit_all() {
    return [](const aa::trust::WiredIdentity &) { return true; };
}

struct Link final {
    std::unique_ptr<ua::UsbAoaEndpoint> endpoint;
    std::shared_ptr<fx::LinkState> state;
    fx::FakeLinkFactory *factory{nullptr};
};

Link open_link() {
    Link link;
    link.state = std::make_shared<fx::LinkState>();
    auto factory = std::make_unique<fx::FakeLinkFactory>(link.state);
    link.factory = factory.get();
    auto endpoint = ua::UsbAoaEndpoint::make(std::move(factory), tr::UsbDeviceSelector{"1-2.1"},
                                             tr::AoaAccessoryInfo{}, admit_all());
    EXPECT_TRUE(endpoint.has_value());
    if (endpoint) {
        EXPECT_TRUE(endpoint.value()->open().has_value());
        link.endpoint = std::move(endpoint.value());
    }
    return link;
}

struct Outcome final {
    bool had_value{true};
    ErrorCode code{ErrorCode::internal};
};

} // namespace

TEST(UsbAoaHotplug, DetachDuringReadFailsTypedAndTerminal) {
    Link link = open_link();
    ASSERT_TRUE(link.endpoint != nullptr);
    fx::FakeBulkPipe *pipe = link.factory->pipe();
    ASSERT_TRUE(pipe != nullptr);
    std::atomic<bool> started{false};
    Outcome outcome;
    std::thread reader([&] {
        std::array<std::byte, 8> buffer{};
        started.store(true);
        auto read = link.endpoint->read(buffer, {});
        outcome.had_value = read.has_value();
        if (!outcome.had_value) {
            outcome.code = read.error().code();
        }
    });
    while (!started.load() || link.state->pipe_state()->reads_started.load() == 0) {
        std::this_thread::yield();
    }
    pipe->detach();
    reader.join();
    EXPECT_FALSE(outcome.had_value);
    EXPECT_EQ(outcome.code, ErrorCode::transport_closed);

    const auto reads_after = link.state->pipe_state()->reads_started.load();
    std::array<std::byte, 8> buffer{};
    auto read = link.endpoint->read(buffer, {});
    ASSERT_FALSE(read.has_value());
    EXPECT_EQ(read.error().code(), ErrorCode::transport_closed);
    EXPECT_EQ(link.state->pipe_state()->reads_started.load(), reads_after);
}

TEST(UsbAoaHotplug, DetachDuringWriteFailsTyped) {
    Link link = open_link();
    ASSERT_TRUE(link.endpoint != nullptr);
    fx::FakeBulkPipe *pipe = link.factory->pipe();
    ASSERT_TRUE(pipe != nullptr);
    pipe->detach();
    const std::array<std::byte, 4> data{std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}};
    auto written = link.endpoint->write(data, {});
    ASSERT_FALSE(written.has_value());
    EXPECT_EQ(written.error().code(), ErrorCode::transport_closed);
}

TEST(UsbAoaHotplug, StaleHandleIsNotReusedAfterDetach) {
    Link link = open_link();
    ASSERT_TRUE(link.endpoint != nullptr);
    fx::FakeBulkPipe *pipe = link.factory->pipe();
    ASSERT_TRUE(pipe != nullptr);
    pipe->detach();
    std::array<std::byte, 8> buffer{};
    ASSERT_FALSE(link.endpoint->read(buffer, {}).has_value());

    Link fresh = open_link();
    ASSERT_TRUE(fresh.endpoint != nullptr);
    auto written = fresh.endpoint->write(buffer, {});
    ASSERT_TRUE(written.has_value());
    EXPECT_EQ(link.state->pipe_state()->out_size(), 0u);
    EXPECT_EQ(fresh.state->pipe_state()->out_size(), buffer.size());
}

TEST(UsbAoaHotplug, TwoDetachCyclesRecoverConsistently) {
    for (int cycle = 0; cycle < 2; ++cycle) {
        Link link = open_link();
        ASSERT_TRUE(link.endpoint != nullptr);
        fx::FakeBulkPipe *pipe = link.factory->pipe();
        ASSERT_TRUE(pipe != nullptr);
        const std::array<std::byte, 2> data{std::byte{0xA5}, std::byte{0x5A}};
        ASSERT_TRUE(link.endpoint->write(data, {}).has_value());
        pipe->detach();
        std::array<std::byte, 2> buffer{};
        auto read = link.endpoint->read(buffer, {});
        ASSERT_FALSE(read.has_value());
        EXPECT_EQ(read.error().code(), ErrorCode::transport_closed);
    }
}

TEST(UsbAoaHotplug, NoPeerReadIsBlockedAndCancellable) {
    Link link = open_link();
    ASSERT_TRUE(link.endpoint != nullptr);
    aa::core::CancellationSource source;
    Outcome outcome;
    PendingIo<void> pending([&] {
        std::array<std::byte, 8> buffer{};
        auto read = link.endpoint->read(buffer, source.get_token());
        outcome.had_value = read.has_value();
        if (!outcome.had_value) {
            outcome.code = read.error().code();
        }
    });
    EXPECT_TRUE(pending.blocked(SYS_futex));
    source.request_stop();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (pending.result.wait_for(std::chrono::milliseconds(1)) != std::future_status::ready) {
        ASSERT_LT(std::chrono::steady_clock::now(), deadline);
    }
    EXPECT_FALSE(outcome.had_value);
    EXPECT_EQ(outcome.code, ErrorCode::cancelled);
}

TEST(UsbAoaHotplug, SwitchTimeDetachIsTypedAndTerminal) {
    auto state = std::make_shared<fx::LinkState>();
    auto factory = std::make_unique<fx::FakeLinkFactory>(state);
    factory->fail_switch = true;
    factory->switch_error = Error{ErrorCode::transport_closed};
    auto endpoint = ua::UsbAoaEndpoint::make(std::move(factory), tr::UsbDeviceSelector{"1-2.1"},
                                             tr::AoaAccessoryInfo{}, admit_all());
    ASSERT_TRUE(endpoint.has_value());
    auto opened = endpoint.value()->open();
    ASSERT_FALSE(opened.has_value());
    EXPECT_EQ(opened.error().code(), ErrorCode::transport_closed);
    EXPECT_EQ(state->claims(), 0);
    EXPECT_EQ(state->switches(), 1);
    auto reopened = endpoint.value()->open();
    ASSERT_FALSE(reopened.has_value());
    EXPECT_EQ(reopened.error().code(), ErrorCode::transport_closed);
}

TEST(UsbAoaHotplug, MissingAccessoryIsTypedTimeout) {
    auto state = std::make_shared<fx::LinkState>();
    auto factory = std::make_unique<fx::FakeLinkFactory>(state);
    factory->fail_claim = true;
    factory->claim_error = Error{ErrorCode::timeout};
    auto endpoint = ua::UsbAoaEndpoint::make(std::move(factory), tr::UsbDeviceSelector{"1-2.1"},
                                             tr::AoaAccessoryInfo{}, admit_all());
    ASSERT_TRUE(endpoint.has_value());
    auto opened = endpoint.value()->open();
    ASSERT_FALSE(opened.has_value());
    EXPECT_EQ(opened.error().code(), ErrorCode::timeout);
    EXPECT_EQ(state->claims(), 1);
}
