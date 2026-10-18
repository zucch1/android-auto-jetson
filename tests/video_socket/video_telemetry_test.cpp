// SPDX-License-Identifier: GPL-3.0-or-later
#include "pair.hpp"

#include <gtest/gtest.h>
#include <optional>

TEST(VideoStream, TelemetryMeasuresActualEndpointsWithoutReplacingAvTimestamp) {
    // Given a fragmented AU and observers on the real socket endpoints.
    auto pair = videotest::make_pair({});
    ASSERT_TRUE(pair);
    std::optional<aa::ipc::VideoEndpoint> sent;
    std::optional<aa::ipc::VideoEndpoint> received;
    pair.value().producer.set_observer([&](const auto& event) { sent = event; });
    pair.value().consumer.set_observer([&](const auto& event) { received = event; });
    const std::vector<std::uint8_t> payload(aa::ipc::kMaxDatagramPayload + 9, 42);
    const auto before = aa::ipc::video_monotonic_ns();
    // When a full access unit traverses the production channel.
    ASSERT_TRUE(pair.value().producer.send_access_unit({7, 123, true, payload}));
    ASSERT_TRUE(pair.value().consumer.receive(aa::core::Milliseconds{2000}));
    EXPECT_FALSE(received);
    ASSERT_TRUE(pair.value().consumer.receive(aa::core::Milliseconds{2000}));
    const auto after = aa::ipc::video_monotonic_ns();
    // Then endpoint times bracket the transfer, with unchanged wire identity.
    ASSERT_TRUE(sent);
    ASSERT_TRUE(received);
    EXPECT_GE(sent->observed_ns, before);
    EXPECT_GE(received->observed_ns, sent->observed_ns);
    EXPECT_LE(received->observed_ns, after);
    EXPECT_EQ(sent->frame, received->frame);
    EXPECT_EQ(sent->sequence, received->sequence);
    EXPECT_EQ(sent->timestamp_ns, 123U);
    EXPECT_EQ(received->timestamp_ns, 123U);
    EXPECT_EQ(sent->producer_pid, received->producer_pid);
    EXPECT_EQ(sent->consumer_pid, received->consumer_pid);
    EXPECT_EQ(received->payload_bytes, payload.size());
    EXPECT_EQ(pair.value().consumer.access_unit().timestamp_ns, 123U);
}

TEST(VideoStream, SuppressedSendHasNoTelemetrySample) {
    // Given a fresh producer still waiting for IDR.
    auto pair = videotest::make_pair({});
    ASSERT_TRUE(pair);
    std::size_t samples{};
    pair.value().producer.set_observer([&](const auto&) { ++samples; });
    const std::vector<std::uint8_t> payload{1};
    // When a non-IDR is suppressed.
    const auto status = pair.value().producer.send_access_unit({1, 99, false, payload});
    // Then there is no successful send endpoint to correlate.
    ASSERT_TRUE(status);
    EXPECT_EQ(status.value(), aa::ipc::SendStatus::dropped_waiting_idr);
    EXPECT_EQ(samples, 0U);
}
