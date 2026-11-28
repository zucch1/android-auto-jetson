// SPDX-License-Identifier: GPL-3.0-or-later
#include "pair.hpp"

#include <aa/ipc/VideoChannel.hpp>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <climits>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <string>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <thread>
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

    aa::core::Result<VideoConsumer> client{VideoConsumer{}};
    std::chrono::milliseconds elapsed{};
    {
        const videotest::SignalStorm storm{::pthread_self()};
        ASSERT_TRUE(storm.valid());
        const auto start = std::chrono::steady_clock::now();
        client = connect_video_consumer(
            path, VideoClientOptions{limits, current_uid(), aa::core::Milliseconds{300}});
        elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start);
    }

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

TEST(VideoStream, ReceiveDeadlineIsEnforcedDuringSignalStorm) {
    NegotiatedLimits limits{};
    auto pair = videotest::make_pair(limits);
    ASSERT_TRUE(pair);
    auto& producer = pair.value().producer;
    auto& consumer = pair.value().consumer;

    const bool waiting_before = consumer.waiting_idr();
    const std::uint64_t resyncs_before = consumer.resyncs();
    const std::uint64_t rejected_before = consumer.rejected();
    const std::uint64_t suppressed_before = consumer.suppressed();

    aa::core::Result<ReceiveStatus> step{ReceiveStatus::fragment};
    std::chrono::milliseconds elapsed{};
    {
        const videotest::SignalStorm storm{::pthread_self()};
        ASSERT_TRUE(storm.valid());
        const auto start = std::chrono::steady_clock::now();
        step = consumer.receive(aa::core::Milliseconds{300});
        elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start);
    }

    ASSERT_FALSE(step);
    EXPECT_EQ(step.error().code(), aa::ErrorCode::timeout);
    EXPECT_GE(elapsed.count(), 250);
    EXPECT_LT(elapsed.count(), 2000);
    EXPECT_EQ(consumer.waiting_idr(), waiting_before);
    EXPECT_EQ(consumer.resyncs(), resyncs_before);
    EXPECT_EQ(consumer.rejected(), rejected_before);
    EXPECT_EQ(consumer.suppressed(), suppressed_before);

    const std::vector<std::uint8_t> payload{0x5a};
    ASSERT_TRUE(producer.send_access_unit({1, 1, true, payload}));
    const auto recovered = consumer.receive(aa::core::Milliseconds{2000});
    ASSERT_TRUE(recovered);
    EXPECT_EQ(recovered.value(), ReceiveStatus::complete);
}

TEST(VideoStream, AcceptDeadlineIsEnforcedDuringSignalStorm) {
    NegotiatedLimits limits{};
    const auto runtime = videotest::private_runtime_dir();
    ASSERT_FALSE(runtime.empty());
    const auto path = video_socket_path_in(runtime);
    auto server = VideoServer::listen(path, VideoServerOptions{limits, current_uid()});
    ASSERT_TRUE(server);

    aa::core::Result<VideoProducer> accepted{VideoProducer{}};
    std::chrono::milliseconds elapsed{};
    {
        const videotest::SignalStorm storm{::pthread_self()};
        ASSERT_TRUE(storm.valid());
        const auto start = std::chrono::steady_clock::now();
        accepted = server.value().accept_consumer(aa::core::Milliseconds{300});
        elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start);
    }

    ASSERT_FALSE(accepted);
    EXPECT_EQ(accepted.error().code(), aa::ErrorCode::timeout);
    EXPECT_GE(elapsed.count(), 250);
    EXPECT_LT(elapsed.count(), 2000);
    EXPECT_TRUE(videotest::streams_one_unit(server.value(), path));

    server.value().close();
    ::unlink(path.value.c_str());
    videotest::remove_startup_lock(path.value);
    ::rmdir((runtime + "/android-auto-receiver").c_str());
    ::rmdir(runtime.c_str());
}

TEST(VideoStream, ZeroTimeoutProbesWithoutBlocking) {
    NegotiatedLimits limits{};
    auto pair = videotest::make_pair(limits);
    ASSERT_TRUE(pair);

    const auto idle_start = std::chrono::steady_clock::now();
    const auto idle = pair.value().consumer.receive(aa::core::Milliseconds{0});
    const auto idle_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - idle_start);
    ASSERT_FALSE(idle);
    EXPECT_EQ(idle.error().code(), aa::ErrorCode::timeout);
    EXPECT_LT(idle_elapsed.count(), 250);

    const std::vector<std::uint8_t> payload{0x5a};
    ASSERT_TRUE(pair.value().producer.send_access_unit({1, 1, true, payload}));
    const auto queued = pair.value().consumer.receive(aa::core::Milliseconds{0});
    ASSERT_TRUE(queued);
    EXPECT_EQ(queued.value(), ReceiveStatus::complete);

    const auto accept_start = std::chrono::steady_clock::now();
    const auto accepted = pair.value().server.accept_consumer(aa::core::Milliseconds{0});
    const auto accept_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - accept_start);
    ASSERT_FALSE(accepted);
    EXPECT_EQ(accepted.error().code(), aa::ErrorCode::timeout);
    EXPECT_LT(accept_elapsed.count(), 250);
}

TEST(VideoStream, NegativeTimeoutWaitsForDelayedDataAndConnection) {
    NegotiatedLimits limits{};
    auto pair = videotest::make_pair(limits);
    ASSERT_TRUE(pair);
    const std::vector<std::uint8_t> payload{0x5a};
    std::thread sender([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds{100});
        (void)pair.value().producer.send_access_unit({1, 1, true, payload});
    });
    const auto step = pair.value().consumer.receive(aa::core::Milliseconds{-1});
    sender.join();
    ASSERT_TRUE(step);
    EXPECT_EQ(step.value(), ReceiveStatus::complete);

    const auto runtime = videotest::private_runtime_dir();
    ASSERT_FALSE(runtime.empty());
    const auto path = video_socket_path_in(runtime);
    auto server = VideoServer::listen(path, VideoServerOptions{limits, current_uid()});
    ASSERT_TRUE(server);
    aa::core::Result<VideoConsumer> client{VideoConsumer{}};
    std::thread connector([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds{100});
        client = connect_video_consumer(
            path, VideoClientOptions{limits, current_uid(), aa::core::Milliseconds{2000}});
    });
    auto producer = server.value().accept_consumer(aa::core::Milliseconds{-1});
    connector.join();
    ASSERT_TRUE(producer);
    ASSERT_TRUE(client);
    const std::vector<std::uint8_t> late{0x5b};
    ASSERT_TRUE(producer.value().send_access_unit({1, 1, true, late}));
    const auto late_step = client.value().receive(aa::core::Milliseconds{2000});
    ASSERT_TRUE(late_step);
    EXPECT_EQ(late_step.value(), ReceiveStatus::complete);

    producer.value().close();
    client.value().close();
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

