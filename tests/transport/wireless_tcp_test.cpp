// SPDX-License-Identifier: GPL-3.0-or-later
#include <aa/transport/TcpTransport.hpp>
#include <aa/transport/Framing.hpp>
#include "../../src/transport/socket_util.hpp"
#include <gtest/gtest.h>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

using namespace std::chrono_literals;

TEST(WirelessTcp, SilentReceiveServicesOwnerAndExpires) {
    // Given: a silent TCP peer and an owner-side deadline, with a safety watchdog.
    auto pair = aa::transport::make_tcp_loopback();
    ASSERT_TRUE(pair);
    aa::core::CancellationSource cancel;
    ASSERT_TRUE(pair.value().server->open(cancel.get_token()));
    unsigned checkpoints{};
    pair.value().server->set_io_checkpoint([&] { ++checkpoints; });
    const auto began = std::chrono::steady_clock::now();
    pair.value().server->set_io_deadline(began + 150ms);
    std::jthread watchdog([&](const std::stop_token& stop) {
        std::mutex mutex;
        std::condition_variable_any cv;
        std::unique_lock lock{mutex};
        cv.wait_for(lock, stop, 1500ms, [] { return false; });
        if (!stop.stop_requested()) cancel.request_stop();
    });
    // When: receive waits on the silent peer.
    const auto received = pair.value().server->receive();
    const auto elapsed = std::chrono::steady_clock::now() - began;
    watchdog.request_stop();
    // Then: owner work was serviced and the deadline, not the watchdog, ended I/O.
    ASSERT_FALSE(received);
    EXPECT_EQ(received.error().code(), aa::ErrorCode::timeout);
    EXPECT_GT(checkpoints, 0U);
    EXPECT_LT(elapsed, 1s);
}

TEST(WirelessTcp, IdleAcceptCancellationAndStopAreBounded) {
    auto listener = aa::transport::TcpListener::listen("127.0.0.1", 0);
    ASSERT_TRUE(listener);
    aa::core::CancellationSource cancel;
    const auto began = std::chrono::steady_clock::now();
    auto accepted = listener.value()->accept({began + 1s, cancel.get_token()}, [&] {
        if (std::chrono::steady_clock::now() - began >= 75ms) cancel.request_stop();
    });
    ASSERT_FALSE(accepted);
    EXPECT_EQ(accepted.error().code(), aa::ErrorCode::cancelled);
    listener.value()->close();
    EXPECT_LT(std::chrono::steady_clock::now() - began, 1s);
}

TEST(WirelessTcp, BackpressuredSendServicesControlAndStopsWithinFiveSeconds) {
    auto pair = aa::transport::make_tcp_loopback();
    ASSERT_TRUE(pair);
    aa::core::CancellationSource cancel;
    ASSERT_TRUE(pair.value().server->open(cancel.get_token()));
    const auto began = std::chrono::steady_clock::now();
    unsigned checkpoints{};
    pair.value().server->set_io_checkpoint([&] {
        ++checkpoints;
        if (std::chrono::steady_clock::now() - began >= 200ms) cancel.request_stop();
    });
    auto frames = aa::transport::encode_message({0, aa::transport::MessageKind::specific,
        std::vector<std::byte>(16'000)}, aa::transport::Encryption::plain, nullptr);
    ASSERT_TRUE(frames);
    std::size_t sent{};
    for (;;) {
        const auto result = pair.value().server->send(frames.value().front());
        if (!result) { EXPECT_EQ(result.error().code(), aa::ErrorCode::cancelled); break; }
        sent += frames.value().front().size();
    }
    EXPECT_GT(sent, 64'000U);
    EXPECT_GT(checkpoints, 1U);
    pair.value().server->close();
    EXPECT_LT(std::chrono::steady_clock::now() - began, 5s);
}
TEST(WirelessTcp, PartialAapHeaderStillServicesControlAndCancels) {
    int listener = aa::transport::socket_util::listen_socket("127.0.0.1", 0);
    ASSERT_GE(listener, 0);
    auto client = aa::transport::TcpTransport::connect("127.0.0.1", aa::transport::socket_util::bound_port(listener));
    ASSERT_TRUE(client);
    int peer = aa::transport::socket_util::accept_socket(listener); ASSERT_GE(peer, 0);
    const std::array<std::byte, 2> partial{std::byte{0}, std::byte{3}};
    ASSERT_EQ(::send(peer, partial.data(), partial.size(), MSG_NOSIGNAL), 2);
    aa::core::CancellationSource cancel;
    ASSERT_TRUE(client.value()->open(cancel.get_token()));
    const auto began = std::chrono::steady_clock::now();
    unsigned checkpoints{};
    client.value()->set_io_checkpoint([&] {
        ++checkpoints;
        if (std::chrono::steady_clock::now() - began >= 75ms) cancel.request_stop();
    });
    const auto received = client.value()->receive();
    EXPECT_FALSE(received);
    if (!received) { EXPECT_EQ(received.error().code(), aa::ErrorCode::cancelled); }
    EXPECT_GT(checkpoints, 1U); EXPECT_LT(std::chrono::steady_clock::now() - began, 1s);
    client.value()->close();
    aa::transport::socket_util::close_fd(peer); aa::transport::socket_util::close_fd(listener);
}
