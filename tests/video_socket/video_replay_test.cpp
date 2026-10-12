// SPDX-License-Identifier: GPL-3.0-or-later
#include "pair.hpp"

#include <aa/ipc/ReplayVideoCapture.hpp>
#include <aa/ipc/VideoChannel.hpp>
#include <aa/replay/Recorder.hpp>
#include <aa/replay/Schema.hpp>

#include <cstddef>
#include <cstdint>
#include <vector>

#include <gtest/gtest.h>

namespace {
using namespace aa::ipc;
namespace replay = aa::replay;

std::vector<std::uint8_t> blob(std::size_t size) { return std::vector<std::uint8_t>(size, 0x5a); }

aa::core::Result<ReceiveStatus> drain_until_complete(VideoConsumer& consumer, int attempts) {
    for (int i = 0; i < attempts; ++i) {
        auto step = consumer.receive(aa::core::Milliseconds{1000});
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

TEST(VideoReplay, StreamCaptureProducesLoadableFixture) {
    NegotiatedLimits limits{};
    auto pair = videotest::make_pair(limits);
    ASSERT_TRUE(pair);

    replay::Recorder recorder{};
    ASSERT_TRUE(recorder.set_metadata(
        aa::diagnostics::EventField::text("event", "video-capture")));
    replay::Expectation expect{};
    expect.states = {"disconnected"};
    ASSERT_TRUE(recorder.set_expectation(expect));
    ReplayVideoCapture capture{recorder};
    pair.value().consumer.set_capture(&capture);

    const auto idr_payload = blob(kMaxDatagramPayload + 3);
    ASSERT_TRUE(pair.value().producer.send_access_unit({1, 10, true, idr_payload}));
    ASSERT_TRUE(drain_until_complete(pair.value().consumer, 4));
    const auto p_payload = blob(64);
    ASSERT_TRUE(pair.value().producer.send_access_unit({2, 20, false, p_payload}));
    ASSERT_TRUE(drain_until_complete(pair.value().consumer, 2));

    auto exported = recorder.export_fixture();
    ASSERT_TRUE(exported) << (exported ? 0 : static_cast<int>(exported.error().code()));
    ASSERT_FALSE(exported.value().file_bytes.empty());
    EXPECT_EQ(exported.value().sha256_hex.size(), 64U);

    auto loaded = replay::load_fixture(exported.value().file_bytes);
    ASSERT_TRUE(loaded);
    std::size_t video_records = 0;
    for (const auto& record : loaded.value().records) {
        if (record.kind != replay::RecordKind::video) {
            continue;
        }
        const auto& video = std::get<replay::VideoRecord>(record.body);
        if (video_records == 0) {
            EXPECT_EQ(video.frame, 1U);
            EXPECT_EQ(video.sequence, 0U);
            EXPECT_EQ(video.timestamp, aa::core::Nanoseconds{10});
            EXPECT_TRUE(video.idr);
            EXPECT_EQ(video.payload.size(), idr_payload.size());
        } else {
            EXPECT_EQ(video.frame, 2U);
            EXPECT_EQ(video.sequence, 2U);
            EXPECT_EQ(video.timestamp, aa::core::Nanoseconds{20});
            EXPECT_FALSE(video.idr);
            EXPECT_EQ(video.payload.size(), p_payload.size());
        }
        ++video_records;
    }
    EXPECT_EQ(video_records, 2U);
    EXPECT_TRUE(replay::check_fixture(exported.value().file_bytes));
}

TEST(VideoReplay, CaptureReportsRecorderRejections) {
    NegotiatedLimits limits{};
    auto pair = videotest::make_pair(limits);
    ASSERT_TRUE(pair);
    replay::Recorder recorder{};
    replay::Expectation expect{};
    expect.states = {"disconnected"};
    ASSERT_TRUE(recorder.set_expectation(expect));
    ReplayVideoCapture capture{recorder};
    pair.value().consumer.set_capture(&capture);

    ASSERT_TRUE(pair.value().producer.send_access_unit({1, 5'000'000, true, blob(16)}));
    const auto first = pair.value().consumer.receive(aa::core::Milliseconds{1000});
    ASSERT_TRUE(first);
    EXPECT_EQ(first.value(), ReceiveStatus::complete);

    ASSERT_TRUE(pair.value().producer.send_access_unit({2, 1, false, blob(16)}));
    const auto backdated = pair.value().consumer.receive(aa::core::Milliseconds{1000});
    ASSERT_FALSE(backdated);
    EXPECT_EQ(backdated.error().code(), aa::ErrorCode::invalid_argument);

    ASSERT_TRUE(pair.value().producer.send_access_unit({3, 9'000'000, true, blob(16)}));
    const auto usable = pair.value().consumer.receive(aa::core::Milliseconds{1000});
    ASSERT_TRUE(usable);
    EXPECT_EQ(usable.value(), ReceiveStatus::complete);
    EXPECT_EQ(pair.value().consumer.access_unit().frame, 3U);
}