TEST(VideoChannel, RecommendedSocketBufferScalesWithProfileAndClampsToBounds) {
    using aa::ipc::kMaxSocketBufferBytes;
    using aa::ipc::kMinSocketBufferBytes;
    using aa::ipc::recommended_socket_buffer_bytes;
    using aa::protocol::VideoCodec;
    using aa::protocol::VideoProfile;

    const auto low = recommended_socket_buffer_bytes(VideoProfile{800, 480, 30, VideoCodec::h264});
    EXPECT_EQ(low, kMinSocketBufferBytes);

    const auto p720 = recommended_socket_buffer_bytes(VideoProfile{1280, 720, 30, VideoCodec::h264});
    EXPECT_GE(p720, kMinSocketBufferBytes);
    EXPECT_LE(p720, kMaxSocketBufferBytes);

    const auto p1080 = recommended_socket_buffer_bytes(VideoProfile{1920, 1080, 60, VideoCodec::h264});
    EXPECT_GT(p1080, p720);
    EXPECT_LT(p1080, kMaxSocketBufferBytes);

    const auto p4k = recommended_socket_buffer_bytes(VideoProfile{3840, 2160, 60, VideoCodec::h265});
    EXPECT_EQ(p4k, kMaxSocketBufferBytes);

    auto pair = videotest::make_pair({});
    ASSERT_TRUE(pair);
    const auto send_sized =
        pair.value().producer.configure_send_buffer(VideoProfile{1920, 1080, 60, VideoCodec::h264});
    ASSERT_TRUE(send_sized);
    EXPECT_EQ(send_sized.value().requested_bytes, p1080);
    EXPECT_EQ(send_sized.value().setsockopt_errno, 0);
    EXPECT_EQ(send_sized.value().getsockopt_errno, 0);
    EXPECT_GT(send_sized.value().granted_raw_bytes, 0u);
    EXPECT_EQ(send_sized.value().granted_bytes, send_sized.value().granted_raw_bytes / 2U);
    EXPECT_EQ(send_sized.value().short_grant(),
              send_sized.value().granted_bytes < send_sized.value().requested_bytes);
    EXPECT_EQ(send_sized.value().has_warning(), send_sized.value().short_grant());

    const auto recv_sized =
        pair.value().consumer.configure_receive_buffer(VideoProfile{1920, 1080, 60, VideoCodec::h264});
    ASSERT_TRUE(recv_sized);
    EXPECT_EQ(recv_sized.value().requested_bytes, p1080);
    EXPECT_EQ(recv_sized.value().setsockopt_errno, 0);
    EXPECT_EQ(recv_sized.value().getsockopt_errno, 0);
    EXPECT_GT(recv_sized.value().granted_raw_bytes, 0u);
    EXPECT_EQ(recv_sized.value().granted_bytes, recv_sized.value().granted_raw_bytes / 2U);

    // Warning telemetry reflects errno or short_grant without altering SocketBufferResult contract.
    aa::ipc::SocketBufferResult simulated_err{};
    simulated_err.requested_bytes = p1080;
    simulated_err.granted_raw_bytes = 425984;
    simulated_err.granted_bytes = 212992;
    simulated_err.setsockopt_errno = EPERM;
    EXPECT_TRUE(simulated_err.has_warning());
    EXPECT_FALSE(simulated_err.short_grant());
    const auto err_warn = aa::ipc::video_socket_buffer_warning_json(
        "producer", simulated_err, VideoProfile{1920, 1080, 60, VideoCodec::h264});
    EXPECT_NE(err_warn.find("\"event\":\"video_socket_buffer_warning\""), std::string::npos);
    EXPECT_NE(err_warn.find("\"reason\":\"setsockopt_failed\""), std::string::npos);

    pair.value().producer.close();
    EXPECT_FALSE(pair.value().producer.configure_send_buffer(VideoProfile{1280, 720, 30}).has_value());
}

TEST(VideoChannel, ProducerWaitingIdrTracksBackpressureAndResetsOnIdr) {
    auto pair = videotest::make_pair({});
    ASSERT_TRUE(pair);
    // Fresh producer starts awaiting its initial IDR before any keyframe is sent.
    EXPECT_TRUE(pair.value().producer.waiting_idr());

    std::uint64_t frame = 1;
    const auto pre_idr = pair.value().producer.send_access_unit(
        {frame++, frame * 1000, false, std::vector<std::uint8_t>{0x00, 0x00, 0x01, 0x41}, std::nullopt});
    ASSERT_TRUE(pre_idr);
    EXPECT_EQ(pre_idr.value(), aa::ipc::SendStatus::dropped_waiting_idr);
    EXPECT_TRUE(pair.value().producer.waiting_idr());

    const auto initial_idr = pair.value().producer.send_access_unit(
        {frame++, frame * 1000, true, std::vector<std::uint8_t>{0x00, 0x00, 0x01, 0x65}, std::nullopt});
    ASSERT_TRUE(initial_idr);
    EXPECT_EQ(initial_idr.value(), aa::ipc::SendStatus::sent);
    EXPECT_FALSE(pair.value().producer.waiting_idr());
    ASSERT_TRUE(pair.value().consumer.receive(aa::core::Milliseconds{500}));

    const int small_buf = 32768;
    ASSERT_EQ(::setsockopt(pair.value().producer.native_handle(), SOL_SOCKET, SO_SNDBUF,
                           &small_buf, sizeof(small_buf)), 0);
    ASSERT_EQ(::setsockopt(pair.value().consumer.native_handle(), SOL_SOCKET, SO_RCVBUF,
                           &small_buf, sizeof(small_buf)), 0);

    std::vector<std::uint8_t> chunk(16384, 0x22);
    aa::ipc::SendStatus status = aa::ipc::SendStatus::sent;
    for (int i = 0; i < 128 && status == aa::ipc::SendStatus::sent; ++i) {
        const auto sent = pair.value().producer.send_access_unit(
            {frame, frame * 1000, false, chunk, std::nullopt});
        ASSERT_TRUE(sent) << aa::to_string(sent.error().code());
        status = sent.value();
        ++frame;
    }
    ASSERT_EQ(status, aa::ipc::SendStatus::dropped_backpressure);
    EXPECT_TRUE(pair.value().producer.waiting_idr());

    const auto still_waiting = pair.value().producer.send_access_unit(
        {frame++, frame * 1000, false, std::vector<std::uint8_t>{0x00, 0x00, 0x01, 0x41}, std::nullopt});
    ASSERT_TRUE(still_waiting);
    EXPECT_EQ(still_waiting.value(), aa::ipc::SendStatus::dropped_waiting_idr);
    EXPECT_TRUE(pair.value().producer.waiting_idr());

    for (;;) {
        const auto got = pair.value().consumer.receive(aa::core::Milliseconds{0});
        if (!got) {
            break;
        }
    }

    const auto recovered = pair.value().producer.send_access_unit(
        {frame++, frame * 1000, true, std::vector<std::uint8_t>{0x00, 0x00, 0x01, 0x65}, std::nullopt});
    ASSERT_TRUE(recovered);
    EXPECT_EQ(recovered.value(), aa::ipc::SendStatus::sent);
    EXPECT_FALSE(pair.value().producer.waiting_idr());
}

