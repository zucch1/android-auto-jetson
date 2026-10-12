// SPDX-License-Identifier: GPL-3.0-or-later
#include "pair.hpp"

#include <aa/ipc/VideoChannel.hpp>

#include <cstdint>
#include <thread>
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
