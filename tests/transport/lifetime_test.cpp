// SPDX-License-Identifier: GPL-3.0-or-later
#include <aa/transport/ByteEndpoint.hpp>
#include <aa/transport/Frames.hpp>
#include <aa/transport/TcpTransport.hpp>
#include <aa/transport/UsbByteTransport.hpp>

#include <array>
#include <gtest/gtest.h>

namespace tr = aa::transport;

TEST(UsbByteTransport, CancelledSendDoesNotEnqueueBytes) {
    // Given: a cancelled USB session with an empty peer queue.
    auto pair = tr::make_loopback();
    ASSERT_TRUE(pair);
    auto sender = tr::UsbByteTransport::over_endpoint(std::move(pair.value().a));
    ASSERT_TRUE(sender);
    aa::core::CancellationSource source;
    ASSERT_TRUE(sender.value()->open(source.get_token()));
    source.request_stop();
    const std::array frame{std::byte{1}, std::byte{3}, std::byte{0}, std::byte{1}, std::byte{42}};
    // When: sending a valid frame after cancellation.
    auto sent = sender.value()->send(frame);
    // Then: no bytes reach the peer; it observes the sender's terminal close.
    ASSERT_FALSE(sent);
    EXPECT_EQ(sent.error().code(), aa::ErrorCode::cancelled);
    std::array<std::byte, 8> buffer{};
    auto read = pair.value().b->read(buffer, {});
    ASSERT_TRUE(read);
    EXPECT_EQ(read.value(), 0u);
}

TEST(TcpTransport, PeerEofClosesReceiveAndSend) {
    // Given: a connected peer that has closed its owned socket.
    auto pair = tr::make_tcp_loopback();
    ASSERT_TRUE(pair);
    pair.value().server->close();
    // When: receiving EOF.
    auto received = pair.value().client->receive();
    // Then: this session is terminal for subsequent operations.
    ASSERT_FALSE(received);
    EXPECT_EQ(received.error().code(), aa::ErrorCode::transport_closed);
    const std::array frame{std::byte{1}, std::byte{3}, std::byte{0}, std::byte{0}};
    auto sent = pair.value().client->send(frame);
    ASSERT_FALSE(sent);
    EXPECT_EQ(sent.error().code(), aa::ErrorCode::transport_closed);
}
