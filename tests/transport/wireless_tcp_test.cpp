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

// Wireless audio (window 8): the A26 sends media audio stop-and-wait
// (max_unacked 1), so its throughput is one 42.67 ms chunk per credit round
// trip, and the receiver's credit acks are small frames. With Nagle on, a small
// frame written while an earlier small frame is still unacknowledged is held
// until the peer's delayed ACK (~40 ms), and video and audio acks interleave
// exactly like that. The phone measured 55-89 ms ack round trips, which is
// slower than real time. Two back-to-back small frames from either end of a
// wireless TCP link must reach a silent peer without waiting for its ACK.
TEST(WirelessTcp, SmallFramesAreNotHeldBehindAnUnacknowledgedFrame) {
    auto pair = aa::transport::make_tcp_loopback();
    ASSERT_TRUE(pair);
    aa::core::CancellationSource cancel;
    auto& accepted = *pair.value().server;
    auto& connected = *pair.value().client;
    ASSERT_TRUE(accepted.open(cancel.get_token()));
    ASSERT_TRUE(connected.open(cancel.get_token()));
    auto ack = aa::transport::encode_message({3, aa::transport::MessageKind::specific,
        std::vector<std::byte>(6, std::byte{1})}, aa::transport::Encryption::plain, nullptr);
    ASSERT_TRUE(ack);
    ASSERT_EQ(ack.value().size(), 1U);
    const auto& frame = ack.value().front();
    constexpr int kRounds = 8;
    const auto held_rounds = [&](aa::transport::TcpTransport& sender,
                                 aa::transport::TcpTransport& receiver) {
        int held = 0;
        for (int round = 0; round < kRounds; ++round) {
            const auto began = std::chrono::steady_clock::now();
            EXPECT_TRUE(sender.send(frame));
            EXPECT_TRUE(sender.send(frame));
            EXPECT_TRUE(receiver.receive());
            EXPECT_TRUE(receiver.receive());
            if (std::chrono::steady_clock::now() - began > 20ms) { ++held; }
            // The peer answers each pair, as the phone answers each ack with
            // its next chunk: an interactive peer delays its own TCP ACKs.
            EXPECT_TRUE(receiver.send(frame));
            EXPECT_TRUE(sender.receive());
        }
        return held;
    };
    // The receiver of the HU's acks is the phone: here the connected end
    // reads what the accepted (HU) end sends, then the reverse.
    const int held_from_accepted = held_rounds(accepted, connected);
    const int held_from_connected = held_rounds(connected, accepted);
    // Nagle holds the second frame of every round after the first for the
    // peer's delayed ACK (~44 ms on loopback); without it a round takes
    // microseconds, so a stray scheduler delay stays rare.
    EXPECT_LT(held_from_accepted, kRounds / 2) << "accepted socket holds small frames (Nagle)";
    EXPECT_LT(held_from_connected, kRounds / 2) << "connected socket holds small frames (Nagle)";
}
