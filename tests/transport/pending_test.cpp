// SPDX-License-Identifier: GPL-3.0-or-later
#include "PendingIo.hpp"
#include "../../src/transport/socket_util.hpp"
#include <aa/transport/Frames.hpp>
#include <aa/transport/TcpTransport.hpp>
#include <aa/transport/UsbByteTransport.hpp>
#include <array>
#include <atomic>
#include <gtest/gtest.h>

namespace tr = aa::transport;
using namespace std::chrono_literals;

TEST(InFlightCancellation, TcpReceiveObservedInsidePoll) {
    // Given: an idle connected TCP session.
    auto pair = tr::make_tcp_loopback();
    ASSERT_TRUE(pair);
    aa::core::CancellationSource source;
    ASSERT_TRUE(pair.value().client->open(source.get_token()));
    PendingIo pending([&] { return pair.value().client->receive(); });
    const bool blocked = pending.blocked(SYS_poll);
    // When: cancellation fires after the kernel wait is observed.
    source.request_stop();
    // Then: the pending read is cancelled within a bounded interval.
    EXPECT_TRUE(blocked);
    ASSERT_EQ(pending.result.wait_for(1s), std::future_status::ready);
    auto result = pending.result.get();
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code(), aa::ErrorCode::cancelled);
}

TEST(InFlightCancellation, UsbReceiveObservedInsideFutex) {
    // Given: an idle USB session waiting on its condition variable.
    auto pair = tr::make_loopback();
    ASSERT_TRUE(pair);
    auto receiver = tr::UsbByteTransport::over_endpoint(std::move(pair.value().b));
    ASSERT_TRUE(receiver);
    aa::core::CancellationSource source;
    ASSERT_TRUE(receiver.value()->open(source.get_token()));
    PendingIo pending([&] { return receiver.value()->receive(); });
    const bool blocked = pending.blocked(SYS_futex);
    // When: cancellation fires after the kernel wait is observed.
    source.request_stop();
    // Then: the pending read is cancelled within a bounded interval.
    EXPECT_TRUE(blocked);
    ASSERT_EQ(pending.result.wait_for(1s), std::future_status::ready);
    auto result = pending.result.get();
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code(), aa::ErrorCode::cancelled);
}

TEST(InFlightCancellation, EndpointLocalCloseWakesPendingRead) {
    // Given: an endpoint with an actual pending read, independent of Transport.
    auto pair = tr::make_loopback();
    ASSERT_TRUE(pair);
    std::array<std::byte, 8> buffer{};
    PendingIo pending([&] { return pair.value().b->read(buffer, {}); });
    const bool blocked = pending.blocked(SYS_futex);
    // When: the endpoint closes under its reader's mutex.
    pair.value().b->close();
    // Then: the read wakes and observes local EOF without cancellation.
    EXPECT_TRUE(blocked);
    ASSERT_EQ(pending.result.wait_for(1s), std::future_status::ready);
    auto result = pending.result.get();
    ASSERT_TRUE(result);
    EXPECT_EQ(result.value(), 0u);
}

TEST(InFlightCancellation, BlockingSocketWriteReturnsPartialWithoutBlocking) {
    // Given: a blocking socket with far less capacity than the submitted data.
    std::array<int, 2> sockets{};
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, sockets.data()), 0);
    const int size = 4096;
    ASSERT_EQ(::setsockopt(sockets[0], SOL_SOCKET, SO_SNDBUF, &size, sizeof(size)), 0);
    const std::vector<std::byte> data(std::size_t{8} * 1024u * 1024u, std::byte{42});
    aa::core::CancellationSource source;
    PendingIo pending([&] { return tr::socket_util::write_fd(sockets[0], data, source.get_token()); });
    // When: writing once without a draining peer.
    const auto status = pending.result.wait_for(1s);
    if (status != std::future_status::ready) {
        source.request_stop();
        ::shutdown(sockets[1], SHUT_RDWR);
    }
    auto result = pending.result.get();
    tr::socket_util::close_fd(sockets[0]);
    tr::socket_util::close_fd(sockets[1]);
    // Then: nonblocking syscall semantics return a positive partial write.
    ASSERT_EQ(status, std::future_status::ready);
    ASSERT_TRUE(result);
    EXPECT_GT(result.value(), 0u);
    EXPECT_LT(result.value(), data.size());
}

TEST(InFlightCancellation, TcpSendObservedUnderBackpressure) {
    // Given: a TCP peer that never consumes incoming frames.
    auto listener = tr::socket_util::listen_socket("127.0.0.1", 0);
    ASSERT_GE(listener, 0);
    const auto port = tr::socket_util::bound_port(listener);
    auto client = tr::TcpTransport::connect("127.0.0.1", port);
    ASSERT_TRUE(client);
    int peer = tr::socket_util::accept_socket(listener);
    ASSERT_GE(peer, 0);
    int client_fd = -1;
    for (int fd = 0; fd < 1024; ++fd) {
        sockaddr_in address{};
        socklen_t length = sizeof(address);
        if (::getpeername(fd, reinterpret_cast<sockaddr*>(&address), &length) == 0 &&
            address.sin_family == AF_INET && ntohs(address.sin_port) == port) {
            client_fd = fd;
            break;
        }
    }
    ASSERT_GE(client_fd, 0);
    const int size = 4096;
    ASSERT_EQ(::setsockopt(client_fd, SOL_SOCKET, SO_SNDBUF, &size, sizeof(size)), 0);
    aa::core::CancellationSource source;
    ASSERT_TRUE(client.value()->open(source.get_token()));
    auto frame = tr::encode_frame({1, tr::FrameType::bulk, tr::MessageKind::specific,
                                  tr::Encryption::plain}, std::vector<std::byte>(16384), 0);
    ASSERT_TRUE(frame);
    std::atomic<unsigned> completed{0};
    PendingIo pending([&]() -> aa::core::Result<void> {
        for (;;) {
            auto result = client.value()->send(frame.value());
            if (!result) {
                return result;
            }
            ++completed;
        }
    });
    const bool blocked = pending.blocked(SYS_poll, client_fd);
    // When: stopping a send observed in poll while its socket is not writable.
    source.request_stop();
    // Then: actual backpressure is cancelled, not an immediate pre-start stop.
    EXPECT_TRUE(blocked);
    EXPECT_GT(completed.load(), 0u);
    ASSERT_EQ(pending.result.wait_for(1s), std::future_status::ready);
    auto result = pending.result.get();
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code(), aa::ErrorCode::cancelled);
    tr::socket_util::close_fd(peer);
    tr::socket_util::close_fd(listener);
}
