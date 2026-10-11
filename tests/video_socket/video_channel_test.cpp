// SPDX-License-Identifier: GPL-3.0-or-later
#include "pair.hpp"

#include <aa/ipc/VideoChannel.hpp>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <vector>

#include <gtest/gtest.h>

namespace {
using namespace aa::ipc;

std::vector<std::uint8_t> pattern(std::size_t size) {
    std::vector<std::uint8_t> bytes(size, 0);
    for (std::size_t i = 0; i < size; ++i) {
        bytes[i] = static_cast<std::uint8_t>(i % 251);
    }
    return bytes;
}

aa::core::Result<ReceiveStatus> drain_until_complete(VideoConsumer& consumer, int attempts) {
    for (int i = 0; i < attempts; ++i) {
        auto step = consumer.receive(aa::core::Milliseconds{2000});
        if (!step) {
            return step;
        }
        if (step.value() == ReceiveStatus::complete) {
            return step;
        }
    }
    return aa::Error{aa::ErrorCode::timeout};
}

// A closed-but-unaccepted AF_UNIX connection stays in the accept queue and
// keeps holding a backlog slot (the queue drains only via accept), so reap
// whatever the raw filler connects left behind before the next handshake.
void reap_queued_connections(VideoServer& server) {
    for (;;) {
        pollfd ready{server.native_handle(), POLLIN, 0};
        if (::poll(&ready, 1, 100) <= 0 || (ready.revents & POLLIN) == 0) {
            return;
        }
        const int fd = ::accept4(server.native_handle(), nullptr, nullptr, SOCK_CLOEXEC);
        if (fd >= 0) {
            ::close(fd);
        }
    }
}

struct BacklogSaturation final {
    std::vector<int> fillers{};
    bool queue_refused{false};
    int unexpected_errno{0};
};

// Saturate the listen backlog with raw nonblocking connects until the kernel
// refuses to queue another. The admitted count is a kernel backlog detail
// (listen(fd,1) still admits more than one), so the loop discovers the bound
// instead of hardcoding how many connects fit.
BacklogSaturation saturate_backlog(const VideoSocketPath& path) {
    BacklogSaturation out;
    for (int attempt = 0; attempt < 8 && !out.queue_refused; ++attempt) {
        const int fd = ::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
        if (fd < 0) {
            out.unexpected_errno = errno;
            return out;
        }
        sockaddr_un address{};
        address.sun_family = AF_UNIX;
        std::memcpy(address.sun_path, path.value.c_str(), path.value.size() + 1);
        if (::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0) {
            out.fillers.push_back(fd);
            continue;
        }
        const int failure = errno;
        ::close(fd);
        if (failure == EAGAIN || failure == EWOULDBLOCK) {
            out.queue_refused = true;
        } else {
            out.unexpected_errno = failure;
            return out;
        }
    }
    return out;
}
} // namespace

TEST(VideoStream, SocketPathAndDirectoryPreparation) {
    const auto runtime = videotest::private_runtime_dir();
    ASSERT_FALSE(runtime.empty());
    const auto path = video_socket_path_in(runtime);
    EXPECT_NE(path.value.find("android-auto-receiver/video.sock"), std::string::npos);
    ASSERT_TRUE(prepare_video_socket_path(path));
    const std::string directory = path.value.substr(0, path.value.find_last_of('/'));
    struct stat info {};
    ASSERT_EQ(::stat(directory.c_str(), &info), 0);
    EXPECT_EQ(info.st_mode & 0777, 0700U);
    auto server = VideoServer::listen(path, VideoServerOptions{{}, current_uid()});
    ASSERT_TRUE(server);
    ASSERT_EQ(::stat(path.value.c_str(), &info), 0);
    EXPECT_NE(info.st_mode & S_IFSOCK, 0U);
    EXPECT_EQ(info.st_mode & 0777, 0600U);
    server.value().close();
    videotest::remove_startup_lock(path.value);
    ::rmdir(directory.c_str());
    ::rmdir(runtime.c_str());
}

