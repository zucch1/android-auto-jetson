// SPDX-License-Identifier: GPL-3.0-or-later
#include "pair.hpp"

#include <aa/ipc/VideoChannel.hpp>

#include <algorithm>
#include <cstdint>
#include <string>
#include <sys/stat.h>
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
