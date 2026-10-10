// SPDX-License-Identifier: GPL-3.0-or-later
// TLS pending-I/O proofs (task 16 follow-up): an in-flight TLS send under real
// socket backpressure and an in-flight TLS receive are observed inside the
// kernel poll wait via PendingIo before cancellation fires, so neither test can
// pass on an immediate pre-start stop. The send peer stops consuming after the
// handshake: the wait is backpressure, not EOF.
#include "PendingIo.hpp"

#include <aa/core/Cancellation.hpp>
#include <aa/transport/Frames.hpp>
#include <aa/transport/TlsTransport.hpp>

#include <atomic>
#include <cstddef>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <gtest/gtest.h>

namespace tr = aa::transport;
using namespace std::chrono_literals;

TEST(TlsInFlight, SendObservedUnderBackpressure) {
    // Given: a handshaked TLS session whose peer never consumes frames, with the
    // client socket shrunk so backpressure is deterministic and quick.
    auto listener = tr::TlsListener::listen("127.0.0.1", 0, [] { return true; },
                                            tr::TlsMode::encryption_only_compatibility);
    ASSERT_TRUE(listener);
    const auto port = listener.value()->port();
    ASSERT_NE(port, 0);
    auto client = tr::TlsTransport::connect("127.0.0.1", port, [] { return true; },
                                            tr::TlsMode::encryption_only_compatibility);
    ASSERT_TRUE(client);
    auto server = listener.value()->accept();
    ASSERT_TRUE(server);
    listener.value()->close();
    // Identify the client socket exactly by TCP getpeername (its peer is the
    // listener port), the same way the TCP backpressure proof finds its fd.
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
    bool client_ok = false;
    bool server_ok = false;
    std::thread client_thread(
        [&] { client_ok = client.value()->open(source.get_token()).has_value(); });
    std::thread server_thread([&] { server_ok = server.value()->open({}).has_value(); });
    client_thread.join();
    server_thread.join();
    ASSERT_TRUE(client_ok && server_ok);
    // The session-open token is valid: the handshake ran under it and nothing
    // has stopped it; the sends below ride that same stored token.
    ASSERT_FALSE(source.stop_requested());

    // One TLS record per frame: a 16KB-payload frame splits into two records and
    // the second can transiently EAGAIN (delayed ACK), observing poll before any
    // whole frame completed. Single-record frames complete many times (32KB of
    // measured loopback capacity) before genuine backpressure, so completed>0
    // is deterministic while the peer never consumes.
    auto frame = tr::encode_frame({1, tr::FrameType::bulk, tr::MessageKind::specific,
                                   tr::Encryption::plain},
                                  std::vector<std::byte>(1024, std::byte{3}), 0);
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
    // Then: the backpressured TLS send is cancelled with a typed error within
    // 1s, after whole frames completed before the buffers filled.
    EXPECT_TRUE(blocked);
    EXPECT_GT(completed.load(), 0u);
    ASSERT_EQ(pending.result.wait_for(1s), std::future_status::ready);
    auto result = pending.result.get();
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code(), aa::ErrorCode::cancelled);
}

TEST(TlsInFlight, ReceiveObservedInsidePoll) {
    // Given: a handshaked TLS session with no inbound data and a live session
    // token (kernel-observed strengthening of the start-flag receive test).
    auto pair = tr::make_tls_loopback([] { return true; },
                                      tr::TlsMode::encryption_only_compatibility);
    ASSERT_TRUE(pair);
    aa::core::CancellationSource source;
    bool client_ok = false;
    bool server_ok = false;
    std::thread client_thread(
        [&] { client_ok = pair.value().client->open(source.get_token()).has_value(); });
    std::thread server_thread([&] { server_ok = pair.value().server->open({}).has_value(); });
    client_thread.join();
    server_thread.join();
    ASSERT_TRUE(client_ok && server_ok);
    ASSERT_FALSE(source.stop_requested());
    PendingIo pending([&] { return pair.value().client->receive(); });
    const bool blocked = pending.blocked(SYS_poll);
    // When: cancellation fires after the kernel wait is observed.
    source.request_stop();
    // Then: the pending TLS read is cancelled within a bounded interval.
    EXPECT_TRUE(blocked);
    ASSERT_EQ(pending.result.wait_for(1s), std::future_status::ready);
    auto result = pending.result.get();
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code(), aa::ErrorCode::cancelled);
}