TEST(VideoChannel, RecommendedBackpressureBudgetScalesByProfileAndClamps) {
    using aa::ipc::kMinSocketBufferBytes;
    using aa::ipc::recommended_backpressure_budget;
    using aa::protocol::VideoCodec;
    using aa::protocol::VideoProfile;

    const auto fallback = recommended_backpressure_budget(std::nullopt);
    EXPECT_EQ(fallback.max_bytes, kMinSocketBufferBytes);
    EXPECT_EQ(fallback.max_age, std::chrono::milliseconds{67});

    const auto sd30 = recommended_backpressure_budget(VideoProfile{800, 480, 30, VideoCodec::h264});
    EXPECT_EQ(sd30.max_bytes, kMinSocketBufferBytes);
    EXPECT_EQ(sd30.max_age, std::chrono::milliseconds{67});

    const auto hd1080p60 = recommended_backpressure_budget(VideoProfile{1920, 1080, 60, VideoCodec::h264});
    EXPECT_EQ(hd1080p60.max_bytes, 486'000U);
    EXPECT_EQ(hd1080p60.max_age, std::chrono::milliseconds{50});

    const auto qhd1440p60 = recommended_backpressure_budget(VideoProfile{2560, 1440, 60, VideoCodec::h265});
    EXPECT_EQ(qhd1440p60.max_bytes, 864'000U);
    EXPECT_EQ(qhd1440p60.max_age, std::chrono::milliseconds{50});

    const auto uhd4k60 = recommended_backpressure_budget(VideoProfile{3840, 2160, 60, VideoCodec::h265});
    EXPECT_EQ(uhd4k60.max_bytes, 1024U * 1024U);
    EXPECT_EQ(uhd4k60.max_age, std::chrono::milliseconds{50});
}

TEST(VideoChannel, TrySendAccessUnitResumesMidAuFragmentWithoutSequenceGapOrLoss) {
    auto pair = videotest::make_pair({});
    ASSERT_TRUE(pair);

    // Clear initial waiting_idr with a small IDR.
    const std::vector<std::uint8_t> idr{0x00, 0x00, 0x01, 0x65};
    ASSERT_EQ(pair.value().producer.try_send_access_unit({1, 100, true, idr, std::nullopt}).value(),
              aa::ipc::TrySendStatus::sent);
    ASSERT_TRUE(pair.value().consumer.receive(aa::core::Milliseconds{500}));

    // Multi-fragment P-frame (40 KB -> 3 fragments at 16 KB datagram limit).
    std::vector<std::uint8_t> p_frame(40 * 1024);
    for (std::size_t i = 0; i < p_frame.size(); ++i) {
        p_frame[i] = static_cast<std::uint8_t>((i * 37U + 11U) & 0xFFU);
    }

    // Allow fragment 0, then return EAGAIN on fragment 1 to exercise mid-AU partial hold.
    std::size_t calls = 0;
    pair.value().producer.set_datagram_send_for_test(
        [&calls](int fd, const void* buf, std::size_t len, int flags, int& out_errno) -> std::ptrdiff_t {
            ++calls;
            if (calls == 2) {
                out_errno = EAGAIN;
                return -1;
            }
            const auto sent = ::send(fd, buf, len, flags);
            out_errno = (sent < 0) ? errno : 0;
            return sent;
        });

    const auto held = pair.value().producer.try_send_access_unit({2, 200, false, p_frame, std::nullopt});
    ASSERT_TRUE(held);
    EXPECT_EQ(held.value(), aa::ipc::TrySendStatus::would_block);
    EXPECT_FALSE(pair.value().producer.waiting_idr());
    EXPECT_EQ(pair.value().producer.dropped(), 0U);
    EXPECT_EQ(pair.value().producer.backpressure_events(), 0U);

    // Retry resumes seamlessly from fragment 1 without resending fragment 0.
    const auto resumed = pair.value().producer.try_send_access_unit({2, 200, false, p_frame, std::nullopt});
    ASSERT_TRUE(resumed);
    EXPECT_EQ(resumed.value(), aa::ipc::TrySendStatus::sent);
    EXPECT_EQ(calls, 4U); // fragment 0, blocked fragment 1, retried fragment 1, fragment 2

    aa::ipc::ReceiveStatus rx = aa::ipc::ReceiveStatus::waiting;
    for (int i = 0; i < 8 && rx != aa::ipc::ReceiveStatus::complete; ++i) {
        const auto step = pair.value().consumer.receive(aa::core::Milliseconds{500});
        ASSERT_TRUE(step);
        rx = step.value();
    }
    ASSERT_EQ(rx, aa::ipc::ReceiveStatus::complete);
    EXPECT_EQ(pair.value().consumer.access_unit().frame, 2U);
    EXPECT_EQ(pair.value().consumer.access_unit().payload, p_frame);
    EXPECT_EQ(pair.value().consumer.rejected(), 0U);
    EXPECT_EQ(pair.value().consumer.suppressed(), 0U);
    EXPECT_EQ(pair.value().consumer.resyncs(), 1U); // Only the initial startup IDR resync.
}

TEST(VideoChannel, AbandonPartialSendForcesWaitingIdrWithoutPartialEscape) {
    auto pair = videotest::make_pair({});
    ASSERT_TRUE(pair);

    const std::vector<std::uint8_t> idr{0x00, 0x00, 0x01, 0x65};
    ASSERT_EQ(pair.value().producer.try_send_access_unit({1, 100, true, idr, std::nullopt}).value(),
              aa::ipc::TrySendStatus::sent);
    ASSERT_TRUE(pair.value().consumer.receive(aa::core::Milliseconds{500}));
    EXPECT_FALSE(pair.value().producer.waiting_idr());

    // Even when no mid-AU partial state is active (e.g., fragment-0 EAGAIN),
    // abandoning a held AU on a retained producer must restore waiting_idr.
    pair.value().producer.abandon_partial_send();
    EXPECT_TRUE(pair.value().producer.waiting_idr());

    const std::vector<std::uint8_t> non_idr{0x00, 0x00, 0x01, 0x41};
    const auto suppressed =
        pair.value().producer.try_send_access_unit({2, 200, false, non_idr, std::nullopt});
    ASSERT_TRUE(suppressed);
    EXPECT_EQ(suppressed.value(), aa::ipc::TrySendStatus::dropped_waiting_idr);

    const auto fresh_idr =
        pair.value().producer.try_send_access_unit({3, 300, true, idr, std::nullopt});
    ASSERT_TRUE(fresh_idr);
    EXPECT_EQ(fresh_idr.value(), aa::ipc::TrySendStatus::sent);
    EXPECT_FALSE(pair.value().producer.waiting_idr());
    ASSERT_TRUE(pair.value().consumer.receive(aa::core::Milliseconds{500}));
    EXPECT_EQ(pair.value().consumer.access_unit().frame, 3U);
}

TEST(VideoChannel, ZeroTimeoutAcceptPreservesPendingHandshakeUntilClientSendsLimits) {
    NegotiatedLimits limits{};
    const auto runtime = videotest::private_runtime_dir();
    ASSERT_FALSE(runtime.empty());
    const auto path = video_socket_path_in(runtime);
    auto server = VideoServer::listen(path, VideoServerOptions{limits, current_uid()});
    ASSERT_TRUE(server);

    // Connect a raw SEQPACKET client and hold before sending its AAL1 record
    // (simulating a client preempted between connect() and send()).
    const int client_fd = ::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    ASSERT_GE(client_fd, 0);
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::memcpy(addr.sun_path, path.value.c_str(), path.value.size() + 1);
    ASSERT_EQ(::connect(client_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)), 0);

    // A 0 ms poll accepts the socket, sends the server's AAL1 record, and
    // returns timeout immediately without closing the pending peer.
    const auto start = std::chrono::steady_clock::now();
    const auto first = server.value().accept_consumer(aa::core::Milliseconds{0});
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start);
    ASSERT_FALSE(first);
    EXPECT_EQ(first.error().code(), aa::ErrorCode::timeout);
    EXPECT_LT(elapsed.count(), 250);
    EXPECT_TRUE(server.value().has_pending_handshake());

    // The server already sent its AAL1 record; read it on the client side, then
    // release the client's AAL1 record.
    std::array<std::uint8_t, kLimitsRecordBytes> server_record{};
    ASSERT_EQ(::recv(client_fd, server_record.data(), server_record.size(), 0),
              static_cast<ssize_t>(kLimitsRecordBytes));
    ASSERT_TRUE(decode_limits_record(server_record));

    std::array<std::uint8_t, kLimitsRecordBytes> client_record{};
    encode_limits_record(client_record, limits);
    ASSERT_EQ(::send(client_fd, client_record.data(), client_record.size(), MSG_NOSIGNAL),
              static_cast<ssize_t>(kLimitsRecordBytes));

    // The next 0 ms poll completes the pending handshake and can stream frames.
    auto accepted = server.value().accept_consumer(aa::core::Milliseconds{0});
    ASSERT_TRUE(accepted);
    EXPECT_FALSE(server.value().has_pending_handshake());

    const std::vector<std::uint8_t> payload{0x00, 0x00, 0x01, 0x65, 0xaa};
    ASSERT_TRUE(accepted.value().send_access_unit({1, 100, true, payload}));
    std::array<std::uint8_t, kFrameHeaderBytes + 64> rx{};
    const ssize_t got = ::recv(client_fd, rx.data(), rx.size(), 0);
    ASSERT_EQ(got, static_cast<ssize_t>(kFrameHeaderBytes + payload.size()));

    ::close(client_fd);
    accepted.value().close();
    server.value().close();
    ::unlink(path.value.c_str());
    videotest::remove_startup_lock(path.value);
    ::rmdir((runtime + "/android-auto-receiver").c_str());
    ::rmdir(runtime.c_str());
}

