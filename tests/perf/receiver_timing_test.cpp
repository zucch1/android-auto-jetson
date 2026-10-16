// SPDX-License-Identifier: GPL-3.0-or-later
// Exercise the shipped wiring, not a duplicate cadence implementation.
#define main wired_receiver_main
#include "../../tools/wired-receiver/main.cpp" // NOLINT(bugprone-suspicious-include): exercises the real receiver sink
#undef main

#include "../../src/audio/AudioSinkFactory.hpp"
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
