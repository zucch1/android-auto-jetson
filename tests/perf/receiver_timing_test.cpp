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

class ReceiverTiming : public testing::Test {
  protected:
    aa::wired_receiver::StdoutLogScope logging{g_running};

    void capture_stdout() {
        ASSERT_FALSE(logging.log.flush());
        testing::internal::CaptureStdout();
    }
    std::string captured_stdout() {
        EXPECT_FALSE(logging.log.flush());
        return testing::internal::GetCapturedStdout();
    }
};
}

TEST_F(ReceiverTiming, ShutdownRetainsPeakAfterLastPeriodicEmission) {
    // Given 90 writes, with a spike on write 80 after the last cadence at 51.
    const auto base = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    const std::vector<std::byte> pcm(4, std::byte{1});
    capture_stdout();
    std::optional<aa::audio::RouterReport> report;
    {
        ConcreteAudioRenderWorker sink{pcm_router_config(), [](const auto &config) {
            return aa::audio::detail::open_router_with_sink_for_test(
                config, std::make_unique<LatePeakSink>());
        }};
        ASSERT_TRUE(sink.ready());
        for (int index = 0; index < 90; ++index) {
            aa::protocol::MessageView message{};
            message.header.channel = aa::protocol::ChannelRole::media_audio;
            message.header.timestamp = aa::core::Nanoseconds{base + index * 20'834LL};
            message.payload = pcm;
            EXPECT_TRUE(sink.submit(message, {message.header.channel, 0,
                                             static_cast<std::uint64_t>(index), 1}));
            EXPECT_TRUE(sink.take_completion(std::chrono::steady_clock::now() +
                                             std::chrono::seconds{2}));
            ASSERT_TRUE(sink.wait_idle(std::chrono::steady_clock::now() +
                                       std::chrono::seconds{2}));
        }
        EXPECT_TRUE(sink.finish());
        report = sink.final_report();
        // When the sink leaves scope (also used on early-return shutdown).
    }
    const auto output = captured_stdout();
    double emitted_peak = 0;
    const std::regex latency{"\"audio_latency_ms\":([0-9.e+-]+)"};
    for (auto it = std::sregex_iterator(output.begin(), output.end(), latency);
         it != std::sregex_iterator(); ++it) {
        emitted_peak = std::max(emitted_peak, std::stod((*it)[1].str()));
    }
    // Then the final cumulative maximum, not the write-51 snapshot, is logged.
    EXPECT_GE(emitted_peak, 70.0) << output;
    EXPECT_DOUBLE_EQ(emitted_peak,
        static_cast<double>(report->stats.max_path_latency.count) / 1e6);
}

TEST_F(ReceiverTiming, EmitsActualAudioWriteCompletionWithScheduledInterval) {
    const std::vector<std::byte> pcm(4, std::byte{1});
    const auto start = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    aa::protocol::MessageView message{};
    message.header.channel = aa::protocol::ChannelRole::media_audio;
    message.header.timestamp = aa::core::Nanoseconds{start};
    message.payload = pcm;
    capture_stdout();
    {
        ConcreteAudioRenderWorker sink{pcm_router_config(), [](const auto &config) {
            return aa::audio::detail::open_router_with_sink_for_test(
                config, std::make_unique<LatePeakSink>());
        }};
        ASSERT_TRUE(sink.ready());
        EXPECT_TRUE(sink.submit(message, {message.header.channel, 0, 1, 1}));
        EXPECT_TRUE(sink.take_completion(std::chrono::steady_clock::now() +
                                         std::chrono::seconds{2}));
        ASSERT_TRUE(sink.wait_idle(std::chrono::steady_clock::now() + std::chrono::seconds{2}));
    }
    const auto output = captured_stdout();
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

TEST_F(ReceiverTiming, VideoDropsIncludeFinalTailAfterPeriodicEmission) {
    // Given an outlet without a consumer and 29 real input access units.
    capture_stdout();
    {
        VideoOutlet outlet{log_line};
        aa::protocol::MessageView message{};
        for (int frame = 0; frame < 29; ++frame) {
            ASSERT_TRUE(outlet.on_message(message));
        }
        // When teardown flushes the cumulative drop tail.
    }
    const auto output = captured_stdout();
    // Then both the initial observation and final actual count are exported.
    EXPECT_NE(output.find("\"frame_drops\":1}"), std::string::npos);
    EXPECT_NE(output.find("\"frame_drops\":29}"), std::string::npos);
}

TEST_F(ReceiverTiming, VideoWithoutInputKeepsDropMeasurementAbsent) {
    // Given an outlet which never observed input.
    capture_stdout();
    { VideoOutlet outlet{log_line}; }
    // When it is destroyed, then no zero-valued observation is invented.
    const auto output = captured_stdout();
    EXPECT_EQ(output.find("frame_drops"), std::string::npos);
}

TEST_F(ReceiverTiming, ActualSocketBackpressureContributesToOutletDrops) {
    // Given a bounded socket consumer which never reads its queued frames.
    auto pair = videotest::make_pair({});
    ASSERT_TRUE(pair);
    std::vector<std::byte> payload(16UL * 1024, std::byte{0});
    payload[2] = std::byte{1};
    payload[3] = std::byte{0x65};
    aa::protocol::MessageView message{};
    message.payload = payload;
    capture_stdout();
    {
        VideoOutlet outlet{log_line};
        outlet.install(std::move(pair.value().producer));
        // When more IDR access units are offered than the kernel budget can hold.
        for (int frame = 0; frame < 64; ++frame) {
            EXPECT_TRUE(outlet.on_message(message));
        }
    }
    const auto output = captured_stdout();
    // Then the observed backpressure reason and a positive final count survive.
    EXPECT_NE(output.find("\"reason\":\"backpressure\""), std::string::npos);
    std::smatch match;
    ASSERT_TRUE(std::regex_search(output, match, std::regex{"\"frame_drops\":([1-9][0-9]*)"}));
    EXPECT_GT(std::stoull(match[1].str()), 0U);
}

TEST_F(ReceiverTiming, CurrentPcmProfilesEmitCompletionAndFlushOnlyOnce) {
    // Given the production media and resampled voice profiles.
    for (const auto role : {aa::protocol::ChannelRole::media_audio,
                            aa::protocol::ChannelRole::guidance_audio,
                            aa::protocol::ChannelRole::system_audio}) {
        SCOPED_TRACE(static_cast<int>(role));
        const auto voice = role != aa::protocol::ChannelRole::media_audio;
        const std::vector<std::byte> pcm(voice ? 640 : 3840, std::byte{1});
        const auto start = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        capture_stdout();
        {
            ConcreteAudioRenderWorker sink{pcm_router_config(), [](const auto &config) {
                return aa::audio::detail::open_router_with_sink_for_test(
                    config, std::make_unique<LatePeakSink>());
            }};
            ASSERT_TRUE(sink.ready());
            // When real PCM is delivered, then explicitly finalized before destruction.
            EXPECT_TRUE(sink.submit({{role, 1, {start}}, pcm}, {role, 0, 1, 1}));
            EXPECT_TRUE(sink.take_completion(std::chrono::steady_clock::now() +
                                             std::chrono::seconds{2}));
            ASSERT_TRUE(sink.wait_idle(std::chrono::steady_clock::now() +
                                       std::chrono::seconds{2}));
            EXPECT_TRUE(sink.finish());
            EXPECT_TRUE(sink.finish());
        }
        const auto output = captured_stdout();
        // Then every path reports a real write interval and exactly one final flush.
        std::smatch completion;
        ASSERT_TRUE(std::regex_search(output, completion, std::regex{
            "\"event\":\"audio_write_completed\",\"scheduled_ns\":([0-9]+),"
            "\"scheduled_end_ns\":([0-9]+),\"completed_ns\":([0-9]+)"})) << output;
        EXPECT_EQ(std::stoll(completion[1].str()), start);
        EXPECT_GT(std::stoll(completion[2].str()), start);
        EXPECT_LE(std::stoll(completion[2].str()), start + 20'000'000);
        EXPECT_GE(std::stoll(completion[3].str()), start);
        const std::regex timing{"\"event\":\"media_timing\""};
        EXPECT_EQ(std::distance(std::sregex_iterator(output.begin(), output.end(), timing),
                                std::sregex_iterator()), 2);
        EXPECT_NE(output.find("\"audio_scheduling_skew_ms\":"), std::string::npos);
        EXPECT_EQ(output.find("\"av_sync_ms\":"), std::string::npos);
    }
}

TEST_F(ReceiverTiming, EmptyRouteDoesNotInventFinalTiming) {
    // Given an opened route without any delivered PCM.
    capture_stdout();
    {
        ConcreteAudioRenderWorker sink{pcm_router_config(), [](const auto &config) {
            return aa::audio::detail::open_router_with_sink_for_test(
                config, std::make_unique<LatePeakSink>());
        }};
        ASSERT_TRUE(sink.ready());
        // When shutdown finalizes the untouched route.
        EXPECT_TRUE(sink.finish());
    }
    // Then neither audio delivery nor timing is fabricated.
    EXPECT_TRUE(captured_stdout().empty());
}