TEST(VideoChannel, ZeroTimeoutPendingHandshakeExpiresAndAdmitsNextClient) {
    NegotiatedLimits limits{};
    const auto runtime = videotest::private_runtime_dir();
    ASSERT_FALSE(runtime.empty());
    const auto path = video_socket_path_in(runtime);
    auto server = VideoServer::listen(path, VideoServerOptions{limits, current_uid()});
    ASSERT_TRUE(server);

    const int silent_fd = ::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    ASSERT_GE(silent_fd, 0);
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::memcpy(addr.sun_path, path.value.c_str(), path.value.size() + 1);
    ASSERT_EQ(::connect(silent_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)), 0);

    // Use a short custom handshake deadline (40 ms) with a 0 ms accept poll.
    ASSERT_FALSE(server.value().accept_consumer(aa::core::Milliseconds{0},
                                                aa::core::Milliseconds{40}));
    EXPECT_TRUE(server.value().has_pending_handshake());

    // Driving the pending handshake with a call budget larger than the
    // remaining handshake deadline expires cleanly at 40 ms without sleeping.
    const auto expired =
        server.value().accept_consumer(aa::core::Milliseconds{500}, aa::core::Milliseconds{40});
    ASSERT_FALSE(expired);
    EXPECT_EQ(expired.error().code(), aa::ErrorCode::timeout);
    EXPECT_FALSE(server.value().has_pending_handshake());
    ::close(silent_fd);

    // A healthy consumer connects cleanly afterwards.
    aa::core::Result<VideoConsumer> consumer{aa::Error{aa::ErrorCode::timeout}};
    std::thread connector([&] {
        consumer = connect_video_consumer(
            path, VideoClientOptions{limits, current_uid(), aa::core::Milliseconds{2000}});
    });
    auto producer = server.value().accept_consumer(aa::core::Milliseconds{2000});
    connector.join();
    ASSERT_TRUE(producer);
    ASSERT_TRUE(consumer);

    producer.value().close();
    consumer.value().close();
    server.value().close();
    ::unlink(path.value.c_str());
    videotest::remove_startup_lock(path.value);
    ::rmdir((runtime + "/android-auto-receiver").c_str());
    ::rmdir(runtime.c_str());
}

