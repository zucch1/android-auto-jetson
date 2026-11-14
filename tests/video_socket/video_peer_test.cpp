// SPDX-License-Identifier: GPL-3.0-or-later
#include "pair.hpp"

#include <aa/ipc/VideoChannel.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include <gtest/gtest.h>

namespace {
using namespace aa::ipc;

std::vector<std::uint8_t> one_byte() { return std::vector<std::uint8_t>{0x5a}; }
} // namespace

TEST(VideoPeer, ServerRejectsDifferentUidConsumer) {
    NegotiatedLimits limits{};
    const std::string runtime = videotest::private_runtime_dir();
    ASSERT_FALSE(runtime.empty());
    const auto path = video_socket_path_in(runtime);
    const auto self = current_uid();
    auto server = VideoServer::listen(path, VideoServerOptions{limits, self + 1});
    ASSERT_TRUE(server);

    aa::core::Result<VideoConsumer> client{VideoConsumer{}};
    std::thread connector([&] {
        client = connect_video_consumer(
            path, VideoClientOptions{limits, self, aa::core::Milliseconds{2000}});
    });
    const auto accepted = server.value().accept_consumer(aa::core::Milliseconds{2000});
    connector.join();
    ASSERT_FALSE(accepted);
    EXPECT_EQ(accepted.error().code(), aa::ErrorCode::transport_unauthorized_peer);
    EXPECT_EQ(server.value().unauthorized_peers(), 1U);
    EXPECT_FALSE(server.value().closed());
    server.value().close();
    ::rmdir(runtime.c_str());
}

TEST(VideoPeer, ClientRejectsDifferentUidServer) {
    NegotiatedLimits limits{};
    const std::string runtime = videotest::private_runtime_dir();
    ASSERT_FALSE(runtime.empty());
    const auto path = video_socket_path_in(runtime);
    const auto self = current_uid();
    auto server = VideoServer::listen(path, VideoServerOptions{limits, self + 1});
    ASSERT_TRUE(server);

    aa::core::Result<VideoConsumer> client{VideoConsumer{}};
    std::thread connector([&] {
        client = connect_video_consumer(
            path, VideoClientOptions{limits, self + 1, aa::core::Milliseconds{2000}});
    });
    auto accepted = server.value().accept_consumer(aa::core::Milliseconds{2000});
    connector.join();
    ASSERT_FALSE(accepted);
    EXPECT_EQ(accepted.error().code(), aa::ErrorCode::transport_unauthorized_peer);
    server.value().close();
    ::rmdir(runtime.c_str());
}

TEST(VideoPeer, SameUidPairStreamsAndCountsNobodyUnauthorized) {
    NegotiatedLimits limits{};
    auto pair = videotest::make_pair(limits);
    ASSERT_TRUE(pair);
    EXPECT_EQ(pair.value().server.unauthorized_peers(), 0U);
    ASSERT_TRUE(pair.value().producer.send_access_unit({1, 1, true, one_byte()}));
    const auto step = pair.value().consumer.receive(aa::core::Milliseconds{2000});
    ASSERT_TRUE(step);
    EXPECT_EQ(step.value(), ReceiveStatus::complete);
}

TEST(VideoPeer, MissingSocketIsTransportUnavailable) {
    const auto path = video_socket_path_in(videotest::private_runtime_dir());
    const auto client =
        connect_video_consumer(path, VideoClientOptions{{}, current_uid(), aa::core::Milliseconds{200}});
    ASSERT_FALSE(client);
    EXPECT_EQ(client.error().code(), aa::ErrorCode::transport_unavailable);
}

// ---- Audit L11 ---------------------------------------------------------------

namespace {
int raw_silent_connect(const std::string& path) {
    const int fd = ::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        return -1;
    }
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        ::close(fd);
        return -1;
    }
    return fd;
}
} // namespace

TEST(VideoPeer, EndpointIsOwnerOnlyWithoutChangingTheProcessUmask) {
    // Given: a process umask that would make files world-readable, and another
    // thread creating files for the whole duration of repeated listens.
    const mode_t original = ::umask(0022);
    const std::string files_dir = videotest::private_runtime_dir();
    ASSERT_FALSE(files_dir.empty());
    std::atomic<bool> stop{false};
    std::atomic<int> wrong_mode{0};
    std::atomic<int> created{0};
    std::thread creator([&] {
        while (!stop.load()) {
            const std::string file = files_dir + "/f" + std::to_string(created.load());
            const int fd = ::open(file.c_str(), O_CREAT | O_WRONLY | O_CLOEXEC, 0666);
            if (fd < 0) {
                continue;
            }
            struct stat info {};
            if (::fstat(fd, &info) != 0 || (info.st_mode & 0777) != 0644) {
                ++wrong_mode;
            }
            ::close(fd);
            ++created;
        }
    });
    NegotiatedLimits limits{};
    for (int i = 0; i < 100; ++i) {
        const std::string runtime = videotest::private_runtime_dir();
        ASSERT_FALSE(runtime.empty());
        const auto path = video_socket_path_in(runtime);
        auto server = VideoServer::listen(path, VideoServerOptions{limits, current_uid()});
        ASSERT_TRUE(server);
        // Then: the endpoint is exactly 0600.
        struct stat info {};
        ASSERT_EQ(::lstat(path.value.c_str(), &info), 0);
        EXPECT_TRUE(S_ISSOCK(info.st_mode));
        EXPECT_EQ(info.st_mode & 0777, 0600U);
        server.value().close();
        videotest::remove_startup_lock(path.value);
        ::rmdir(runtime.c_str());
    }
    stop.store(true);
    creator.join();
    // And: no concurrently created file was affected, and the umask is untouched.
    EXPECT_GT(created.load(), 0);
    EXPECT_EQ(wrong_mode.load(), 0);
    EXPECT_EQ(::umask(original), 0022U);
}

TEST(VideoPeer, SilentPeerHandshakeIsBoundedAndTheNextConsumerIsAccepted) {
    // Given: a listening server and a same-UID peer that connects and never speaks.
    NegotiatedLimits limits{};
    const std::string runtime = videotest::private_runtime_dir();
    ASSERT_FALSE(runtime.empty());
    const auto path = video_socket_path_in(runtime);
    auto server = VideoServer::listen(path, VideoServerOptions{limits, current_uid()});
    ASSERT_TRUE(server);
    const int silent = raw_silent_connect(path.value);
    ASSERT_GE(silent, 0);

    // When: the accept loop is given a 30 s budget.
    const auto started = std::chrono::steady_clock::now();
    const auto refused = server.value().accept_consumer(aa::core::Milliseconds{30000});
    const auto elapsed = std::chrono::steady_clock::now() - started;

    // Then: the handshake gives up at its own bound, not the caller's budget.
    ASSERT_FALSE(refused);
    EXPECT_GE(elapsed, std::chrono::milliseconds{1500});
    EXPECT_LT(elapsed, std::chrono::milliseconds{5000});
    ::close(silent);

    // And: the next, well-behaved consumer is accepted on the same server.
    aa::core::Result<VideoConsumer> client{VideoConsumer{}};
    std::thread connector([&] {
        client = connect_video_consumer(
            path, VideoClientOptions{limits, current_uid(), aa::core::Milliseconds{2000}});
    });
    const auto accepted = server.value().accept_consumer(aa::core::Milliseconds{2000});
    connector.join();
    EXPECT_TRUE(accepted);
    EXPECT_TRUE(client);
    server.value().close();
    videotest::remove_startup_lock(path.value);
    ::rmdir(runtime.c_str());
}