TEST(VideoStream, HappyEncodedStreamWithFragmentedIdr) {
    NegotiatedLimits limits{};
    auto pair = videotest::make_pair(limits);
    ASSERT_TRUE(pair);
    auto& producer = pair.value().producer;
    auto& consumer = pair.value().consumer;
    EXPECT_EQ(producer.limits(), limits);
    EXPECT_EQ(consumer.limits(), limits);
    EXPECT_TRUE(consumer.waiting_idr());

    const auto idr_payload = pattern(kMaxDatagramPayload * 2 + 7);
    ASSERT_TRUE(producer.send_access_unit({1, 100, true, idr_payload}));
    auto step = drain_until_complete(consumer, 4);
    ASSERT_TRUE(step) << static_cast<int>(step.error().code());
    EXPECT_EQ(step.value(), ReceiveStatus::complete);
    const auto& unit = consumer.access_unit();
    EXPECT_EQ(unit.frame, 1U);
    EXPECT_EQ(unit.timestamp_ns, 100U);
    EXPECT_TRUE(unit.idr);
    EXPECT_EQ(unit.payload.size(), idr_payload.size());
    EXPECT_TRUE(std::equal(idr_payload.begin(), idr_payload.end(), unit.payload.begin()));
    EXPECT_FALSE(consumer.waiting_idr());
    EXPECT_EQ(consumer.resyncs(), 1U);

    const auto p_payload = pattern(64);
    ASSERT_TRUE(producer.send_access_unit({2, 200, false, p_payload}));
    step = drain_until_complete(consumer, 2);
    ASSERT_TRUE(step);
    EXPECT_FALSE(consumer.access_unit().idr);
    EXPECT_EQ(consumer.access_unit().frame, 2U);
    EXPECT_EQ(consumer.access_unit().payload, p_payload);
}

TEST(VideoStream, ProducerCloseDeliversCleanEndOfStream) {
    NegotiatedLimits limits{};
    auto pair = videotest::make_pair(limits);
    ASSERT_TRUE(pair);
    const auto payload = pattern(32);
    ASSERT_TRUE(pair.value().producer.send_access_unit({1, 1, true, payload}));
    ASSERT_TRUE(drain_until_complete(pair.value().consumer, 2));
    pair.value().producer.close();
    const auto eof = pair.value().consumer.receive(aa::core::Milliseconds{2000});
    ASSERT_FALSE(eof);
    EXPECT_EQ(eof.error().code(), aa::ErrorCode::transport_closed);
    EXPECT_TRUE(pair.value().consumer.closed() || !pair.value().producer.closed() == false);
}

TEST(VideoStream, NegotiatedLimitsAreFieldMinimum) {
    NegotiatedLimits server_side{8192, 262144, 65536};
    NegotiatedLimits client_side{4096, 131072, 32768};
    auto pair = videotest::make_pair(server_side);
    ASSERT_TRUE(pair);
    EXPECT_EQ(pair.value().producer.limits().max_datagram_payload, 8192U);
    auto second = videotest::make_pair(client_side);
    ASSERT_TRUE(second);
    EXPECT_EQ(second.value().consumer.limits().max_datagram_payload, 4096U);
    EXPECT_EQ(second.value().consumer.limits().max_access_unit_bytes, 131072U);
    EXPECT_EQ(second.value().consumer.limits().socket_budget_bytes, 32768U);
}

TEST(VideoStream, CloseIsIdempotentAndCleansUp) {
    NegotiatedLimits limits{};
    auto pair = videotest::make_pair(limits);
    ASSERT_TRUE(pair);
    const auto path = pair.value().server.path();
    pair.value().producer.close();
    pair.value().producer.close();
    pair.value().consumer.close();
    pair.value().consumer.close();
    pair.value().server.close();
    pair.value().server.close();
    EXPECT_TRUE(pair.value().server.closed());
    struct stat info {};
    EXPECT_NE(::stat(path.value.c_str(), &info), 0);
    const auto closed_send =
        pair.value().producer.send_access_unit({1, 1, true, std::vector<std::uint8_t>{1}});
    ASSERT_FALSE(closed_send);
    EXPECT_EQ(closed_send.error().code(), aa::ErrorCode::transport_closed);
    const auto closed_receive = pair.value().consumer.receive(aa::core::Milliseconds{0});
    ASSERT_FALSE(closed_receive);
    EXPECT_EQ(closed_receive.error().code(), aa::ErrorCode::transport_closed);
}