TEST(VideoChannel, BorrowedProducersPreserveMonotonicCountersAcrossHandoffAndInterruptedAu) {
    NegotiatedLimits limits{};
    limits.max_datagram_payload = 1024;
    auto pair = videotest::make_pair(limits);
    ASSERT_TRUE(pair);
    auto& owner = pair.value().producer;
    auto& consumer = pair.value().consumer;

    // Producer 1 borrows the supervisor-owned connection, sends IDR frame 1,
    // delta frame 2, and then fails mid-AU after fragment 0 of multi-fragment
    // frame 3 (simulating a worker crash mid-access-unit).
    auto borrowed1 = owner.borrow();
    ASSERT_TRUE(borrowed1);
    const std::vector<std::uint8_t> idr1{0x00, 0x00, 0x01, 0x65, 0x11};
    const std::vector<std::uint8_t> delta2{0x00, 0x00, 0x01, 0x41, 0x22};
    ASSERT_EQ(borrowed1.value().try_send_access_unit({1, 100, true, idr1}).value(),
              TrySendStatus::sent);
    ASSERT_EQ(borrowed1.value().try_send_access_unit({2, 200, false, delta2}).value(),
              TrySendStatus::sent);

    std::vector<std::uint8_t> partial3(2500, 0x33);
    unsigned frag_calls = 0;
    borrowed1.value().set_datagram_send_for_test(
        [&](int fd, const void* buf, std::size_t len, int flags, int& out_errno) -> std::ptrdiff_t {
            if (++frag_calls == 2) {
                out_errno = EAGAIN;
                return -1;
            }
            const auto sent = ::send(fd, buf, len, flags);
            out_errno = (sent < 0) ? errno : 0;
            return sent;
        });
    ASSERT_EQ(borrowed1.value().try_send_access_unit({3, 300, false, partial3}).value(),
              TrySendStatus::would_block);
    // Closing borrowed producer 1 must not send SHUT_WR on the shared connection.
    borrowed1.value().close();

    // Producer 2 borrows the SAME connection and sees the updated monotonic
    // counters past the interrupted frame 3.
    auto borrowed2 = owner.borrow();
    ASSERT_TRUE(borrowed2);
    EXPECT_GE(borrowed2.value().last_frame_id(), 3U);
    EXPECT_GE(borrowed2.value().next_sequence(), 3U);

    const std::uint64_t next_frame = borrowed2.value().last_frame_id() + 1;
    const std::vector<std::uint8_t> idr2{0x00, 0x00, 0x01, 0x65, 0x44};
    ASSERT_EQ(borrowed2.value().try_send_access_unit({next_frame, 400, true, idr2}).value(),
              TrySendStatus::sent);

    // Consumer receives frame 1, frame 2, drops interrupted frame 3 cleanly,
    // and accepts producer 2's IDR with ZERO rejected records.
    ASSERT_EQ(drain_until_complete(consumer, 4).value(), ReceiveStatus::complete);
    EXPECT_EQ(consumer.access_unit().frame, 1U);
    EXPECT_EQ(consumer.access_unit().payload, idr1);

    ASSERT_EQ(drain_until_complete(consumer, 4).value(), ReceiveStatus::complete);
    EXPECT_EQ(consumer.access_unit().frame, 2U);
    EXPECT_EQ(consumer.access_unit().payload, delta2);

    ASSERT_EQ(drain_until_complete(consumer, 8).value(), ReceiveStatus::complete);
    EXPECT_EQ(consumer.access_unit().frame, next_frame);
    EXPECT_EQ(consumer.access_unit().payload, idr2);
    EXPECT_EQ(consumer.rejected(), 0U);
    EXPECT_FALSE(owner.peer_closed());

    // Closing the consumer is detected immediately by owner.peer_closed().
    consumer.close();
    EXPECT_TRUE(owner.peer_closed());
}

TEST(VideoChannel, ExpiredPendingHandshakeRejectsAlreadyQueuedLateLimitsRecord) {
    NegotiatedLimits limits{};
    const auto runtime = videotest::private_runtime_dir();
    ASSERT_FALSE(runtime.empty());
    const auto path = video_socket_path_in(runtime);
    auto server = VideoServer::listen(path, VideoServerOptions{limits, current_uid()});
    ASSERT_TRUE(server);

    const int late_fd = ::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    ASSERT_GE(late_fd, 0);
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::memcpy(addr.sun_path, path.value.c_str(), path.value.size() + 1);
    ASSERT_EQ(::connect(late_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)), 0);

    const auto started = std::chrono::steady_clock::now();
    const auto first =
        server.value().accept_consumer(aa::core::Milliseconds{0}, aa::core::Milliseconds{20});
    ASSERT_FALSE(first);
    EXPECT_EQ(first.error().code(), aa::ErrorCode::timeout);
    EXPECT_TRUE(server.value().has_pending_handshake());

    // Wait past the 20 ms handshake deadline, then queue a valid AAL1 record
    // BEFORE the next server poll. Readability must not defeat expiration.
    std::this_thread::sleep_until(started + std::chrono::milliseconds{40});
    std::array<std::uint8_t, kLimitsRecordBytes> server_record{};
    ASSERT_EQ(::recv(late_fd, server_record.data(), server_record.size(), 0),
              static_cast<ssize_t>(kLimitsRecordBytes));
    std::array<std::uint8_t, kLimitsRecordBytes> client_record{};
    encode_limits_record(client_record, limits);
    ASSERT_EQ(::send(late_fd, client_record.data(), client_record.size(), MSG_NOSIGNAL),
              static_cast<ssize_t>(kLimitsRecordBytes));

    const auto expired =
        server.value().accept_consumer(aa::core::Milliseconds{0}, aa::core::Milliseconds{20});
    ASSERT_FALSE(expired);
    EXPECT_EQ(expired.error().code(), aa::ErrorCode::timeout);
    EXPECT_FALSE(server.value().has_pending_handshake());

    // The server closed the expired socket without reading the late AAL1 record
    // (Linux SOCK_SEQPACKET reports EOF or ECONNRESET when closed with unread data).
    std::array<std::uint8_t, 8> probe{};
    errno = 0;
    const ssize_t probe_rc = ::recv(late_fd, probe.data(), probe.size(), 0);
    EXPECT_TRUE(probe_rc == 0 || (probe_rc == -1 && errno == ECONNRESET))
        << "probe_rc=" << probe_rc << " errno=" << errno;
    ::close(late_fd);

    // Freshly accepted socket with an explicit 0 ms handshake budget still
    // performs its single immediate probe when the peer record is already queued.
    const int eager_fd = ::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    ASSERT_GE(eager_fd, 0);
    ASSERT_EQ(::connect(eager_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)), 0);
    ASSERT_EQ(::send(eager_fd, client_record.data(), client_record.size(), MSG_NOSIGNAL),
              static_cast<ssize_t>(kLimitsRecordBytes));
    auto eager =
        server.value().accept_consumer(aa::core::Milliseconds{0}, aa::core::Milliseconds{0});
    ASSERT_TRUE(eager);
    EXPECT_FALSE(server.value().has_pending_handshake());

    eager.value().close();
    ::close(eager_fd);
    server.value().close();
    ::unlink(path.value.c_str());
    videotest::remove_startup_lock(path.value);
    ::rmdir((runtime + "/android-auto-receiver").c_str());
    ::rmdir(runtime.c_str());
}

