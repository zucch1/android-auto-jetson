// SPDX-License-Identifier: GPL-3.0-or-later
#include "pair.hpp"

#include <aa/ipc/VideoChannel.hpp>

#include <chrono>
#include <cstdint>
#include <sys/socket.h>
#include <vector>

#include <gtest/gtest.h>

namespace {
using namespace aa::ipc;

std::vector<std::uint8_t> blob(std::size_t size) { return std::vector<std::uint8_t>(size, 0x7a); }

aa::core::Result<ReceiveStatus> drain_until_complete(VideoConsumer& consumer, int attempts) {
    for (int i = 0; i < attempts; ++i) {
        auto step = consumer.receive(aa::core::Milliseconds{500});
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

TEST(VideoBackpressure, SlowConsumerDropsToIdrThenRecovers) {
    NegotiatedLimits limits{};
    limits.socket_budget_bytes = 16384;
    auto pair = videotest::make_pair(limits);
    ASSERT_TRUE(pair);
    auto& producer = pair.value().producer;
    auto& consumer = pair.value().consumer;

    ASSERT_TRUE(producer.send_access_unit({1, 1, true, blob(8192)}));
    ASSERT_TRUE(drain_until_complete(consumer, 4));

    std::uint64_t accepted = 0;
    for (std::uint64_t frame = 2; frame < 400; ++frame) {
        const auto outcome = producer.send_access_unit({frame, frame, false, blob(16384)});
        ASSERT_TRUE(outcome);
        if (outcome.value() == SendStatus::sent) {
            ++accepted;
        }
    }
    EXPECT_GT(producer.dropped(), 0U);
    EXPECT_GT(producer.backpressure_events(), 0U);
    EXPECT_LT(accepted, 398U);

    std::uint64_t delivered = 0;
    for (int i = 0; i < 600; ++i) {
        auto step = consumer.receive(aa::core::Milliseconds{50});
        if (!step) {
            break;
        }
        if (step.value() == ReceiveStatus::complete) {
            ++delivered;
        }
    }
    EXPECT_GT(delivered, 0U);
    EXPECT_EQ(consumer.rejected(), 0U);

    const auto resume = std::chrono::steady_clock::now();
    ASSERT_TRUE(producer.send_access_unit({900, 900, true, blob(8192)}));
    auto recovered = drain_until_complete(consumer, 800);
    ASSERT_TRUE(recovered);
    const auto recovery_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - resume);
    EXPECT_TRUE(consumer.access_unit().idr);
    EXPECT_EQ(consumer.access_unit().frame, 900U);
    EXPECT_GT(consumer.resyncs(), 0U);
    EXPECT_LE(recovery_ms.count(), 250);
}

TEST(VideoBackpressure, NonIdrIsSuppressedWhileWaitingForIdr) {
    NegotiatedLimits limits{};
    auto pair = videotest::make_pair(limits);
    ASSERT_TRUE(pair);
    auto& producer = pair.value().producer;
    auto& consumer = pair.value().consumer;

    ASSERT_TRUE(producer.send_access_unit({1, 1, true, blob(1024)}));
    ASSERT_TRUE(drain_until_complete(consumer, 4));
    EXPECT_FALSE(consumer.waiting_idr());

    std::vector<std::uint8_t> gap(kFrameHeaderBytes + 4, 0x33);
    encode_frame_header(gap, VideoFrameHeader{2, 50000, 2, 0, 1, 4, 4, false});
    (void)::send(producer.native_handle(), gap.data(), gap.size(), MSG_NOSIGNAL);
    const auto suppressed_by_gap = consumer.receive(aa::core::Milliseconds{500});
    ASSERT_TRUE(suppressed_by_gap);
    EXPECT_EQ(suppressed_by_gap.value(), ReceiveStatus::waiting);
    EXPECT_TRUE(consumer.waiting_idr());

    ASSERT_TRUE(producer.send_access_unit({3, 3, false, blob(1024)}));
    const auto suppressed = consumer.receive(aa::core::Milliseconds{500});
    ASSERT_TRUE(suppressed);
    EXPECT_EQ(suppressed.value(), ReceiveStatus::waiting);
    EXPECT_GT(consumer.suppressed(), 0U);

    ASSERT_TRUE(producer.send_access_unit({4, 4, true, blob(1024)}));
    auto recovered = drain_until_complete(consumer, 8);
    ASSERT_TRUE(recovered);
    EXPECT_TRUE(consumer.access_unit().idr);
    EXPECT_EQ(consumer.access_unit().frame, 4U);
}