TEST(VideoStream, RejectsEmptyAndOversizeAccessUnits) {
    NegotiatedLimits limits{};
    limits.max_access_unit_bytes = 1024;
    auto pair = videotest::make_pair(limits);
    ASSERT_TRUE(pair);
    const auto empty =
        pair.value().producer.send_access_unit({1, 1, true, std::span<const std::uint8_t>{}});
    ASSERT_FALSE(empty);
    EXPECT_EQ(empty.error().code(), aa::ErrorCode::invalid_argument);
    const auto oversize = pair.value().producer.send_access_unit({2, 2, true, pattern(1025)});
    ASSERT_FALSE(oversize);
    EXPECT_EQ(oversize.error().code(), aa::ErrorCode::transport_oversize_frame);
}

TEST(VideoStream, ConnectDeadlineIsEnforcedOnSaturatedBacklog) {
    NegotiatedLimits limits{};
    const auto runtime = videotest::private_runtime_dir();
    ASSERT_FALSE(runtime.empty());
    const auto path = video_socket_path_in(runtime);
    auto server = VideoServer::listen(path, VideoServerOptions{limits, current_uid()});
    ASSERT_TRUE(server);

    const auto backlog = saturate_backlog(path);
    EXPECT_EQ(backlog.unexpected_errno, 0) << "unexpected filler connect failure";
    EXPECT_FALSE(backlog.fillers.empty()) << "backlog admitted no connection";
    ASSERT_TRUE(backlog.queue_refused) << "backlog never saturated";

    const auto start = std::chrono::steady_clock::now();
    const auto client = connect_video_consumer(
        path, VideoClientOptions{limits, current_uid(), aa::core::Milliseconds{300}});
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start);
    ASSERT_FALSE(client);
    EXPECT_EQ(client.error().code(), aa::ErrorCode::timeout);
    EXPECT_GE(elapsed.count(), 250);
    EXPECT_LT(elapsed.count(), 1500);

    for (const int fd : backlog.fillers) {
        ::close(fd);
    }
    reap_queued_connections(server.value());
    EXPECT_TRUE(videotest::streams_one_unit(server.value(), path));

    server.value().close();
    ::unlink(path.value.c_str());
    videotest::remove_startup_lock(path.value);
    ::rmdir((runtime + "/android-auto-receiver").c_str());
    ::rmdir(runtime.c_str());
}

TEST(VideoStream, ConnectDeadlineIsEnforcedDuringSignalStorm) {
    NegotiatedLimits limits{};
    const auto runtime = videotest::private_runtime_dir();
    ASSERT_FALSE(runtime.empty());
    const auto path = video_socket_path_in(runtime);
    auto server = VideoServer::listen(path, VideoServerOptions{limits, current_uid()});
    ASSERT_TRUE(server);

    const auto backlog = saturate_backlog(path);
    EXPECT_EQ(backlog.unexpected_errno, 0) << "unexpected filler connect failure";
    EXPECT_FALSE(backlog.fillers.empty()) << "backlog admitted no connection";
    ASSERT_TRUE(backlog.queue_refused) << "backlog never saturated";

    struct sigaction action {};
    struct sigaction previous {};
    action.sa_handler = +[](int) {};
    ::sigemptyset(&action.sa_mask);
    ASSERT_EQ(::sigaction(SIGUSR1, &action, &previous), 0);
    const pthread_t waiter = ::pthread_self();
    std::atomic<bool> storming{true};
    std::thread storm([waiter, &storming] {
        while (storming.load(std::memory_order_relaxed)) {
            (void)::pthread_kill(waiter, SIGUSR1);
            std::this_thread::sleep_for(std::chrono::milliseconds{5});
        }
    });

    const auto start = std::chrono::steady_clock::now();
    const auto client = connect_video_consumer(
        path, VideoClientOptions{limits, current_uid(), aa::core::Milliseconds{300}});
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start);

    storming.store(false, std::memory_order_relaxed);
    storm.join();
    (void)::sigaction(SIGUSR1, &previous, nullptr);

    ASSERT_FALSE(client);
    EXPECT_EQ(client.error().code(), aa::ErrorCode::timeout);
    EXPECT_GE(elapsed.count(), 250);
    EXPECT_LT(elapsed.count(), 2000);

    for (const int fd : backlog.fillers) {
        ::close(fd);
    }
    reap_queued_connections(server.value());

    server.value().close();
    ::unlink(path.value.c_str());
    videotest::remove_startup_lock(path.value);
    ::rmdir((runtime + "/android-auto-receiver").c_str());
    ::rmdir(runtime.c_str());
}