TEST(VideoChannel, SharedStateMemfdIsMode0600AndSizeSealedAndRejectsInvalidAdopt) {
    auto pair = videotest::make_pair({});
    ASSERT_TRUE(pair);
    auto& owner = pair.value().producer;
    auto& consumer = pair.value().consumer;

    auto state_fd = owner.prepare_shared_state();
    ASSERT_TRUE(state_fd);

    struct stat info {};
    ASSERT_EQ(::fstat(state_fd.value(), &info), 0);
    EXPECT_EQ(info.st_mode & 0777, 0600U);

    const int seals = ::fcntl(state_fd.value(), F_GET_SEALS);
    ASSERT_GE(seals, 0);
    EXPECT_EQ(seals & (F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_SEAL),
              (F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_SEAL));

    // Size mutations must fail with EPERM so neither process can cause SIGBUS.
    errno = 0;
    EXPECT_EQ(::ftruncate(state_fd.value(), 0), -1);
    EXPECT_EQ(errno, EPERM);
    errno = 0;
    EXPECT_EQ(::ftruncate(state_fd.value(), 4096), -1);
    EXPECT_EQ(errno, EPERM);

    // adopt_accepted must reject swapped/out-of-range limits and unsealed memfds.
    NegotiatedLimits swapped{};
    swapped.max_datagram_payload = static_cast<std::uint32_t>(kMaxAccessUnitBytes);
    swapped.max_access_unit_bytes = static_cast<std::uint32_t>(kMaxDatagramPayload);
    const int dup_sock = ::fcntl(owner.native_handle(), F_DUPFD_CLOEXEC, 0);
    ASSERT_GE(dup_sock, 0);
    const int dup_state = ::fcntl(state_fd.value(), F_DUPFD_CLOEXEC, 0);
    ASSERT_GE(dup_state, 0);
    auto bad_limits = VideoProducer::adopt_accepted(dup_sock, swapped, dup_state);
    ASSERT_FALSE(bad_limits);
    EXPECT_EQ(bad_limits.error().code(), aa::ErrorCode::protocol_negotiation_failed);

    const int unsealed = ::memfd_create("unsealed-test", MFD_CLOEXEC);
    ASSERT_GE(unsealed, 0);
    ASSERT_EQ(::ftruncate(unsealed, 64), 0);
    auto bad_state = VideoProducer::adopt_accepted(dup_sock, owner.limits(), unsealed);
    ASSERT_FALSE(bad_state);
    EXPECT_EQ(bad_state.error().code(), aa::ErrorCode::transport_io);
    ::close(unsealed);
    ::close(dup_state);
    ::close(dup_sock);

    // Owner close marks the shared state retired before SHUT_WR so a still-live
    // borrowed producer returns transport_closed while the consumer is still open.
    auto borrowed = owner.borrow();
    ASSERT_TRUE(borrowed);
    EXPECT_FALSE(borrowed.value().retired());
    owner.close();
    EXPECT_TRUE(borrowed.value().retired());
    const std::vector<std::uint8_t> idr{0x00, 0x00, 0x01, 0x65, 0x77};
    const auto try_res = borrowed.value().try_send_access_unit({1, 100, true, idr});
    ASSERT_FALSE(try_res);
    EXPECT_EQ(try_res.error().code(), aa::ErrorCode::transport_closed);
    const auto send_res = borrowed.value().send_access_unit({1, 100, true, idr});
    ASSERT_FALSE(send_res);
    EXPECT_EQ(send_res.error().code(), aa::ErrorCode::transport_closed);

    const auto eof = consumer.receive(aa::core::Milliseconds{500});
    ASSERT_FALSE(eof);
    EXPECT_EQ(eof.error().code(), aa::ErrorCode::transport_closed);
}

TEST(VideoChannel, DrainQueuedConnectionsClearsBacklogWithoutAal1AndKeepsListenerLive) {
    NegotiatedLimits limits{};
    const auto runtime = videotest::private_runtime_dir();
    ASSERT_FALSE(runtime.empty());
    const auto path = video_socket_path_in(runtime);
    auto server = VideoServer::listen(path, VideoServerOptions{limits, current_uid()});
    ASSERT_TRUE(server);

    const int stale_fd = ::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    ASSERT_GE(stale_fd, 0);
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::memcpy(addr.sun_path, path.value.c_str(), path.value.size() + 1);
    ASSERT_EQ(::connect(stale_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)), 0);

    const auto drained = server.value().drain_queued_connections();
    ASSERT_TRUE(drained);
    EXPECT_EQ(drained.value(), 1U);
    EXPECT_FALSE(server.value().has_pending_handshake());
    pollfd pfd{stale_fd, POLLIN, 0};
    ASSERT_EQ(::poll(&pfd, 1, 500), 1);
    std::array<std::uint8_t, kLimitsRecordBytes> buf{};
    EXPECT_EQ(::recv(stale_fd, buf.data(), buf.size(), MSG_DONTWAIT), 0);
    ::close(stale_fd);

    // Subsequent healthy consumer connects cleanly on the same listener.
    aa::core::Result<VideoConsumer> consumer{aa::Error{aa::ErrorCode::timeout}};
    std::thread connector([&] {
        consumer = connect_video_consumer(
            path, VideoClientOptions{limits, current_uid(), aa::core::Milliseconds{2000}});
    });
    auto producer = server.value().accept_consumer(aa::core::Milliseconds{2000});
    connector.join();
    ASSERT_TRUE(producer);
    ASSERT_TRUE(consumer);

    producer.value().close();
    consumer.value().close();
    server.value().close();
    ::unlink(path.value.c_str());
    videotest::remove_startup_lock(path.value);
    ::rmdir((runtime + "/android-auto-receiver").c_str());
    ::rmdir(runtime.c_str());
}

namespace {

struct FdBudgetGuard final {
    rlimit saved{};
    bool active{false};
    std::vector<int> fillers{};

    bool engage(std::size_t free_slots) {
        if (::getrlimit(RLIMIT_NOFILE, &saved) != 0) {
            return false;
        }
        int max_fd = 2;
        if (DIR* dir = ::opendir("/proc/self/fd")) {
            while (const dirent* ent = ::readdir(dir)) {
                char* end = nullptr;
                const long val = std::strtol(ent->d_name, &end, 10);
                if (end != ent->d_name && *end == '\0' && val > max_fd && val <= INT_MAX) {
                    max_fd = static_cast<int>(val);
                }
            }
            ::closedir(dir);
        }
        const int devnull = ::open("/dev/null", O_RDONLY | O_CLOEXEC);
        if (devnull < 0) {
            return false;
        }
        fillers.push_back(devnull);
        if (devnull > max_fd) {
            max_fd = devnull;
        }
        const rlim_t target =
            static_cast<rlim_t>(max_fd + 2) + static_cast<rlim_t>(free_slots);
        if (target >= saved.rlim_cur) {
            release();
            return false;
        }
        rlimit lowered = saved;
        lowered.rlim_cur = target;
        if (::setrlimit(RLIMIT_NOFILE, &lowered) != 0) {
            release();
            return false;
        }
        active = true;
        for (;;) {
            const int dup_fd = ::fcntl(devnull, F_DUPFD_CLOEXEC, 0);
            if (dup_fd < 0) {
                break;
            }
            fillers.push_back(dup_fd);
        }
        for (std::size_t i = 0; i < free_slots && !fillers.empty(); ++i) {
            ::close(fillers.back());
            fillers.pop_back();
        }
        return true;
    }

