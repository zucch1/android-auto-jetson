// SPDX-License-Identifier: GPL-3.0-or-later
// Exercise the shipped wiring, not a duplicate cadence implementation.
#define main wired_receiver_main
#include "../../tools/wired-receiver/main.cpp" // NOLINT(bugprone-suspicious-include): exercises the real receiver sink
#undef main

#include "../../src/audio/AudioSinkFactory.hpp"
#include "../video_socket/pair.hpp"
#include <gtest/gtest.h>
#include <regex>

namespace {
class LatePeakSink final : public aa::audio::detail::ObservedAudioSink {
  public:
    aa::core::Result<void> write(const aa::audio::MixedChunk &) override {
        if (++writes_ == 80) {
            std::this_thread::sleep_for(std::chrono::milliseconds{80});
        }
        return {};
    }
    aa::core::Result<void> drain(aa::core::Milliseconds) override { return {}; }
    aa::audio::SinkProbeStats probe_stats() const override { return {}; }
    aa::audio::RenderBarrierStats render_stats() const override { return {}; }
  private:
    int writes_{};
};
}

TEST(ReceiverTiming, ShutdownRetainsPeakAfterLastPeriodicEmission) {
    // Given 90 writes, with a spike on write 80 after the last cadence at 51.
    auto router = aa::audio::detail::open_router_with_sink_for_test(
        pcm_router_config(), std::make_unique<LatePeakSink>());
    ASSERT_TRUE(router.has_value());
    const auto base = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    const std::vector<std::byte> pcm(4, std::byte{1});
    testing::internal::CaptureStdout();
    {
        RouterSink sink(router.value());
        for (int index = 0; index < 90; ++index) {
            aa::protocol::MessageView message{};
            message.header.channel = aa::protocol::ChannelRole::media_audio;
            message.header.timestamp = aa::core::Nanoseconds{base + index * 20'834LL};
            message.payload = pcm;
            EXPECT_TRUE(sink.on_message(message).has_value());
        }
        // When the sink leaves scope (also used on early-return shutdown).
    }
    const auto output = testing::internal::GetCapturedStdout();
    double emitted_peak = 0;
    const std::regex latency{"\"audio_latency_ms\":([0-9.e+-]+)"};
    for (auto it = std::sregex_iterator(output.begin(), output.end(), latency);
         it != std::sregex_iterator(); ++it) {
        emitted_peak = std::max(emitted_peak, std::stod((*it)[1].str()));
    }
    // Then the final cumulative maximum, not the write-51 snapshot, is logged.
    EXPECT_GE(emitted_peak, 70.0) << output;
    EXPECT_DOUBLE_EQ(emitted_peak,
        static_cast<double>(router.value().report().stats.max_path_latency.count) / 1e6);
}

TEST(ReceiverTiming, EmitsActualAudioWriteCompletionWithScheduledInterval) {
    auto router = aa::audio::detail::open_router_with_sink_for_test(
        pcm_router_config(), std::make_unique<LatePeakSink>());
    ASSERT_TRUE(router.has_value());
    const std::vector<std::byte> pcm(4, std::byte{1});
    const auto start = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    aa::protocol::MessageView message{};
    message.header.channel = aa::protocol::ChannelRole::media_audio;
    message.header.timestamp = aa::core::Nanoseconds{start};
    message.payload = pcm;
    testing::internal::CaptureStdout();
    {
        RouterSink sink(router.value());
        EXPECT_TRUE(sink.on_message(message).has_value());
    }
    const auto output = testing::internal::GetCapturedStdout();
    const auto end = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    std::smatch match;
    ASSERT_TRUE(std::regex_search(output, match, std::regex{
        "\"event\":\"audio_write_completed\",\"scheduled_ns\":([0-9]+),"
        "\"scheduled_end_ns\":([0-9]+),\"completed_ns\":([0-9]+)"})) << output;
    EXPECT_EQ(std::stoll(match[1].str()), start);
    EXPECT_GT(std::stoll(match[2].str()), start);
    EXPECT_GE(std::stoll(match[3].str()), start);
    EXPECT_LE(std::stoll(match[3].str()), end);
}

TEST(ReceiverTiming, VideoDropsIncludeFinalTailAfterPeriodicEmission) {
    // Given an outlet without a consumer and 29 real input access units.
    testing::internal::CaptureStdout();
    {
        VideoOutlet outlet{log_line};
        aa::protocol::MessageView message{};
        for (int frame = 0; frame < 29; ++frame) {
            ASSERT_TRUE(outlet.on_message(message));
        }
        // When teardown flushes the cumulative drop tail.
    }
    const auto output = testing::internal::GetCapturedStdout();
    // Then both the initial observation and final actual count are exported.
    EXPECT_NE(output.find("\"frame_drops\":1}"), std::string::npos);
    EXPECT_NE(output.find("\"frame_drops\":29}"), std::string::npos);
}

TEST(ReceiverTiming, VideoWithoutInputKeepsDropMeasurementAbsent) {
    // Given an outlet which never observed input.
    testing::internal::CaptureStdout();
    { VideoOutlet outlet{log_line}; }
    // When it is destroyed, then no zero-valued observation is invented.
    const auto output = testing::internal::GetCapturedStdout();
    EXPECT_EQ(output.find("frame_drops"), std::string::npos);
}

TEST(ReceiverTiming, ActualSocketBackpressureContributesToOutletDrops) {
    // Given a bounded socket consumer which never reads its queued frames.
    auto pair = videotest::make_pair({});
    ASSERT_TRUE(pair);
    std::vector<std::byte> payload(16UL * 1024, std::byte{0});
    payload[2] = std::byte{1};
    payload[3] = std::byte{0x65};
    aa::protocol::MessageView message{};
    message.payload = payload;
    testing::internal::CaptureStdout();
    {
        VideoOutlet outlet{log_line};
        outlet.install(std::move(pair.value().producer));
        // When more IDR access units are offered than the kernel budget can hold.
        for (int frame = 0; frame < 64; ++frame) {
            EXPECT_TRUE(outlet.on_message(message));
        }
    }
    const auto output = testing::internal::GetCapturedStdout();
    // Then the observed backpressure reason and a positive final count survive.
    EXPECT_NE(output.find("\"reason\":\"backpressure\""), std::string::npos);
    std::smatch match;
    ASSERT_TRUE(std::regex_search(output, match, std::regex{"\"frame_drops\":([1-9][0-9]*)"}));
    EXPECT_GT(std::stoull(match[1].str()), 0U);
}