TEST(VideoStream, ConnectRefusalFailsBeforeTheDeadline) {
    const auto runtime = videotest::private_runtime_dir();
    ASSERT_FALSE(runtime.empty());
    const auto path = video_socket_path_in(runtime);

    const auto measure = [&](const VideoSocketPath& target) {
        const auto start = std::chrono::steady_clock::now();
        const auto client = connect_video_consumer(
            target,
            VideoClientOptions{{}, current_uid(), aa::core::Milliseconds{5000}});
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start);
        ASSERT_FALSE(client);
        EXPECT_EQ(client.error().code(), aa::ErrorCode::transport_unavailable);
        EXPECT_LT(elapsed.count(), 1000);
    };

    measure(path);

    const std::string directory = path.value.substr(0, path.value.find_last_of('/'));
    ASSERT_EQ(::mkdir(directory.c_str(), 0700), 0);
    ASSERT_TRUE(videotest::leave_stale_socket_node(path.value));
    measure(path);

    ::unlink(path.value.c_str());
    ::rmdir(directory.c_str());
    ::rmdir(runtime.c_str());
}

TEST(VideoStream, ConcurrentConsumersAreIndependent) {
    NegotiatedLimits limits{};
    const auto runtime = videotest::private_runtime_dir();
    ASSERT_FALSE(runtime.empty());
    const auto path = video_socket_path_in(runtime);
    auto server = VideoServer::listen(path, VideoServerOptions{limits, current_uid()});
    ASSERT_TRUE(server);

    const auto handshake = [&](aa::core::Result<VideoConsumer>& client) {
        std::thread connector([&] {
            client = connect_video_consumer(
                path, VideoClientOptions{limits, current_uid(), aa::core::Milliseconds{2000}});
        });
        auto producer = server.value().accept_consumer(aa::core::Milliseconds{2000});
        connector.join();
        return producer;
    };

    aa::core::Result<VideoConsumer> client_a{VideoConsumer{}};
    auto producer_a = handshake(client_a);
    ASSERT_TRUE(producer_a);
    ASSERT_TRUE(client_a);

    const std::vector<std::uint8_t> payload_a1{0xa1, 0xa2};
    ASSERT_TRUE(producer_a.value().send_access_unit({1, 1, true, payload_a1}));
    ASSERT_TRUE(drain_until_complete(client_a.value(), 4));
    EXPECT_EQ(client_a.value().access_unit().payload, payload_a1);

    aa::core::Result<VideoConsumer> client_b{VideoConsumer{}};
    auto producer_b = handshake(client_b);
    ASSERT_TRUE(producer_b) << "second producer must be returned while the first is live";
    ASSERT_TRUE(client_b);

    const std::vector<std::uint8_t> payload_a2{0xb1, 0xb2, 0xb3};
    const std::vector<std::uint8_t> payload_b1{0xc1};
    ASSERT_TRUE(producer_a.value().send_access_unit({2, 2, true, payload_a2}));
    ASSERT_TRUE(producer_b.value().send_access_unit({1, 1, true, payload_b1}));
    ASSERT_TRUE(drain_until_complete(client_a.value(), 4));
    ASSERT_TRUE(drain_until_complete(client_b.value(), 4));
    EXPECT_EQ(client_a.value().access_unit().payload, payload_a2);
    EXPECT_EQ(client_b.value().access_unit().payload, payload_b1);

    producer_b.value().close();
    client_b.value().close();
    const std::vector<std::uint8_t> payload_a3{0xd1, 0xd2, 0xd3};
    ASSERT_TRUE(producer_a.value().send_access_unit({3, 3, true, payload_a3}));
    ASSERT_TRUE(drain_until_complete(client_a.value(), 4));
    EXPECT_EQ(client_a.value().access_unit().payload, payload_a3);

    producer_a.value().close();
    client_a.value().close();
    server.value().close();
    struct stat info {};
    EXPECT_NE(::stat(path.value.c_str(), &info), 0);
    videotest::remove_startup_lock(path.value);
    ::rmdir((runtime + "/android-auto-receiver").c_str());
    ::rmdir(runtime.c_str());
}