    void release() noexcept {
        for (const int fd : fillers) {
            ::close(fd);
        }
        fillers.clear();
        if (active) {
            (void)::setrlimit(RLIMIT_NOFILE, &saved);
            active = false;
        }
    }

    ~FdBudgetGuard() { release(); }
};

} // namespace

TEST(VideoChannel, DrainQueuedConnectionsFailsOnAcceptEmfileAndDoesNotReportEmptyQueue) {
    NegotiatedLimits limits{};
    const auto runtime = videotest::private_runtime_dir();
    ASSERT_FALSE(runtime.empty());
    const auto path = video_socket_path_in(runtime);
    auto server = VideoServer::listen(path, VideoServerOptions{limits, current_uid()});
    ASSERT_TRUE(server);

    const int stale_fd = ::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    ASSERT_GE(stale_fd, 0);
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::memcpy(addr.sun_path, path.value.c_str(), path.value.size() + 1);
    ASSERT_EQ(::connect(stale_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)), 0);

    // Exhaust the descriptor budget so accept4 fails with EMFILE; drain must
    // report transport_io rather than treating accept failure as an empty queue.
    aa::core::Result<std::size_t> failed_drain{0U};
    {
        FdBudgetGuard fd_guard;
        ASSERT_TRUE(fd_guard.engage(/*free_slots=*/0));
        failed_drain = server.value().drain_queued_connections();
    }
    ASSERT_FALSE(failed_drain);
    EXPECT_EQ(failed_drain.error().code(), aa::ErrorCode::transport_io);

    // Once descriptors are available again, the stale peer is still in the
    // backlog and a retry drains it cleanly.
    const auto recovered_drain = server.value().drain_queued_connections();
    ASSERT_TRUE(recovered_drain);
    EXPECT_EQ(recovered_drain.value(), 1U);
    pollfd pfd{stale_fd, POLLIN, 0};
    ASSERT_EQ(::poll(&pfd, 1, 500), 1);
    std::array<std::uint8_t, kLimitsRecordBytes> buf{};
    EXPECT_EQ(::recv(stale_fd, buf.data(), buf.size(), MSG_DONTWAIT), 0);
    ::close(stale_fd);

    server.value().close();
    ::unlink(path.value.c_str());
    videotest::remove_startup_lock(path.value);
    ::rmdir((runtime + "/android-auto-receiver").c_str());
    ::rmdir(runtime.c_str());
}

TEST(VideoChannel, ConcurrentRetirementOnlyMapsShutdownErrnoAndPreservesTransportIo) {
    const std::vector<std::uint8_t> idr{0x00, 0x00, 0x01, 0x65, 0x77};

    // Case 1: owner retires concurrently during send and syscall returns EPIPE ->
    // transport_closed (both try_send_access_unit and send_access_unit).
    for (const int shutdown_errno : {EPIPE, ECONNRESET}) {
        for (const bool use_try : {true, false}) {
            auto pair = videotest::make_pair({});
            ASSERT_TRUE(pair);
            auto& owner = pair.value().producer;
            auto borrowed = owner.borrow();
            ASSERT_TRUE(borrowed);
            borrowed.value().set_datagram_send_for_test(
                [&](int, const void*, std::size_t, int, int& out_errno) -> std::ptrdiff_t {
                    owner.close();
                    out_errno = shutdown_errno;
                    return -1;
                });
            if (use_try) {
                const auto res = borrowed.value().try_send_access_unit({1, 100, true, idr});
                ASSERT_FALSE(res);
                EXPECT_EQ(res.error().code(), aa::ErrorCode::transport_closed);
            } else {
                const auto res = borrowed.value().send_access_unit({1, 100, true, idr});
                ASSERT_FALSE(res);
                EXPECT_EQ(res.error().code(), aa::ErrorCode::transport_closed);
            }
            EXPECT_TRUE(borrowed.value().retired());
        }
    }

    // Case 2: owner retires concurrently during send, but syscall fails with an
    // unrelated error (EIO) -> MUST preserve transport_io (never mask as
    // consumer replacement).
    for (const bool use_try : {true, false}) {
        auto pair = videotest::make_pair({});
        ASSERT_TRUE(pair);
        auto& owner = pair.value().producer;
        auto borrowed = owner.borrow();
        ASSERT_TRUE(borrowed);
        borrowed.value().set_datagram_send_for_test(
            [&](int, const void*, std::size_t, int, int& out_errno) -> std::ptrdiff_t {
                owner.close();
                out_errno = EIO;
                return -1;
            });
        if (use_try) {
            const auto res = borrowed.value().try_send_access_unit({1, 100, true, idr});
            ASSERT_FALSE(res);
            EXPECT_EQ(res.error().code(), aa::ErrorCode::transport_io);
        } else {
            const auto res = borrowed.value().send_access_unit({1, 100, true, idr});
            ASSERT_FALSE(res);
            EXPECT_EQ(res.error().code(), aa::ErrorCode::transport_io);
        }
        EXPECT_TRUE(borrowed.value().retired());
    }
}

TEST(VideoChannel, BlockingHandshakeRejectsRecordAfterPostWaitDeadlineExpiry) {
    NegotiatedLimits limits{};
    const auto runtime = videotest::private_runtime_dir();
    ASSERT_FALSE(runtime.empty());
    const auto path = video_socket_path_in(runtime);
    auto server = VideoServer::listen(path, VideoServerOptions{limits, current_uid()});
    ASSERT_TRUE(server);

    const auto t0 = std::chrono::steady_clock::time_point{std::chrono::seconds{100}};
    auto fake_now = t0;
    unsigned post_wait_step = 0;
    server.value().set_clock_for_test([&] {
        if (post_wait_step > 0) {
            // First clock read (pre-wait check) stays before the deadline so a
            // blocking wait (poll_wait > 0) runs; the second read (post-wait
            // deadline check after await_readable returns) is past the deadline.
            const unsigned call = post_wait_step++;
            return call == 1 ? (t0 + std::chrono::milliseconds{100})
                             : (t0 + std::chrono::milliseconds{250});
        }
        return fake_now;
    });

    const int client_fd = ::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    ASSERT_GE(client_fd, 0);
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::memcpy(addr.sun_path, path.value.c_str(), path.value.size() + 1);
    ASSERT_EQ(::connect(client_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)), 0);

    // Accept at t0 with a 200 ms handshake deadline (deadline = t0 + 200 ms).
    const auto first =
        server.value().accept_consumer(aa::core::Milliseconds{0}, aa::core::Milliseconds{200});
    ASSERT_FALSE(first);
    EXPECT_EQ(first.error().code(), aa::ErrorCode::timeout);
    EXPECT_TRUE(server.value().has_pending_handshake());

    // Read server AAL1 and queue client AAL1 so await_readable() returns
    // immediately on the next blocking accept_consumer() call.
    std::array<std::uint8_t, kLimitsRecordBytes> server_record{};
    pollfd srv_pfd{client_fd, POLLIN, 0};
    ASSERT_EQ(::poll(&srv_pfd, 1, 500), 1);
    ASSERT_EQ(::recv(client_fd, server_record.data(), server_record.size(), MSG_DONTWAIT),
              static_cast<ssize_t>(kLimitsRecordBytes));
    std::array<std::uint8_t, kLimitsRecordBytes> client_record{};
    encode_limits_record(client_record, limits);
    ASSERT_EQ(::send(client_fd, client_record.data(), client_record.size(), MSG_NOSIGNAL),
              static_cast<ssize_t>(kLimitsRecordBytes));

    // Arm the clock so pre-wait is at t0+100ms (blocking poll_wait=100ms) and
    // post-wait is at t0+250ms (past t0+200ms deadline). Must reject as timeout.
    post_wait_step = 1;
    const auto expired =
        server.value().accept_consumer(aa::core::Milliseconds{500}, aa::core::Milliseconds{200});
    ASSERT_FALSE(expired);
    EXPECT_EQ(expired.error().code(), aa::ErrorCode::timeout);
    EXPECT_FALSE(server.value().has_pending_handshake());
    ::close(client_fd);

    // Control check: when post-wait stays before the deadline, the blocking
    // handshake completes normally.
    post_wait_step = 0;
    fake_now = t0;
    const int ok_fd = ::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    ASSERT_GE(ok_fd, 0);
    ASSERT_EQ(::connect(ok_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)), 0);
    ASSERT_FALSE(
        server.value().accept_consumer(aa::core::Milliseconds{0}, aa::core::Milliseconds{200}));
    pollfd ok_pfd{ok_fd, POLLIN, 0};
    ASSERT_EQ(::poll(&ok_pfd, 1, 500), 1);
    ASSERT_EQ(::recv(ok_fd, server_record.data(), server_record.size(), MSG_DONTWAIT),
              static_cast<ssize_t>(kLimitsRecordBytes));
    ASSERT_EQ(::send(ok_fd, client_record.data(), client_record.size(), MSG_NOSIGNAL),
              static_cast<ssize_t>(kLimitsRecordBytes));
    fake_now = t0 + std::chrono::milliseconds{150};
    auto ok_prod =
        server.value().accept_consumer(aa::core::Milliseconds{500}, aa::core::Milliseconds{200});
    ASSERT_TRUE(ok_prod);
    ok_prod.value().close();
    ::close(ok_fd);

    server.value().close();
    ::unlink(path.value.c_str());
    videotest::remove_startup_lock(path.value);
    ::rmdir((runtime + "/android-auto-receiver").c_str());
    ::rmdir(runtime.c_str());
}

TEST(VideoChannel, PrepareSharedStateBeforeAal1ClosesSocketOnSetupFailureWithoutSendingAal1) {
    NegotiatedLimits limits{};
    const auto runtime = videotest::private_runtime_dir();
    ASSERT_FALSE(runtime.empty());
    const auto path = video_socket_path_in(runtime);
    auto server = VideoServer::listen(
        path, VideoServerOptions{limits, current_uid(), /*prepare_shared_state=*/true});
    ASSERT_TRUE(server);

    const int client_fd = ::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    ASSERT_GE(client_fd, 0);
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::memcpy(addr.sun_path, path.value.c_str(), path.value.size() + 1);
    ASSERT_EQ(::connect(client_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)), 0);

    // Pre-queue the client AAL1 record so that if the server sent its AAL1
    // before preparing shared state, accept_consumer would complete AAL1 first.
    std::array<std::uint8_t, kLimitsRecordBytes> client_record{};
    encode_limits_record(client_record, limits);
    ASSERT_EQ(::send(client_fd, client_record.data(), client_record.size(), MSG_NOSIGNAL),
              static_cast<ssize_t>(kLimitsRecordBytes));

    // Leave exactly 1 free fd: accept4 succeeds, memfd_create fails with EMFILE.
    aa::core::Result<VideoProducer> failed{aa::Error{aa::ErrorCode::timeout}};
    {
        FdBudgetGuard fd_guard;
        ASSERT_TRUE(fd_guard.engage(/*free_slots=*/1));
        failed = server.value().accept_consumer(aa::core::Milliseconds{0});
    }
    ASSERT_FALSE(failed);
    EXPECT_EQ(failed.error().code(), aa::ErrorCode::transport_io);
    EXPECT_FALSE(server.value().has_pending_handshake());

    // Client must NOT have received any AAL1 record — only EOF or ECONNRESET.
    pollfd pfd{client_fd, POLLIN, 0};
    ASSERT_EQ(::poll(&pfd, 1, 500), 1);
    std::array<std::uint8_t, kLimitsRecordBytes> rx{};
    errno = 0;
    const ssize_t got = ::recv(client_fd, rx.data(), rx.size(), MSG_DONTWAIT);
    EXPECT_TRUE(got == 0 || (got == -1 && errno == ECONNRESET))
        << "got=" << got << " errno=" << errno;
    ::close(client_fd);

    // With descriptors restored, a subsequent client completes AAL1 and gets a
    // pre-prepared 0600 size-sealed shared counter file.
    aa::core::Result<VideoConsumer> consumer{aa::Error{aa::ErrorCode::timeout}};
    std::thread connector([&] {
        consumer = connect_video_consumer(
            path, VideoClientOptions{limits, current_uid(), aa::core::Milliseconds{2000}});
    });
    auto producer = server.value().accept_consumer(aa::core::Milliseconds{2000});
    connector.join();
    ASSERT_TRUE(producer);
    ASSERT_TRUE(consumer);
    auto state_fd = producer.value().prepare_shared_state();
    ASSERT_TRUE(state_fd);
    const int seals = ::fcntl(state_fd.value(), F_GET_SEALS);
    ASSERT_GE(seals, 0);
    EXPECT_EQ(seals & (F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_SEAL),
              (F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_SEAL));

    producer.value().close();
    consumer.value().close();
    server.value().close();
    ::unlink(path.value.c_str());
    videotest::remove_startup_lock(path.value);
    ::rmdir((runtime + "/android-auto-receiver").c_str());
    ::rmdir(runtime.c_str());
}
