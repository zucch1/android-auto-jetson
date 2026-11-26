// SPDX-License-Identifier: GPL-3.0-or-later
// Task 25 AAP audio pipeline tests: four-channel decode -> duck/mix -> routing
// element with per-channel semantics, backend honesty, degraded typed failure
// and the fixture-tested microphone seam (disabled by default). Gate fix round
// 2 regressions: sample-timeline mixing of staggered channels (F2), the
// render-completion barrier vs element-ingress distinction and drain-before-
// teardown (F1), missing-default-output and mid-stream output loss degrade
// paths (F3), decode-inclusive path latency (F4), availability-agnostic AAC
// probe reporting with deterministic policy fixtures (F5).
#include <aa/audio/AudioDecode.hpp>
#include <aa/audio/AudioMix.hpp>
#include <aa/audio/AudioRouter.hpp>
#include <aa/audio/AudioTelemetry.hpp>
#include <aa/audio/Microphone.hpp>
#include <aa/protocol/Protocol.hpp>

#include "../../src/audio/AudioSinkFactory.hpp"
#include "../../src/audio/RateCorrection.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <functional>
#include <iterator>
#include <mutex>
#include <optional>
#include <random>
#include <signal.h>
#include <sys/stat.h>
#include <cerrno>
#include <cstring>
#include <array>
#include <sstream>
#include <spawn.h>
#include <string>
#include <thread>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

extern char** environ;

namespace {

constexpr std::int16_t kMediaLevel = 1000;
constexpr std::int16_t kGuidanceLevel = 2000;
constexpr std::int16_t kSystemLevel = 3000;
constexpr std::int16_t kSpeechLevel = 4000;

constexpr std::int16_t kMediaAlone = 1000;
constexpr std::int16_t kGuidanceAlone = 2000;
constexpr std::int16_t kSystemAlone = 1500;
constexpr std::int16_t kSpeechAlone = 4000;
constexpr std::int16_t kMediaWithSystem = 2500;
constexpr std::int16_t kMediaWithGuidance = 2250;
constexpr std::int16_t kMediaWithSpeech = 4250;
constexpr std::int16_t kAllFourDucked = 7750;
constexpr std::int16_t kMediaDuckedAlone = 250;

[[nodiscard]] std::string fixture_path(const std::string& name) {
    return std::string{AA_AUDIO_FIXTURE_DIR} + "/" + name;
}

[[nodiscard]] std::vector<std::byte> read_file(const std::string& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return {};
    }
    const std::string bytes{std::istreambuf_iterator<char>{input},
                            std::istreambuf_iterator<char>{}};
    std::vector<std::byte> out(bytes.size());
    if (!bytes.empty()) {
        std::memcpy(out.data(), bytes.data(), bytes.size());
    }
    return out;
}

struct Tone final {
    std::int16_t level{};
    std::size_t frames{};
};

[[nodiscard]] std::vector<std::byte> constant_pcm(Tone tone) {
    std::vector<std::byte> pcm(tone.frames * 2 * sizeof(std::int16_t));
    for (std::size_t i = 0; i < tone.frames * 2; ++i) {
        std::memcpy(pcm.data() + i * sizeof(tone.level), &tone.level, sizeof(tone.level));
    }
    return pcm;
}

[[nodiscard]] aa::audio::AudioFormat stereo_48k() { return aa::audio::AudioFormat{48000, 2, 16}; }

[[nodiscard]] aa::protocol::MessageView make_message(aa::protocol::ChannelRole channel,
                                                     std::uint64_t sequence,
                                                     aa::core::Nanoseconds timestamp,
                                                     const std::vector<std::byte>& payload) {
    return aa::protocol::MessageView{
        aa::protocol::MessageHeader{channel, sequence, timestamp},
        std::span<const std::byte>{payload.data(), payload.size()}};
}

[[nodiscard]] std::vector<std::vector<std::byte>> split_adts(const std::vector<std::byte>& stream) {
    std::vector<std::vector<std::byte>> frames;
    std::size_t offset = 0;
    while (offset + 7 <= stream.size()) {
        const auto* header = reinterpret_cast<const unsigned char*>(stream.data() + offset);
        if (header[0] != 0xFF || (header[1] & 0xF6U) != 0xF0U) {
            break;
        }
        const std::size_t length = (static_cast<std::size_t>(header[3] & 0x03U) << 11)
            | (static_cast<std::size_t>(header[4]) << 3)
            | (static_cast<std::size_t>(header[5] & 0xE0U) >> 5);
        if (length < 7 || offset + length > stream.size()) {
            break;
        }
        frames.emplace_back(stream.begin() + static_cast<std::ptrdiff_t>(offset),
                            stream.begin() + static_cast<std::ptrdiff_t>(offset + length));
        offset += length;
    }
    return frames;
}

[[nodiscard]] aa::core::Nanoseconds now_ns() {
    return aa::core::Nanoseconds{std::chrono::duration_cast<std::chrono::nanoseconds>(
                                     std::chrono::steady_clock::now().time_since_epoch())
                                     .count()};
}

class AudioPipelineMix : public ::testing::Test {
protected:
    aa::audio::MixPolicy policy_{};
    aa::audio::AudioMixer mixer_{policy_};
    std::vector<std::byte> media_pcm_{constant_pcm({kMediaLevel, 960})};
    std::vector<std::byte> guidance_pcm_{constant_pcm({kGuidanceLevel, 960})};
    std::vector<std::byte> system_pcm_{constant_pcm({kSystemLevel, 960})};
    std::vector<std::byte> speech_pcm_{constant_pcm({kSpeechLevel, 960})};
    aa::audio::PcmChunk media_{aa::audio::Role::media, stereo_48k(), aa::core::Nanoseconds{0},
                               std::span<const std::byte>{media_pcm_}};
    aa::audio::PcmChunk guidance_{aa::audio::Role::guidance, stereo_48k(), aa::core::Nanoseconds{0},
                                  std::span<const std::byte>{guidance_pcm_}};
    aa::audio::PcmChunk system_{aa::audio::Role::system, stereo_48k(), aa::core::Nanoseconds{0},
                                std::span<const std::byte>{system_pcm_}};
    aa::audio::PcmChunk speech_{aa::audio::Role::speech, stereo_48k(), aa::core::Nanoseconds{0},
                                std::span<const std::byte>{speech_pcm_}};

    [[nodiscard]] std::int16_t mix_level(const aa::audio::PcmChunk* media,
                                         const aa::audio::PcmChunk* guidance,
                                         const aa::audio::PcmChunk* system,
                                         const aa::audio::PcmChunk* speech,
                                         aa::core::Nanoseconds timestamp) {
        aa::audio::MixFrame frame;
        frame.timestamp = timestamp;
        frame.samples = 960;
        frame.roles = {media, guidance, system, speech};
        auto mixed = mixer_.mix(frame);
        EXPECT_TRUE(mixed.has_value());
        if (!mixed.has_value()) {
            return 0;
        }
        std::int16_t first{};
        std::memcpy(&first, mixed.value().pcm.data(), sizeof(first));
        return first;
    }
};

} // namespace

TEST_F(AudioPipelineMix, PerRoleGainsMatchDocumentedRatios) {
    EXPECT_EQ(mix_level(&media_, nullptr, nullptr, nullptr, aa::core::Nanoseconds{0}), kMediaAlone);
    EXPECT_EQ(mix_level(nullptr, &guidance_, nullptr, nullptr, aa::core::Nanoseconds{1'000'000}),
              kGuidanceAlone);
    EXPECT_EQ(mix_level(nullptr, nullptr, &system_, nullptr, aa::core::Nanoseconds{2'000'000}),
              kSystemAlone);
    EXPECT_EQ(mix_level(nullptr, nullptr, nullptr, &speech_, aa::core::Nanoseconds{3'000'000}),
              kSpeechAlone);
}

TEST_F(AudioPipelineMix, SpeechAndGuidanceDuckMedia) {
    EXPECT_EQ(mix_level(&media_, nullptr, nullptr, &speech_, aa::core::Nanoseconds{0}),
              kMediaWithSpeech);
    mixer_ = aa::audio::AudioMixer{policy_};
    EXPECT_EQ(mix_level(&media_, &guidance_, nullptr, nullptr, aa::core::Nanoseconds{0}),
              kMediaWithGuidance);
}

TEST_F(AudioPipelineMix, SystemAudioDoesNotDuckMedia) {
    EXPECT_EQ(mix_level(&media_, nullptr, &system_, nullptr, aa::core::Nanoseconds{0}),
              kMediaWithSystem);
}

TEST_F(AudioPipelineMix, AllFourRolesMixWithDucking) {
    EXPECT_EQ(mix_level(&media_, &guidance_, &system_, &speech_, aa::core::Nanoseconds{0}),
              kAllFourDucked);
}

TEST_F(AudioPipelineMix, DuckingExpiresAfterWindow) {
    EXPECT_EQ(mix_level(&media_, nullptr, nullptr, &speech_, aa::core::Nanoseconds{0}),
              kMediaWithSpeech);
    const aa::core::Nanoseconds within_window{150LL * 1'000 * 1'000};
    EXPECT_EQ(mix_level(&media_, nullptr, nullptr, nullptr, within_window), kMediaDuckedAlone);
    const aa::core::Nanoseconds outside_window{350LL * 1'000 * 1'000};
    EXPECT_EQ(mix_level(&media_, nullptr, nullptr, nullptr, outside_window), kMediaAlone);
}

TEST_F(AudioPipelineMix, MuteSilencesEveryRole) {
    mixer_.set_muted(true);
    EXPECT_TRUE(mixer_.muted());
    EXPECT_EQ(mix_level(&media_, &guidance_, &system_, &speech_, aa::core::Nanoseconds{0}), 0);
    mixer_.set_muted(false);
    const aa::core::Nanoseconds after_window{350LL * 1'000 * 1'000};
    EXPECT_EQ(mix_level(&media_, nullptr, nullptr, nullptr, after_window), kMediaAlone);
}

TEST_F(AudioPipelineMix, EmptyAndMismatchedFramesFailTyped) {
    aa::audio::MixFrame empty;
    empty.samples = 960;
    auto empty_result = mixer_.mix(empty);
    ASSERT_FALSE(empty_result.has_value());
    EXPECT_EQ(empty_result.error().code(), aa::ErrorCode::invalid_argument);

    aa::audio::MixFrame zero;
    zero.samples = 0;
    zero.roles = {&media_, nullptr, nullptr, nullptr};
    auto zero_result = mixer_.mix(zero);
    ASSERT_FALSE(zero_result.has_value());
    EXPECT_EQ(zero_result.error().code(), aa::ErrorCode::invalid_argument);

    aa::audio::PcmChunk wrong_size{aa::audio::Role::media, stereo_48k(), aa::core::Nanoseconds{0},
                                   std::span<const std::byte>{media_pcm_}.first(64)};
    aa::audio::MixFrame mismatched;
    mismatched.timestamp = aa::core::Nanoseconds{0};
    mismatched.samples = 960;
    mismatched.roles = {&wrong_size, &guidance_, nullptr, nullptr};
    auto mismatched_result = mixer_.mix(mismatched);
    ASSERT_FALSE(mismatched_result.has_value());
    EXPECT_EQ(mismatched_result.error().code(), aa::ErrorCode::audio_format_unsupported);
}

namespace {

[[nodiscard]] aa::audio::RouterConfig pcm_router_config() {
    aa::audio::RouterConfig config;
    // Uses pulsesink: the only supported production real-time route (pipewiresink is refused, #80).
    config.backend = aa::audio::SinkBackend::pulse;
    config.output_format = stereo_48k();
    for (auto& role : config.roles) {
        role.codec = aa::audio::Codec::pcm;
        role.input_format = stereo_48k();
    }
    return config;
}

struct RecordingSinkState;

class AudioPipelineRoute : public ::testing::Test {
protected:
    const aa::protocol::ChannelRole media{aa::protocol::ChannelRole::media_audio};
    const aa::protocol::ChannelRole guidance{aa::protocol::ChannelRole::guidance_audio};
    const aa::core::Nanoseconds base{1'000'000'000};
    std::shared_ptr<RecordingSinkState> rec;
    std::optional<aa::audio::AudioRouter> router;
    std::vector<aa::audio::WriteCompletion> completions;

    void open_incremental();
    void enqueue(aa::protocol::ChannelRole role, std::uint64_t sequence,
                 Tone tone, std::int64_t offset_ns = 0);
    aa::audio::RenderStep step();
    void expect_headers(const std::vector<aa::protocol::MessageHeader>& actual,
                        const std::vector<std::pair<aa::protocol::ChannelRole, std::uint64_t>>& expected);

    [[nodiscard]] aa::core::Result<aa::audio::AudioRouter> open_router() {
        return aa::audio::AudioRouter::open(pcm_router_config());
    }

    void deliver(aa::audio::AudioRouter& router,
                 const std::vector<std::vector<std::byte>>& payloads,
                 aa::core::Nanoseconds timestamp) {
        const std::array<aa::protocol::ChannelRole, 4> channels{
            aa::protocol::ChannelRole::media_audio, aa::protocol::ChannelRole::guidance_audio,
            aa::protocol::ChannelRole::system_audio, aa::protocol::ChannelRole::speech_audio};
        std::uint64_t sequence = 0;
        for (std::size_t i = 0; i < payloads.size(); ++i) {
            const auto message = make_message(channels[i], sequence++, timestamp, payloads[i]);
            auto submitted = router.submit(message);
            ASSERT_TRUE(submitted.has_value());
        }
        auto pumped = router.pump();
        ASSERT_TRUE(pumped.has_value());
    }

    [[nodiscard]] std::vector<std::vector<std::byte>> four_channel_payloads() {
        return {read_file(fixture_path("media_48k_s16le_stereo.pcm")),
                read_file(fixture_path("guidance_48k_s16le_stereo.pcm")),
                read_file(fixture_path("system_48k_s16le_stereo.pcm")),
                read_file(fixture_path("speech_48k_s16le_stereo.pcm"))};
    }
};

} // namespace

TEST_F(AudioPipelineRoute, HappyFourChannelPlaybackDeliversMixedSamplesToSinkElement) {
    auto router = open_router();
    ASSERT_TRUE(router.has_value());
    ASSERT_EQ(router.value().state(), aa::audio::RouterState::active);

    const std::vector<std::vector<std::byte>> payloads = four_channel_payloads();
    for (const auto& payload : payloads) {
        ASSERT_FALSE(payload.empty());
    }

    deliver(router.value(), payloads, now_ns());

    const auto& probe = router.value().report().sink_probe;
    EXPECT_GE(probe.buffers, 1U);
    EXPECT_EQ(probe.samples, 960U * 2U);
    EXPECT_TRUE(probe.non_silent());
    EXPECT_EQ(probe.min_sample, kAllFourDucked);
    EXPECT_EQ(probe.max_sample, kAllFourDucked);
}

TEST_F(AudioPipelineRoute, PerChannelSemanticsReachSinkElement) {
    struct Scenario final {
        std::array<bool, 4> present;
        std::int16_t expected;
    };
    const std::vector<Scenario> scenarios{
        {{true, false, false, false}, kMediaAlone},
        {{false, true, false, false}, kGuidanceAlone},
        {{false, false, true, false}, kSystemAlone},
        {{false, false, false, true}, kSpeechAlone},
        {{true, false, true, false}, kMediaWithSystem},
        {{true, true, false, false}, kMediaWithGuidance},
        {{true, false, false, true}, kMediaWithSpeech},
    };
    const std::array<std::string, 4> names{"media_48k_s16le_stereo.pcm", "guidance_48k_s16le_stereo.pcm",
                                           "system_48k_s16le_stereo.pcm", "speech_48k_s16le_stereo.pcm"};

    for (const auto& scenario : scenarios) {
        auto router = open_router();
        ASSERT_TRUE(router.has_value());
        std::vector<std::vector<std::byte>> payloads;
        std::vector<std::size_t> active;
        for (std::size_t i = 0; i < 4; ++i) {
            if (scenario.present[i]) {
                payloads.push_back(read_file(fixture_path(names[i])));
                active.push_back(i);
            }
        }
        std::uint64_t sequence = 0;
        const std::array<aa::protocol::ChannelRole, 4> channels{
            aa::protocol::ChannelRole::media_audio, aa::protocol::ChannelRole::guidance_audio,
            aa::protocol::ChannelRole::system_audio, aa::protocol::ChannelRole::speech_audio};
        const aa::core::Nanoseconds timestamp = now_ns();
        for (std::size_t i = 0; i < active.size(); ++i) {
            const auto message =
                make_message(channels[active[i]], sequence++, timestamp, payloads[i]);
            ASSERT_TRUE(router.value().submit(message).has_value());
        }
        ASSERT_TRUE(router.value().pump().has_value());

        const auto& probe = router.value().report().sink_probe;
        EXPECT_EQ(probe.min_sample, scenario.expected);
        EXPECT_EQ(probe.max_sample, scenario.expected);
        EXPECT_EQ(probe.non_silent(), scenario.expected != 0);
    }
}

TEST_F(AudioPipelineRoute, MuteDeliversSilenceNotAbsence) {
    auto router = open_router();
    ASSERT_TRUE(router.has_value());
    router.value().set_muted(true);

    const std::vector<std::vector<std::byte>> payloads = four_channel_payloads();
    deliver(router.value(), payloads, now_ns());

    const auto& probe = router.value().report().sink_probe;
    EXPECT_GE(probe.buffers, 1U);
    EXPECT_EQ(probe.min_sample, 0);
    EXPECT_EQ(probe.max_sample, 0);
    EXPECT_FALSE(probe.non_silent());
}

TEST_F(AudioPipelineRoute, LatencyAndCompletionSkewRecordedWithinBudgets) {
    auto router = open_router();
    ASSERT_TRUE(router.has_value());

    const std::vector<std::vector<std::byte>> payloads = four_channel_payloads();

    deliver(router.value(), payloads, now_ns());
    const auto warmup_path_latency_ns = router.value().report().stats.max_path_latency.count;
    const auto warmup_completion_skew_ns = router.value().report().stats.max_completion_skew.count;

    deliver(router.value(), payloads, now_ns());
    const auto& stats = router.value().report().stats;
    EXPECT_EQ(stats.chunks_submitted, 8U);
    EXPECT_EQ(stats.frames_written, 2U);

    RecordProperty("observed_warmup_path_latency_ns", static_cast<int>(warmup_path_latency_ns));
    RecordProperty("observed_warmup_completion_skew_ns", static_cast<int>(warmup_completion_skew_ns));
    RecordProperty("observed_steady_path_latency_ns",
                   static_cast<int>(stats.last_path_latency.count));
    RecordProperty("observed_steady_completion_skew_ns",
                   static_cast<int>(stats.last_completion_skew.count));
    RecordProperty("measurement_method",
                   "steady_clock ns at submit() BEFORE decoding (gate fix F4: decoder waits are "
                   "inside the path) vs DATA-HANDOFF-CONFIRMED WRITE COMPLETION (gate r3 F3: the "
                   "clock is sampled when write() returns - after the write's nonempty buffer "
                   "was bound to its own render window and its PipeWire/Pulse handoff event was "
                   "credited, plus zero-length-marker settlement; the handoff event itself is "
                   "not separately timestamped). Chunk timestamps stamped from the same "
                   "steady_clock (common monotonic A/V clock) at receiver input; warmup frame "
                   "(stream startup) recorded separately from the steady-state frame the budgets "
                   "are asserted on. The device/sink-write endpoint is NOT measured: sink-write "
                   "latency verdicts and delivery claims are withdrawn; these budgets bind to the "
                   "input-to-data-handoff-confirmed-write-completion leg only");

    EXPECT_LE(stats.last_path_latency.count, 200LL * 1000 * 1000);
    EXPECT_LE(stats.last_completion_skew.count, 50LL * 1000 * 1000);
}

TEST_F(AudioPipelineRoute, StaggeredStartChannelsMixOnSampleTimeline) {
    auto router = open_router();
    ASSERT_TRUE(router.has_value());

    const std::vector<std::byte> media = constant_pcm({kMediaLevel, 960});
    const std::vector<std::byte> guidance = constant_pcm({kGuidanceLevel, 960});
    const aa::core::Nanoseconds base = now_ns();
    const aa::core::Nanoseconds guidance_start{base.count + 10LL * 1000 * 1000};

    const auto media_message =
        make_message(aa::protocol::ChannelRole::media_audio, 0, base, media);
    const auto guidance_message =
        make_message(aa::protocol::ChannelRole::guidance_audio, 1, guidance_start, guidance);
    ASSERT_TRUE(router.value().submit(media_message).has_value());
    ASSERT_TRUE(router.value().submit(guidance_message).has_value());
    ASSERT_TRUE(router.value().pump().has_value());

    const auto& report = router.value().report();
    // 30 ms of timeline (media 20 ms + guidance staggered 10..30 ms), split at
    // every run boundary: [0,10 ms) media alone, [10,20 ms) ducked overlap,
    // [20,30 ms) guidance alone. The pre-fix mixer emitted two 20 ms blocks
    // (40 ms total, no ducked overlap) instead.
    EXPECT_EQ(report.stats.frames_written, 3U);
    EXPECT_EQ(report.sink_probe.samples, 1440U * 2U);
    EXPECT_EQ(report.sink_probe.min_sample, kMediaAlone);
    EXPECT_EQ(report.sink_probe.max_sample, kMediaWithGuidance);
    EXPECT_EQ(report.sink_probe.buffers, 3U);
}

TEST_F(AudioPipelineRoute, UnequalDurationRunsSplitAtBoundaries) {
    auto router = open_router();
    ASSERT_TRUE(router.has_value());

    const std::vector<std::byte> media = constant_pcm({kMediaLevel, 960});
    const std::vector<std::byte> guidance = constant_pcm({kGuidanceLevel, 240});
    const aa::core::Nanoseconds base = now_ns();
    const aa::core::Nanoseconds guidance_start{base.count + 10LL * 1000 * 1000};

    const auto media_message =
        make_message(aa::protocol::ChannelRole::media_audio, 0, base, media);
    const auto guidance_message =
        make_message(aa::protocol::ChannelRole::guidance_audio, 1, guidance_start, guidance);
    ASSERT_TRUE(router.value().submit(media_message).has_value());
    ASSERT_TRUE(router.value().submit(guidance_message).has_value());
    ASSERT_TRUE(router.value().pump().has_value());

    const auto& report = router.value().report();
    // media [0,20 ms) + guidance [10,15 ms): [0,10 ms) media alone, [10,15 ms)
    // ducked overlap, [15,20 ms) media alone still inside the ducking window
    // (documented policy). Total 20 ms split at every start/end boundary.
    EXPECT_EQ(report.stats.frames_written, 3U);
    EXPECT_EQ(report.sink_probe.samples, 960U * 2U);
    EXPECT_EQ(report.sink_probe.min_sample, kMediaDuckedAlone);
    EXPECT_EQ(report.sink_probe.max_sample, kMediaWithGuidance);
    EXPECT_EQ(report.sink_probe.buffers, 3U);
}

TEST_F(AudioPipelineRoute, NonOverlappingRunsEmitSilenceGapAndLaterAudio) {
    auto router = open_router();
    ASSERT_TRUE(router.has_value());

    const std::vector<std::byte> media = constant_pcm({kMediaLevel, 960});
    const std::vector<std::byte> guidance = constant_pcm({kGuidanceLevel, 960});
    const aa::core::Nanoseconds base = now_ns();
    const aa::core::Nanoseconds guidance_start{base.count + 30LL * 1000 * 1000};

    const auto media_message =
        make_message(aa::protocol::ChannelRole::media_audio, 0, base, media);
    const auto guidance_message =
        make_message(aa::protocol::ChannelRole::guidance_audio, 1, guidance_start, guidance);
    ASSERT_TRUE(router.value().submit(media_message).has_value());
    ASSERT_TRUE(router.value().submit(guidance_message).has_value());
    ASSERT_TRUE(router.value().pump().has_value());

    const auto& report = router.value().report();
    // media [0,20 ms) + gap [20,30 ms) + guidance [30,50 ms): the empty
    // interval is emitted as correctly sized silence (50 ms duration
    // preserved) instead of aborting the timeline, and the later guidance
    // samples still reach the sink.
    EXPECT_EQ(report.stats.frames_written, 3U);
    EXPECT_EQ(report.sink_probe.samples, 2400U * 2U);
    EXPECT_EQ(report.sink_probe.min_sample, 0);
    EXPECT_EQ(report.sink_probe.max_sample, kGuidanceAlone);
    EXPECT_EQ(report.sink_probe.buffers, 3U);
}

TEST_F(AudioPipelineRoute, RenderBarrierDistinguishesIngressFromRenderCompletion) {
    auto router = open_router();
    ASSERT_TRUE(router.has_value());
    deliver(router.value(), four_channel_payloads(), now_ns());

    const auto& report = router.value().report();
    // Three distinct observation levels: one write puts two buffers on the
    // sink pad (data + zero-length marker) but ingress counts only the data
    // buffer; chain-return counts the marker; and HANDOFF counts the
    // renderer's actual PipeWire/Pulse push event - the only level that
    // acknowledges the write. A conflated implementation (markers counted as
    // ingress, or an ack at chain-return) breaks these counts.
    EXPECT_EQ(report.sink_probe.buffers, 1U);
    EXPECT_EQ(report.sink_probe.samples, 960U * 2U);
    EXPECT_EQ(report.render_barrier.chain_return_buffers, 1U);
    EXPECT_EQ(report.render_barrier.chain_return_samples, 960U * 2U);
    EXPECT_GE(report.render_barrier.handoff_events, 1U);
    // The end-of-stream round-trip is a fourth observation: no drain has run
    // yet, and it is a render-path bookkeeping barrier only - it carries no
    // delivery or device-consumption guarantee.
    EXPECT_EQ(report.render_barrier.drains_completed, 0U);

    router.value().close();
    EXPECT_GE(report.render_barrier.drains_completed, 1U);
}

namespace aa::audio::detail {
[[nodiscard]] SinkOpenOutcome open_native_sink_for_test(const SinkOpenRequest& request);
[[nodiscard]] core::Result<AudioRouter> open_router_with_native_sink_for_test(
    const RouterConfig& config);
[[nodiscard]] bool start_foreign_sink_traffic(std::uint32_t buffers);
void stop_foreign_sink_traffic() noexcept;
[[nodiscard]] std::uint64_t handoff_events_seen() noexcept;
} // namespace aa::audio::detail

TEST_F(AudioPipelineRoute, ExplicitPulseBackendDeliversWithHandoff) {
    // Gate r4 F1 regression: a healthy pulsesink route must NOT be rejected by
    // the handoff attribution (its render commit emits the element object).
    auto config = pcm_router_config();
    config.backend = aa::audio::SinkBackend::pulse;
    auto router = aa::audio::AudioRouter::open(config);
    ASSERT_TRUE(router.has_value());
    ASSERT_EQ(router.value().state(), aa::audio::RouterState::active);

    deliver(router.value(), four_channel_payloads(), now_ns());
    const auto& report = router.value().report();
    EXPECT_EQ(report.sink_probe.samples, 960U * 2U);
    EXPECT_TRUE(report.sink_probe.non_silent());
    EXPECT_EQ(report.sink_probe.min_sample, kAllFourDucked);
    EXPECT_EQ(report.sink_probe.max_sample, kAllFourDucked);
    EXPECT_GE(report.render_barrier.handoff_events, 1U);
}

TEST_F(AudioPipelineRoute, ForeignSinkTrafficNeverAcknowledgesTrackedRoute) {
    // Gate r3 F1 counterexample, deterministic: an UNRELATED in-process
    // audiotestsrc ! pipewiresink pipeline emits real handoff push events on
    // its own streaming thread while the tracked write's buffer is dropped
    // after its render window opened. Those foreign pushes must never
    // acknowledge the tracked route - the write fails typed and degrades.
    auto config = pcm_router_config();
    config.drop_nonempty_after_window = true;
    config.sink_write_timeout = aa::core::Milliseconds{1500};
    auto router = aa::audio::AudioRouter::open(config);
    ASSERT_TRUE(router.has_value());
    ASSERT_TRUE(aa::audio::detail::start_foreign_sink_traffic(600));

    const auto baseline = aa::audio::detail::handoff_events_seen();
    bool foreign_seen = false;
    for (int i = 0; i < 100 && !foreign_seen; ++i) {
        foreign_seen = aa::audio::detail::handoff_events_seen() > baseline;
        if (!foreign_seen) {
            usleep(30000);
        }
    }
    ASSERT_TRUE(foreign_seen);

    const std::vector<std::byte> media = constant_pcm({kMediaLevel, 960});
    const auto message =
        make_message(aa::protocol::ChannelRole::media_audio, 0, now_ns(), media);
    ASSERT_TRUE(router.value().submit(message).has_value());
    auto pumped = router.value().pump();
    ASSERT_FALSE(pumped.has_value());
    EXPECT_EQ(pumped.error().code(), aa::ErrorCode::audio_sink_unavailable);

    EXPECT_EQ(router.value().state(), aa::audio::RouterState::degraded);
    const auto& report = router.value().report();
    EXPECT_EQ(report.render_barrier.handoff_events, 0U);
    EXPECT_GT(report.render_barrier.handoff_events_seen, baseline);
    aa::audio::detail::stop_foreign_sink_traffic();
    router.value().close();
}

TEST_F(AudioPipelineRoute, DataDroppedAtIngressNeverAcknowledges) {
    // Gate r2 F1 regression (the GDB fault-injection scenario, deterministic):
    // the write's nonempty buffer is dropped at the routing element's ingress
    // while the chain-return marker proceeds. The handoff barrier must NOT
    // acknowledge: no render push exists for the dropped data, so the write
    // fails typed and the router degrades with a recorded cause.
    auto config = pcm_router_config();
    config.drop_nonempty_at_ingress = true;
    config.sink_write_timeout = aa::core::Milliseconds{500};
    auto router = aa::audio::AudioRouter::open(config);
    ASSERT_TRUE(router.has_value());

    const std::vector<std::byte> media = constant_pcm({kMediaLevel, 960});
    const auto message =
        make_message(aa::protocol::ChannelRole::media_audio, 0, now_ns(), media);
    ASSERT_TRUE(router.value().submit(message).has_value());
    auto pumped = router.value().pump();
    ASSERT_FALSE(pumped.has_value());
    EXPECT_EQ(pumped.error().code(), aa::ErrorCode::audio_sink_unavailable);

    const auto& report = router.value().report();
    EXPECT_EQ(router.value().state(), aa::audio::RouterState::degraded);
    EXPECT_NE(report.sink.cause.find("disabled"), std::string::npos);
    EXPECT_EQ(report.render_barrier.handoff_events, 0U);
    EXPECT_GT(report.sink_probe.samples, 0U);
    router.value().close();
}

TEST_F(AudioPipelineRoute, DroppedDataUsesOneWriteDeadline) {
    // Gate fix 3: the ingress-drop scenario with its write deadline pinned.
    // The write spends ONE bounded budget (500 ms admission + chain + handoff)
    // and the pump returns with the typed failure before a second timeout
    // window could stack on the first (which would measure >= 1 s).
    auto config = pcm_router_config();
    config.drop_nonempty_at_ingress = true;
    config.sink_write_timeout = aa::core::Milliseconds{500};
    auto router = aa::audio::AudioRouter::open(config);
    ASSERT_TRUE(router.has_value());

    const std::vector<std::byte> media = constant_pcm({kMediaLevel, 960});
    const auto message =
        make_message(aa::protocol::ChannelRole::media_audio, 0, now_ns(), media);
    ASSERT_TRUE(router.value().submit(message).has_value());
    const auto started = std::chrono::steady_clock::now();
    auto pumped = router.value().pump();
    const auto elapsed = std::chrono::steady_clock::now() - started;
    ASSERT_FALSE(pumped.has_value());
    EXPECT_EQ(pumped.error().code(), aa::ErrorCode::audio_sink_unavailable);
    EXPECT_LT(elapsed, std::chrono::milliseconds{800});

    const auto& report = router.value().report();
    EXPECT_EQ(router.value().state(), aa::audio::RouterState::degraded);
    EXPECT_NE(report.sink.cause.find("disabled"), std::string::npos);
    EXPECT_EQ(report.render_barrier.handoff_events, 0U);
    EXPECT_GT(report.sink_probe.samples, 0U);
    router.value().close();
}

TEST_F(AudioPipelineRoute, BackpressureTimeoutDegradesAndRefusesFurtherSubmit) {
    // Gate fix 3: the full-appsrc-queue condition seen through the router. The
    // downstream block stalls the route after readiness and the queue is
    // prefilled to its byte threshold, so the next mixed write cannot be
    // admitted; its ONE deadline expires at admission, the pump fails typed,
    // the router degrades with the cause recording the timeout, and every
    // later submit is refused.
    aa::audio::detail::SinkOpenRequest request;
    // Uses pulsesink: the only supported production real-time route (#80).
    request.backend = aa::audio::SinkBackend::pulse;
    request.format = stereo_48k();
    request.start_timeout = aa::core::Milliseconds{2000};
    request.write_timeout = aa::core::Milliseconds{200};
    auto outcome = aa::audio::detail::open_default_sink(request);
    ASSERT_TRUE(outcome.sink != nullptr) << outcome.report.cause;

    // Releases the downstream block on EVERY exit path (including assertion
    // failures) and only then lets the router release the sink: the harness
    // is declared after the sink, so it runs first at scope exit.
    struct Harness final {
        aa::audio::detail::ObservedAudioSink* sink{nullptr};
        std::optional<aa::audio::AudioRouter> router{};
        ~Harness() {
            if (sink != nullptr) {
                aa::audio::detail::unblock_downstream_for_test(*sink);
            }
            router.reset();
        }
    };
    Harness harness{};
    harness.sink = outcome.sink.get();

    ASSERT_TRUE(aa::audio::detail::block_downstream_for_test(*harness.sink));
    (void)aa::audio::detail::prefill_appsrc_for_test(*harness.sink, 4);
    ASSERT_TRUE(aa::audio::detail::wait_downstream_block_entered_for_test(
        *harness.sink, aa::core::Milliseconds{2000}));
    const std::uint64_t filled = aa::audio::detail::prefill_appsrc_for_test(*harness.sink, 65536);
    EXPECT_GE(filled, 65536U);

    auto opened = aa::audio::detail::open_router_with_sink_for_test(pcm_router_config(),
                                                                    std::move(outcome.sink));
    ASSERT_TRUE(opened.has_value());
    harness.router.emplace(std::move(opened).value());

    const std::vector<std::byte> media = constant_pcm({kMediaLevel, 960});
    const auto message =
        make_message(aa::protocol::ChannelRole::media_audio, 0, now_ns(), media);
    ASSERT_TRUE(harness.router->submit(message).has_value());
    auto pumped = harness.router->pump();
    ASSERT_FALSE(pumped.has_value());
    EXPECT_EQ(pumped.error().code(), aa::ErrorCode::audio_sink_unavailable);
    EXPECT_EQ(harness.router->state(), aa::audio::RouterState::degraded);
    EXPECT_NE(harness.router->report().sink.cause.find("timeout"), std::string::npos);

    const auto late = make_message(aa::protocol::ChannelRole::media_audio, 1, now_ns(), media);
    auto refused = harness.router->submit(late);
    ASSERT_FALSE(refused.has_value());
    EXPECT_EQ(refused.error().code(), aa::ErrorCode::audio_sink_unavailable);
    // The harness releases the downstream block before teardown here.
}

// A shutdown's deadline cuts a write below its own timeout (#228).
TEST_F(AudioPipelineRoute, WriteDeadlineCutsABlockedWriteBelowItsTimeout) {
    aa::audio::detail::SinkOpenRequest request;
    request.backend = aa::audio::SinkBackend::pulse;
    request.format = stereo_48k();
    auto outcome = aa::audio::detail::open_default_sink(request);
    ASSERT_TRUE(outcome.sink != nullptr) << outcome.report.cause;
    struct Release final {
        aa::audio::detail::ObservedAudioSink& sink;
        ~Release() { aa::audio::detail::unblock_downstream_for_test(sink); }
    } release{*outcome.sink};
    ASSERT_TRUE(aa::audio::detail::block_downstream_for_test(*outcome.sink));
    const auto media = constant_pcm({kMediaLevel, 960});
    aa::audio::MixedChunk chunk;
    chunk.format = stereo_48k();
    chunk.timestamp = now_ns();
    chunk.pcm = media;
    const auto started = std::chrono::steady_clock::now();
    outcome.sink->set_write_deadline(started + std::chrono::milliseconds{200});
    const auto written = outcome.sink->write(chunk);
    const auto took = std::chrono::steady_clock::now() - started;
    ASSERT_FALSE(written);
    EXPECT_EQ(written.error().code(), aa::ErrorCode::timeout);
    EXPECT_GE(took, std::chrono::milliseconds{190});
    EXPECT_LT(took, std::chrono::milliseconds{1'000});
}

TEST_F(AudioPipelineRoute, MissingDefaultOutputDegradesTypedWithRecordedCause) {
    auto config = pcm_router_config();
    config.backend = aa::audio::SinkBackend::pipewire;
    config.sink_remote = "aa-task25-no-such-output";
    auto router = aa::audio::detail::open_router_with_native_sink_for_test(config);
    ASSERT_TRUE(router.has_value());

    EXPECT_EQ(router.value().state(), aa::audio::RouterState::degraded);
    const auto& cause = router.value().report().sink.cause;
    EXPECT_NE(cause.find("disabled"), std::string::npos);
    EXPECT_NE(cause.find("aa-task25-no-such-output"), std::string::npos);

    const std::vector<std::byte> payload = constant_pcm({kMediaLevel, 960});
    const auto message =
        make_message(aa::protocol::ChannelRole::media_audio, 0, now_ns(), payload);
    auto submitted = router.value().submit(message);
    ASSERT_FALSE(submitted.has_value());
    EXPECT_EQ(submitted.error().code(), aa::ErrorCode::audio_sink_unavailable);

    auto pumped = router.value().pump();
    ASSERT_FALSE(pumped.has_value());
    EXPECT_EQ(pumped.error().code(), aa::ErrorCode::audio_sink_unavailable);

    EXPECT_EQ(router.value().report().stats.frames_written, 0U);
}

TEST_F(AudioPipelineRoute, PathLatencyIncludesDecodeTime) {
    auto config = pcm_router_config();
    config.roles[3].codec = aa::audio::Codec::aac_lc_adts;
    const auto decode_report = aa::audio::AudioDecoder::probe(aa::audio::Codec::aac_lc_adts);
    if (!decode_report.usable) {
        GTEST_SKIP() << "no viable AAC decoder: " << decode_report.cause;
    }
    auto router = aa::audio::AudioRouter::open(config);
    ASSERT_TRUE(router.has_value());

    const std::vector<std::byte> stream =
        read_file(fixture_path("speech_aac_lc_adts_48k_stereo.aac"));
    ASSERT_FALSE(stream.empty());
    const auto message =
        make_message(aa::protocol::ChannelRole::speech_audio, 0, now_ns(), stream);
    ASSERT_TRUE(router.value().submit(message).has_value());
    ASSERT_TRUE(router.value().pump().has_value());

    const auto latency = router.value().report().stats.last_path_latency.count;
    RecordProperty("observed_decode_inclusive_path_latency_ns", static_cast<int>(latency));
    RecordProperty("measurement_method",
                   "submit() stamps steady_clock BEFORE decode(); the AAC decode path always "
                   "waits the 25 ms parser drain-quiet window, so a decode-excluding measurement "
                   "would stay far below 20 ms while a decode-inclusive one cannot");
    EXPECT_GE(latency, 20LL * 1000 * 1000);
}

namespace {

// Isolated PipeWire backend (gate r2 F3 / r3 F2): private daemon + session
// manager on a scratch runtime, spawned with a RESTRICTED environment built
// BEFORE any spawn (scratch HOME/XDG dirs, private scratch-local D-Bus session
// bus with no service activation, dead system-bus address) and RESTRICTED
// configurations for BOTH variants (no Bluetooth, no ALSA/v4l2/libcamera
// discovery, no portal/rtkit/jackdbus/x11 modules, no state outside scratch),
// so no system-bus, BlueZ or real-device interaction can occur. start()
// distinguishes hard configuration errors (the test must FAIL) from missing
// daemon binaries (the test SKIPs).
class IsolatedPipeWire final {
public:
    struct StartOutcome;

    IsolatedPipeWire() = default;
    IsolatedPipeWire(const IsolatedPipeWire&) = delete;
    IsolatedPipeWire& operator=(const IsolatedPipeWire&) = delete;
    IsolatedPipeWire(IsolatedPipeWire&& other) noexcept
        : scratch_(std::move(other.scratch_)), runtime_dir_(std::move(other.runtime_dir_)),
          env_(std::move(other.env_)), pids_(std::move(other.pids_)),
          had_parent_runtime_dir_(other.had_parent_runtime_dir_),
          parent_runtime_dir_(std::move(other.parent_runtime_dir_)) {
        other.runtime_dir_.clear();
        other.pids_.clear();
        other.had_parent_runtime_dir_ = false;
    }
    ~IsolatedPipeWire() {
        kill_backend();
        if (!runtime_dir_.empty()) {
            if (had_parent_runtime_dir_) {
                setenv("PIPEWIRE_RUNTIME_DIR", parent_runtime_dir_.c_str(), 1);
            } else {
                unsetenv("PIPEWIRE_RUNTIME_DIR");
            }
        }
    }

    [[nodiscard]] static StartOutcome start(bool with_null_sink);

    void kill_backend() {
        for (pid_t& pid : pids_) {
            if (pid > 0) {
                kill(pid, SIGKILL);
                waitpid(pid, nullptr, 0);
                pid = 0;
            }
        }
    }

private:
    enum class SpawnResult { ok, unavailable, error };

    [[nodiscard]] static bool make_directory(const std::string& path) {
        if (mkdir(path.c_str(), 0700) == 0) {
            return true;
        }
        return errno == EEXIST;
    }

    [[nodiscard]] SpawnResult spawn(const std::vector<std::string>& args) {
        std::vector<char*> argv;
        argv.reserve(args.size() + 1);
        for (const auto& arg : args) {
            argv.push_back(const_cast<char*>(arg.c_str()));
        }
        argv.push_back(nullptr);
        std::vector<char*> envp;
        envp.reserve(env_.size() + 1);
        for (auto& entry : env_) {
            envp.push_back(entry.data());
        }
        envp.push_back(nullptr);
        pid_t pid = 0;
        const int rc = posix_spawnp(&pid, args.front().c_str(), nullptr, nullptr, argv.data(),
                                    envp.data());
        if (rc == ENOENT) {
            return SpawnResult::unavailable;
        }
        if (rc != 0) {
            return SpawnResult::error;
        }
        pids_.push_back(pid);
        return SpawnResult::ok;
    }

public:
    struct ConfigOutcome {
        enum class Stage { ok, template_read, denied_module_verify, destination_open, write_close };
        Stage stage{Stage::ok};
        std::string detail{};
    };

    // Restricted daemon context derived from the installed default (known-good
    // core setup) with every hardware/D-Bus-dependent module stanza removed by
    // EXACT module name (no prefix collisions) and the bell/jackdbus/access
    // autoloads disabled. A null Audio/Sink is added only when requested.
    // Returns a stage-specific outcome so fixture failures say which stage
    // broke and with which paths, instead of one boolean.
    [[nodiscard]] static ConfigOutcome write_restricted_daemon_config(
        const std::string& template_path, const std::string& dest_path, bool with_null_sink) {
        std::ifstream input(template_path);
        if (!input) {
            return {ConfigOutcome::Stage::template_read, "cannot open " + template_path};
        }
        std::string body{std::istreambuf_iterator<char>{input},
                         std::istreambuf_iterator<char>{}};
        const std::array<const char*, 12> deny{"libpipewire-module-rt",
                                               "libpipewire-module-portal",
                                               "libpipewire-module-jackdbus-detect",
                                               "libpipewire-module-x11-bell",
                                               "libpipewire-module-alsa-monitor",
                                               "libpipewire-module-bluez5-dbus",
                                               "libpipewire-module-bluez5-monitor",
                                               "libpipewire-module-v4l2-monitor",
                                               "libpipewire-module-libcamera",
                                               "libpipewire-module-avb",
                                               "libpipewire-module-jack-tunnel",
                                               "libpipewire-module-netjack2-manager"};

        // Exact module-token match shared by the stripper and the fail-closed
        // verify: "libpipewire-module-rtkit" is NOT "libpipewire-module-rt"
        // (the verify's old prefix compare conflated them).
        const auto token_after = [](const std::string& line, std::size_t start) {
            const std::size_t begin = line.find_first_not_of(" \t\"", start);
            if (begin == std::string::npos) {
                return std::string{};
            }
            const std::size_t stop = line.find_first_not_of(
                "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-", begin);
            return line.substr(begin,
                               stop == std::string::npos ? std::string::npos : stop - begin);
        };
        const auto denied_token = [&](const std::string& token) {
            return std::any_of(std::begin(deny), std::end(deny),
                               [&](const char* bad) { return token == bad; });
        };

        std::istringstream stream{body};
        std::string line;
        std::string kept;
        int skip_depth = 0;
        bool skip = false;
        while (std::getline(stream, line)) {
            if (skip) {
                skip_depth += static_cast<int>(std::count(line.begin(), line.end(), '{'))
                    - static_cast<int>(std::count(line.begin(), line.end(), '}'));
                if (skip_depth <= 0) {
                    skip = false;
                }
                continue;
            }
            const std::size_t name = line.find("name = libpipewire-module-");
            if (name != std::string::npos) {
                const std::string module = token_after(line, name + 7);
                if (denied_token(module)) {
                    skip_depth = static_cast<int>(std::count(line.begin(), line.end(), '{'))
                        - static_cast<int>(std::count(line.begin(), line.end(), '}'));
                    skip = true;
                    continue;
                }
            }
            if (line.find("module.x11.bell") != std::string::npos
                && line.find('=') != std::string::npos && line.find('#') == std::string::npos) {
                line = "module.x11.bell = false";
            }
            if (line.find("module.jackdbus-detect") != std::string::npos
                && line.find('=') != std::string::npos && line.find('#') == std::string::npos) {
                line = "module.jackdbus-detect = false";
            }
            kept += line + "\n";
        }

        // Fail closed (gate r3 F2 "assert in the test config"): no ACTIVE
        // stanza may reference a denied module (commented examples in the
        // upstream config are not loaded and are not asserted against). The
        // verify tokenizes more broadly than the stripper (quotes/spacing
        // tolerated) so a forbidden reference the stripper missed is caught
        // here rather than silently kept.
        {
            std::istringstream verify{kept};
            std::string vline;
            while (std::getline(verify, vline)) {
                const std::size_t hash = vline.find_first_not_of(" \t");
                if (hash != std::string::npos && vline[hash] == '#') {
                    continue;
                }
                const std::size_t ref = vline.find("name = ");
                if (ref == std::string::npos) {
                    continue;
                }
                const std::string token = token_after(vline, ref + 7);
                if (!token.empty() && denied_token(token)) {
                    std::string cause = "retained forbidden module ";
                    cause += token;
                    cause += " in ";
                    cause += dest_path;
                    return {ConfigOutcome::Stage::denied_module_verify, cause};
                }
            }
        }

        if (with_null_sink) {
            const std::string marker = "context.objects = [";
            const std::size_t at = kept.find(marker);
            const std::string stanza =
                "    { factory = adapter\n"
                "        args = {\n"
                "            factory.name     = support.null-audio-sink\n"
                "            node.name        = \"auto_null\"\n"
                "            node.description = \"Dummy Output\"\n"
                "            media.class      = \"Audio/Sink\"\n"
                "            audio.position   = \"FL,FR\"\n"
                "            monitor.channel-volumes = true\n"
                "        }\n"
                "    },\n";
            std::ofstream output(dest_path);
            if (!output) {
                return {ConfigOutcome::Stage::destination_open, "cannot open " + dest_path};
            }
            if (at == std::string::npos) {
                output << kept << "\ncontext.objects = [\n" << stanza << "]\n";
            } else {
                output << kept.substr(0, at + marker.size()) << "\n" << stanza
                       << kept.substr(at + marker.size());
            }
            output.close();
            if (!output) {
                return {ConfigOutcome::Stage::write_close, "write failed " + dest_path};
            }
            return {};
        }
        std::ofstream output(dest_path);
        if (!output) {
            return {ConfigOutcome::Stage::destination_open, "cannot open " + dest_path};
        }
        output << kept;
        output.close();
        if (!output) {
            return {ConfigOutcome::Stage::write_close, "write failed " + dest_path};
        }
        return {};
    }

private:

    // Policy-only session manager: linking without Bluetooth, ALSA/v4l2/
    // libcamera monitors, portal access checks or restore-stream persistence
    // outside the scratch dir. The system fragments that would do hardware
    // discovery or D-Bus access are shadowed by checked no-op stubs.
    [[nodiscard]] static bool write_restricted_wireplumber_config(
        const std::string& scratch_dir, const std::string& fragments) {
        const std::array<std::pair<const char*, const char*>, 4> stubs{{
            {"30-alsa-monitor.lua", "alsa_monitor"},
            {"30-v4l2-monitor.lua", "v4l2_monitor"},
            {"30-libcamera-monitor.lua", "libcamera_monitor"},
            {"20-default-access.lua", "default_access"},
        }};
        for (const auto& [file, global_name] : stubs) {
            std::ofstream stub(fragments + "/" + file);
            if (!stub) {
                return false;
            }
            stub << global_name << " = { enable = function() end }\n";
            if (!stub.good()) {
                return false;
            }
        }
        for (const char* tuned :
             {"50-alsa-config.lua", "50-v4l2-config.lua", "50-libcamera-config.lua",
              "50-default-access-config.lua"}) {
            std::ofstream stub(fragments + "/" + tuned);
            if (!stub) {
                return false;
            }
            stub << "-- disabled for isolation QA\n";
            if (!stub.good()) {
                return false;
            }
        }

        std::ofstream output(scratch_dir + "/wireplumber-minimal.conf");
        if (!output) {
            return false;
        }
        output << "context.properties = {\n"
               << "  log.level = 2\n"
               << "  support.dbus = false\n"
               << "  wireplumber.script-engine = lua-scripting\n"
               << "}\n"
               << "context.spa-libs = {\n"
               << "  audio.convert.* = audioconvert/libspa-audioconvert\n"
               << "  support.*       = support/libspa-support\n"
               << "}\n"
               << "context.modules = [\n"
               << "  { name = libpipewire-module-protocol-native }\n"
               << "  { name = libpipewire-module-client-node }\n"
               << "  { name = libpipewire-module-client-device }\n"
               << "  { name = libpipewire-module-adapter }\n"
               << "  { name = libpipewire-module-metadata }\n"
               << "  { name = libpipewire-module-session-manager }\n"
               << "]\n"
               << "wireplumber.components = [\n"
               << "  { name = libwireplumber-module-lua-scripting, type = module }\n"
               << "  { name = main.lua, type = config/lua }\n"
               << "  { name = policy.lua, type = config/lua }\n"
               << "]\n";
        return output.good();
    }

    std::string scratch_{};
    std::string runtime_dir_{};
    std::vector<std::string> env_{};
    std::vector<pid_t> pids_{};
    bool had_parent_runtime_dir_{false};
    std::string parent_runtime_dir_{};
};

struct IsolatedPipeWire::StartOutcome {
    std::optional<IsolatedPipeWire> backend{};
    std::string error{};
    bool unavailable{false};
    std::string unavailable_what{};
};

IsolatedPipeWire::StartOutcome IsolatedPipeWire::start(bool with_null_sink) {
        StartOutcome outcome;
        // Required executables are checked BEFORE anything spawns or is
        // created: a missing daemon binary must SKIP locally but must FAIL in
        // CI, and it must never leave a spawned bus behind. The search path
        // matches the restricted child PATH below.
        const auto executable_present = [](const char* name) {
            for (const char* dir : {"/usr/local/bin", "/usr/bin", "/bin"}) {
                const std::string candidate = std::string{dir} + "/" + name;
                if (::access(candidate.c_str(), X_OK) == 0) {
                    return true;
                }
            }
            return false;
        };
        std::string missing;
        for (const char* required : {"dbus-daemon", "pipewire"}) {
            if (!executable_present(required)) {
                missing += (missing.empty() ? "" : ", ");
                missing += required;
            }
        }
        if (with_null_sink && !executable_present("wireplumber")) {
            missing += (missing.empty() ? "" : ", ");
            missing += "wireplumber";
        }
        if (!missing.empty()) {
            outcome.unavailable = true;
            outcome.unavailable_what = missing;
            return outcome;
        }
        char templ[] = "/tmp/aa-task25-pw-XXXXXX";
        const char* dir = mkdtemp(templ);
        if (dir == nullptr) {
            outcome.error = "mkdtemp failed for the isolation scratch dir";
            return outcome;
        }
        IsolatedPipeWire self;
        self.scratch_ = dir;
        self.runtime_dir_ = self.scratch_ + "/run";
        const std::array<std::string, 5> dirs{
            self.runtime_dir_, self.scratch_ + "/state", self.scratch_ + "/cache",
            self.scratch_ + "/config", self.scratch_ + "/config/wireplumber"};
        for (const auto& created : dirs) {
            if (!make_directory(created)) {
                outcome.error = "mkdir failed: " + created;
                return outcome;
            }
        }
        const std::string fragments = self.scratch_ + "/config/wireplumber/main.lua.d";
        if (!make_directory(fragments)) {
            outcome.error = "mkdir failed: " + fragments;
            return outcome;
        }

        // Restricted child environment, populated BEFORE any bus or daemon is
        // spawned so nothing can escape to the real user/system session.
        const std::string bus_socket = self.scratch_ + "/state/dbus.sock";
        self.env_.clear();
        self.env_.push_back("PATH=/usr/local/bin:/usr/bin:/bin");
        self.env_.push_back("HOME=" + self.scratch_);
        self.env_.push_back("XDG_STATE_HOME=" + self.scratch_ + "/state");
        self.env_.push_back("XDG_CACHE_HOME=" + self.scratch_ + "/cache");
        self.env_.push_back("XDG_CONFIG_HOME=" + self.scratch_ + "/config");
        self.env_.push_back("XDG_RUNTIME_DIR=" + self.runtime_dir_);
        self.env_.push_back("PIPEWIRE_RUNTIME_DIR=" + self.runtime_dir_);
        self.env_.push_back("DBUS_SESSION_BUS_ADDRESS=unix:path=" + bus_socket);
        self.env_.push_back(
            "DBUS_SYSTEM_BUS_ADDRESS=unix:path=" + self.scratch_ + "/state/no-system-bus.sock");

        // Private session bus WITHOUT service activation: no <servicedir>/
        // <standard_session_servicedirs> entries means nothing (gvfsd, portal
        // helpers) can be activated out of /usr/share/dbus-1/services.
        const std::string bus_config = self.scratch_ + "/state/dbus-session.conf";
        {
            std::ofstream output(bus_config);
            if (!output) {
                outcome.error = "cannot write " + bus_config;
                return outcome;
            }
            output << "<!DOCTYPE busconfig PUBLIC \"-//freedesktop//DTD D-Bus Bus Configuration 1.0//EN\" \"http://www.freedesktop.org/standards/dbus/1.0/busconfig.dtd\">\n"
                   << "<busconfig>\n"
                   << "  <type>session</type>\n"
                   << "  <listen>unix:path=" << bus_socket << "</listen>\n"
                   << "  <auth>EXTERNAL</auth>\n"
                   << "  <policy context=\"default\">\n"
                   << "    <allow user=\"*\"/>\n"
                   << "    <allow own=\"*\"/>\n"
                   << "    <allow send_type=\"method_call\"/>\n"
                   << "    <allow send_type=\"signal\"/>\n"
                   << "    <allow send_type=\"method_return\"/>\n"
                   << "    <allow send_type=\"error\"/>\n"
                   << "    <allow receive_type=\"method_call\"/>\n"
                   << "    <allow receive_type=\"signal\"/>\n"
                   << "    <allow receive_type=\"method_return\"/>\n"
                   << "    <allow receive_type=\"error\"/>\n"
                   << "  </policy>\n"
                   << "</busconfig>\n";
            if (!output.good()) {
                outcome.error = "failed writing " + bus_config;
                return outcome;
            }
        }

        const char* parent_runtime = std::getenv("PIPEWIRE_RUNTIME_DIR");
        self.had_parent_runtime_dir_ = parent_runtime != nullptr;
        if (parent_runtime != nullptr) {
            self.parent_runtime_dir_ = parent_runtime;
        }
        setenv("PIPEWIRE_RUNTIME_DIR", self.runtime_dir_.c_str(), 1);

        const SpawnResult bus_spawn = self.spawn(
            {"dbus-daemon", "--nofork", "--config-file=" + bus_config});
        if (bus_spawn != SpawnResult::ok) {
            if (bus_spawn == SpawnResult::unavailable) {
                outcome.unavailable = true;
            } else {
                outcome.error = "dbus-daemon spawn failed";
            }
            return outcome;
        }
        usleep(200000);

        // Restricted daemon config for BOTH variants (null sink only when the
        // route needs an output): hardware and D-Bus modules stripped.
        const std::string daemon_config = self.scratch_ + "/pipewire-restricted.conf";
        const ConfigOutcome config = write_restricted_daemon_config(
            "/usr/share/pipewire/pipewire.conf", daemon_config, with_null_sink);
        if (config.stage != ConfigOutcome::Stage::ok) {
            outcome.error = "cannot write the restricted daemon config: " + config.detail;
            return outcome;
        }
        const SpawnResult daemon_spawn =
            self.spawn({"pipewire", "-c", daemon_config});
        if (daemon_spawn != SpawnResult::ok) {
            if (daemon_spawn == SpawnResult::unavailable) {
                outcome.unavailable = true;
            } else {
                outcome.error = "pipewire daemon spawn failed";
            }
            return outcome;
        }

        if (with_null_sink) {
            if (!write_restricted_wireplumber_config(self.scratch_, fragments)) {
                outcome.error = "cannot write the restricted session-manager config/stubs";
                return outcome;
            }
            const SpawnResult wp_spawn =
                self.spawn({"wireplumber", "-c", self.scratch_ + "/wireplumber-minimal.conf"});
            if (wp_spawn != SpawnResult::ok) {
                if (wp_spawn == SpawnResult::unavailable) {
                    outcome.unavailable = true;
                } else {
                    outcome.error = "wireplumber spawn failed";
                }
                return outcome;
            }
        }
        usleep(800000);
        outcome.backend.emplace(std::move(self));
        return outcome;
}


} // namespace

namespace {

struct SinkLoggingCase {
    const char* name;
    const char* debug;
    bool sink_debug;
    bool factory_info;
};

class AudioPipelineSinkLogging : public ::testing::TestWithParam<SinkLoggingCase> {};

} // namespace

TEST_P(AudioPipelineSinkLogging, PreservesUserThresholdsWithRealHandoff) {
    const auto& config = GetParam();
    if (std::getenv("AA_AUDIO_LOG_CHILD") != nullptr) {
        auto started = IsolatedPipeWire::start(true);
        ASSERT_TRUE(started.error.empty()) << started.error;
        ASSERT_TRUE(started.backend) << "isolated PipeWire prerequisites missing: "
                                    << started.unavailable_what;
        auto daemon = std::move(*started.backend);
        std::mutex mutex;
        std::optional<aa::audio::SinkWriteProgress> latest;
        aa::audio::detail::SinkOpenRequest request;
        request.format = stereo_48k();
        request.backend = aa::audio::SinkBackend::pipewire;
        request.on_write_progress = [&](const auto& value) {
            std::lock_guard lock(mutex);
            latest = value;
        };
        auto opened = aa::audio::detail::open_native_sink_for_test(request);
        ASSERT_TRUE(opened.sink) << opened.report.cause;
        const auto pcm = constant_pcm({kMediaLevel, 960});
        ASSERT_TRUE(opened.sink->write({stereo_48k(), now_ns(), pcm}));
        {
            std::lock_guard lock(mutex);
            ASSERT_TRUE(latest);
            EXPECT_FALSE(latest->readiness_probe);
            EXPECT_TRUE(latest->handoff_at);
            EXPECT_TRUE(latest->marker_at);
            EXPECT_TRUE(latest->completed_at);
            EXPECT_FALSE(latest->outcome);
        }
        EXPECT_EQ(opened.sink->render_stats().handoff_events, 1U);
        EXPECT_EQ(opened.sink->render_stats().chain_return_buffers, 1U);
        ASSERT_TRUE(opened.sink->drain(aa::core::Milliseconds{2000}));
        return;
    }

    // GST_DEBUG is startup configuration: exec the same case before any
    // GStreamer initialization, with a real timeout and captured child stdout/stderr.
    char path[] = "/tmp/aa-sink-logging-XXXXXX";
    const int fd = mkstemp(path);
    ASSERT_GE(fd, 0);
    struct Capture final {
        int fd;
        const char* path;
        ~Capture() { close(fd); unlink(path); }
    } capture{fd, path};
    std::vector<std::string> env;
    for (char** entry = environ; *entry != nullptr; ++entry) {
        const std::string value{*entry};
        if (value.rfind("GST_DEBUG", 0) != 0 && value.rfind("AA_AUDIO_LOG_CHILD=", 0) != 0) {
            env.push_back(value);
        }
    }
    env.emplace_back("AA_AUDIO_LOG_CHILD=1");
    env.emplace_back("GST_DEBUG_NO_COLOR=1");
    if (config.debug != nullptr) { env.emplace_back(std::string{"GST_DEBUG="} + config.debug); }
    std::vector<char*> envp;
    envp.reserve(env.size() + 1);
    for (auto& value : env) { envp.push_back(value.data()); }
    envp.push_back(nullptr);
    const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
    std::string filter = std::string{"--gtest_filter="} + info->test_suite_name() + "." + info->name();
    char* argv[] = {const_cast<char*>("timeout"), const_cast<char*>("--kill-after=5s"),
                    const_cast<char*>("20s"), const_cast<char*>("/proc/self/exe"),
                    filter.data(), const_cast<char*>("--gtest_output="), nullptr};
    // /proc/self/exe would name timeout after exec; resolve this test first.
    std::array<char, 4096> executable{};
    const auto length = readlink("/proc/self/exe", executable.data(), executable.size() - 1);
    ASSERT_GT(length, 0);
    ASSERT_LT(static_cast<std::size_t>(length), executable.size() - 1);
    argv[3] = executable.data();
    posix_spawn_file_actions_t actions;
    ASSERT_EQ(posix_spawn_file_actions_init(&actions), 0);
    const int stderr_redirected = posix_spawn_file_actions_adddup2(&actions, fd, STDERR_FILENO);
    const int redirected = stderr_redirected == 0
        ? posix_spawn_file_actions_adddup2(&actions, fd, STDOUT_FILENO) : stderr_redirected;
    pid_t pid = 0;
    const int spawned = redirected == 0
        ? posix_spawn(&pid, "/usr/bin/timeout", &actions, nullptr, argv, envp.data()) : redirected;
    posix_spawn_file_actions_destroy(&actions);
    ASSERT_EQ(spawned, 0);
    int status = 0;
    pid_t waited;
    do { waited = waitpid(pid, &status, 0); } while (waited == -1 && errno == EINTR);
    ASSERT_EQ(waited, pid);
    std::ifstream input{path};
    const std::string output{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
    RecordProperty("child_stderr", output);
    ASSERT_TRUE(WIFEXITED(status)) << status << "\n" << output;
    ASSERT_EQ(WEXITSTATUS(status), 0) << output;
    bool sink_debug = false;
    bool factory_info = false;
    std::istringstream lines{output};
    for (std::string line; std::getline(lines, line);) {
        std::istringstream fields{line};
        std::string timestamp, process, thread, level, category;
        if (!(fields >> timestamp >> process >> thread >> level >> category)) { continue; }
        if (category == "pipewiresink"
            && line.find("gst_pipewire_sink_render: push buffer") != std::string::npos) {
            sink_debug = true;
        }
        if (category == "GST_ELEMENT_FACTORY") { factory_info = true; }
        if (!config.sink_debug) {
            EXPECT_FALSE((category == "pipewiresink" || category == "pulse")
                         && (level == "DEBUG" || level == "LOG" || level == "TRACE")) << line;
        }
    }
    EXPECT_EQ(sink_debug, config.sink_debug);
    EXPECT_EQ(factory_info, config.factory_info);
}

INSTANTIATE_TEST_SUITE_P(Startup, AudioPipelineSinkLogging, ::testing::Values(
    SinkLoggingCase{"Unrelated", "GST_ELEMENT_FACTORY:4,GST_STATES:4", false, true},
    SinkLoggingCase{"SinkDebug", "pipewiresink:DEBUG", true, false},
    SinkLoggingCase{"SinkTrace", "pipewiresink:TRACE", true, false},
    SinkLoggingCase{"WildcardOverride", "*:DEBUG,pipewire*:INFO,pulse:INFO", false, true},
    SinkLoggingCase{"Numeric", "5", true, true},
    SinkLoggingCase{"Unset", nullptr, false, false}),
    [](const ::testing::TestParamInfo<SinkLoggingCase>& info) { return info.param.name; });

TEST_F(AudioPipelineRoute, IsolatedBackendWithoutSinksNeverOpensActive) {
    // Gate r2 F3 regression (a): an available PipeWire server with no outputs
    // must NOT pass as active while the host Pulse provider is present - the
    // readiness is the chosen route's render-probe round-trip, not a generic
    // Audio/Sink enumeration that another provider can satisfy.
    auto started = IsolatedPipeWire::start(false);
    ASSERT_TRUE(started.error.empty()) << started.error;
    if (!started.backend.has_value()) {
        if (std::getenv("CI") != nullptr) {
            FAIL() << "daemon prerequisites missing in CI (must run): " << started.unavailable_what;
        }
        GTEST_SKIP() << "pipewire daemon unavailable on this host: " << started.unavailable_what;
    }
    auto daemon = std::move(*started.backend);

    auto config = pcm_router_config();
    config.backend = aa::audio::SinkBackend::pipewire;
    auto router = aa::audio::detail::open_router_with_native_sink_for_test(config);
    ASSERT_TRUE(router.has_value());

    EXPECT_EQ(router.value().state(), aa::audio::RouterState::degraded);
    const auto& cause = router.value().report().sink.cause;
    EXPECT_FALSE(cause.empty());
    EXPECT_NE(cause.find("disabled"), std::string::npos);

    const std::vector<std::byte> media = constant_pcm({kMediaLevel, 960});
    const auto message =
        make_message(aa::protocol::ChannelRole::media_audio, 0, now_ns(), media);
    auto submitted = router.value().submit(message);
    ASSERT_FALSE(submitted.has_value());
    EXPECT_EQ(submitted.error().code(), aa::ErrorCode::audio_sink_unavailable);
    EXPECT_EQ(router.value().report().stats.frames_written, 0U);
    router.value().close();
}

TEST_F(AudioPipelineRoute, DaemonKillMidStreamDegradesTypedWithRecordedCause) {
    // Gate r2 F3 regression (b): REAL mid-stream output loss - the isolated
    // backend is killed after the first handoff-accepted write (no artificial
    // EOS); the next pump must fail typed and degrade the router with a
    // recorded cause instead of succeeding silently.
    auto started = IsolatedPipeWire::start(true);
    ASSERT_TRUE(started.error.empty()) << started.error;
    if (!started.backend.has_value()) {
        if (std::getenv("CI") != nullptr) {
            FAIL() << "daemon prerequisites missing in CI (must run): " << started.unavailable_what;
        }
        GTEST_SKIP() << "pipewire/wireplumber unavailable on this host";
    }
    auto daemon = std::move(*started.backend);

    auto config = pcm_router_config();
    config.backend = aa::audio::SinkBackend::pipewire;
    auto router = aa::audio::detail::open_router_with_native_sink_for_test(config);
    ASSERT_TRUE(router.has_value());
    ASSERT_EQ(router.value().state(), aa::audio::RouterState::active);

    const std::vector<std::byte> media = constant_pcm({kMediaLevel, 960});
    // Contiguous timeline timestamps (960 frames apart): the kill window is
    // wall-clock time, and a real timestamp gap there would trip the router's
    // discontinuity policy before the dead sink is ever exercised.
    const aa::core::Nanoseconds base_ts = now_ns();
    const aa::core::Nanoseconds contiguous_ts{
        base_ts.count + 960 * 1'000'000'000LL
            / static_cast<std::int64_t>(config.output_format.sample_rate_hz)};
    const auto first = make_message(aa::protocol::ChannelRole::media_audio, 0, base_ts, media);
    ASSERT_TRUE(router.value().submit(first).has_value());
    ASSERT_TRUE(router.value().pump().has_value());
    EXPECT_GE(router.value().report().render_barrier.handoff_events, 1U);

    daemon.kill_backend();
    usleep(1000000);

    const auto second =
        make_message(aa::protocol::ChannelRole::media_audio, 1, contiguous_ts, media);
    ASSERT_TRUE(router.value().submit(second).has_value());
    auto pumped = router.value().pump();
    ASSERT_FALSE(pumped.has_value());
    EXPECT_EQ(pumped.error().code(), aa::ErrorCode::audio_sink_unavailable);

    EXPECT_EQ(router.value().state(), aa::audio::RouterState::degraded);
    const auto& cause = router.value().report().sink.cause;
    EXPECT_FALSE(cause.empty());
    EXPECT_NE(cause.find("disabled"), std::string::npos);
    router.value().close();
}

TEST_F(AudioPipelineRoute, OutputLossMidStreamDegradesTypedWithRecordedCause) {
    auto router = open_router();
    ASSERT_TRUE(router.has_value());
    ASSERT_EQ(router.value().state(), aa::audio::RouterState::active);

    const std::vector<std::byte> media = constant_pcm({kMediaLevel, 960});
    const auto first = make_message(aa::protocol::ChannelRole::media_audio, 0, now_ns(), media);
    ASSERT_TRUE(router.value().submit(first).has_value());
    ASSERT_TRUE(router.value().pump().has_value());
    EXPECT_EQ(router.value().state(), aa::audio::RouterState::active);
    EXPECT_EQ(router.value().report().stats.frames_written, 1U);

    // The routed output goes away mid-session: the stream is drained to
    // end-of-stream (the render drain confirms the output path is finished)
    // while the router stays open. The route is now TERMINAL: the intentional
    // flush refuses every later submission before decode or sink access
    // (invalid_argument - the terminal flush semantic) - never a silent
    // success.
    ASSERT_TRUE(router.value().flush().has_value());
    EXPECT_GE(router.value().report().render_barrier.drains_completed, 1U);

    const auto second = make_message(aa::protocol::ChannelRole::media_audio, 1, now_ns(), media);
    auto refused = router.value().submit(second);
    ASSERT_FALSE(refused.has_value());
    EXPECT_EQ(refused.error().code(), aa::ErrorCode::invalid_argument);
    auto pumped = router.value().pump();
    ASSERT_FALSE(pumped.has_value());
    EXPECT_EQ(pumped.error().code(), aa::ErrorCode::invalid_argument);

    const auto third = make_message(aa::protocol::ChannelRole::media_audio, 2, now_ns(), media);
    auto late_submit = router.value().submit(third);
    ASSERT_FALSE(late_submit.has_value());
    EXPECT_EQ(late_submit.error().code(), aa::ErrorCode::invalid_argument);
    router.value().close();

    // The output-loss assertions themselves (unchanged strength): when the
    // routed output is gone mid-session, the intentional flush surfaces the
    // loss typed and degrades the router with a recorded cause - never a
    // silent success - and every later submission is refused. Uses the
    // isolated daemon so the output can genuinely go away mid-session.
    auto started = IsolatedPipeWire::start(true);
    ASSERT_TRUE(started.error.empty()) << started.error;
    if (!started.backend.has_value()) {
        if (std::getenv("CI") != nullptr) {
            FAIL() << "daemon prerequisites missing in CI (must run): " << started.unavailable_what;
        }
        GTEST_SKIP() << "pipewire/wireplumber unavailable on this host";
    }
    auto daemon = std::move(*started.backend);

    auto config = pcm_router_config();
    config.backend = aa::audio::SinkBackend::pipewire;
    auto lost = aa::audio::detail::open_router_with_native_sink_for_test(config);
    ASSERT_TRUE(lost.has_value());
    ASSERT_EQ(lost.value().state(), aa::audio::RouterState::active);

    const std::vector<std::byte> media_lost = constant_pcm({kMediaLevel, 960});
    const aa::core::Nanoseconds base_ts = now_ns();
    const aa::core::Nanoseconds contiguous_ts{
        base_ts.count + 960 * 1'000'000'000LL
            / static_cast<std::int64_t>(config.output_format.sample_rate_hz)};
    const auto first_lost =
        make_message(aa::protocol::ChannelRole::media_audio, 0, base_ts, media_lost);
    ASSERT_TRUE(lost.value().submit(first_lost).has_value());
    ASSERT_TRUE(lost.value().pump().has_value());
    EXPECT_EQ(lost.value().state(), aa::audio::RouterState::active);
    EXPECT_EQ(lost.value().report().stats.frames_written, 1U);
    const auto pending_lost =
        make_message(aa::protocol::ChannelRole::media_audio, 1, contiguous_ts, media_lost);
    ASSERT_TRUE(lost.value().submit(pending_lost).has_value());

    daemon.kill_backend();
    usleep(1000000);

    // The intentional flush must now refuse a later submission: its mixed
    // write surfaces the output loss typed, degrades the router with the
    // recorded cause, and every later operation keeps the degraded refusal.
    auto flushed = lost.value().flush();
    ASSERT_FALSE(flushed.has_value());
    EXPECT_EQ(flushed.error().code(), aa::ErrorCode::audio_sink_unavailable);

    EXPECT_EQ(lost.value().state(), aa::audio::RouterState::degraded);
    const auto& cause = lost.value().report().sink.cause;
    EXPECT_FALSE(cause.empty());
    EXPECT_NE(cause.find("disabled"), std::string::npos);

    const auto second_lost =
        make_message(aa::protocol::ChannelRole::media_audio, 2, now_ns(), media_lost);
    auto refused_late = lost.value().submit(second_lost);
    ASSERT_FALSE(refused_late.has_value());
    EXPECT_EQ(refused_late.error().code(), aa::ErrorCode::audio_sink_unavailable);
    auto pumped_late = lost.value().pump();
    ASSERT_FALSE(pumped_late.has_value());
    EXPECT_EQ(pumped_late.error().code(), aa::ErrorCode::audio_sink_unavailable);
    lost.value().close();
}

TEST_F(AudioPipelineRoute, UnreachableSinkServerDegradesTypedWithoutSilence) {
    auto config = pcm_router_config();
    config.backend = aa::audio::SinkBackend::pulse;
    config.sink_remote = "unix:/tmp/aa-task25-nonexistent-pulse-server";
    auto router = aa::audio::AudioRouter::open(config);
    ASSERT_TRUE(router.has_value());

    EXPECT_EQ(router.value().state(), aa::audio::RouterState::degraded);
    ASSERT_FALSE(router.value().report().sink.cause.empty());
    EXPECT_NE(router.value().report().sink.cause.find("disabled"), std::string::npos);

    const std::vector<std::byte> payload = read_file(fixture_path("media_48k_s16le_stereo.pcm"));
    const auto message = make_message(aa::protocol::ChannelRole::media_audio, 0, now_ns(), payload);
    auto submitted = router.value().submit(message);
    ASSERT_FALSE(submitted.has_value());
    EXPECT_EQ(submitted.error().code(), aa::ErrorCode::audio_sink_unavailable);

    auto pumped = router.value().pump();
    ASSERT_FALSE(pumped.has_value());
    EXPECT_EQ(pumped.error().code(), aa::ErrorCode::audio_sink_unavailable);

    EXPECT_EQ(router.value().report().sink_probe.buffers, 0U);
    EXPECT_EQ(router.value().report().stats.frames_written, 0U);
}

TEST_F(AudioPipelineRoute, PostCloseOperationsFailTyped) {
    auto router = open_router();
    ASSERT_TRUE(router.has_value());
    router.value().close();
    router.value().close();

    const std::vector<std::byte> payload = read_file(fixture_path("media_48k_s16le_stereo.pcm"));
    const auto message = make_message(aa::protocol::ChannelRole::media_audio, 0, now_ns(), payload);
    auto submitted = router.value().submit(message);
    ASSERT_FALSE(submitted.has_value());
    EXPECT_EQ(submitted.error().code(), aa::ErrorCode::invalid_argument);
    auto pumped = router.value().pump();
    ASSERT_FALSE(pumped.has_value());
    EXPECT_EQ(pumped.error().code(), aa::ErrorCode::invalid_argument);
}

TEST(AudioPipelineDecodeSuite, PcmDecodeDeliversFixtureBytesUnchanged) {
    aa::audio::AudioDecoder::Config config;
    config.codec = aa::audio::Codec::pcm;
    config.input_format = stereo_48k();
    config.output_format = stereo_48k();
    auto decoder = aa::audio::AudioDecoder::open(config);
    ASSERT_TRUE(decoder.has_value());

    const std::vector<std::byte> payload = read_file(fixture_path("media_48k_s16le_stereo.pcm"));
    ASSERT_FALSE(payload.empty());
    const aa::audio::EncodedChunk chunk{aa::audio::Role::media,
                                        aa::audio::Codec::pcm,
                                        stereo_48k(),
                                        7,
                                        aa::core::Nanoseconds{123456789},
                                        payload,
                                        {}};
    auto decoded = decoder.value().decode(chunk);
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(decoded.value().timestamp.count, 123456789);
    EXPECT_EQ(decoded.value().role, aa::audio::Role::media);
    EXPECT_EQ(decoded.value().pcm.size(), payload.size());
    EXPECT_EQ(std::memcmp(decoded.value().pcm.data(), payload.data(), payload.size()), 0);
}

TEST(AudioPipelineDecodeSuite, AacAdtsFramesDecodeToNonSilentPcm) {
    aa::audio::AudioDecoder::Config config;
    config.codec = aa::audio::Codec::aac_lc_adts;
    config.input_format = stereo_48k();
    config.output_format = stereo_48k();

    const auto report = aa::audio::AudioDecoder::probe(aa::audio::Codec::aac_lc_adts);
    for (const auto& candidate : report.candidates) {
        EXPECT_FALSE(candidate.cause.empty());
        if (!candidate.available) {
            EXPECT_NE(candidate.cause.find("not found"), std::string::npos);
        }
    }
    if (!report.usable) {
        GTEST_SKIP() << "no viable AAC decoder: " << report.cause;
    }

    auto decoder = aa::audio::AudioDecoder::open(config);
    ASSERT_TRUE(decoder.has_value());

    const std::vector<std::byte> stream =
        read_file(fixture_path("speech_aac_lc_adts_48k_stereo.aac"));
    const auto frames = split_adts(stream);
    ASSERT_GE(frames.size(), 1U);

    const aa::audio::EncodedChunk chunk{aa::audio::Role::speech,
                                        aa::audio::Codec::aac_lc_adts,
                                        stereo_48k(),
                                        0,
                                        aa::core::Nanoseconds{0},
                                        stream,
                                        {}};
    auto decoded = decoder.value().decode(chunk);
    ASSERT_TRUE(decoded.has_value());
    ASSERT_GT(decoded.value().pcm.size(), 0U);
    ASSERT_EQ(decoded.value().pcm.size() % (2 * sizeof(std::int16_t)), 0U);

    bool non_silent = false;
    for (std::size_t i = 0; i + sizeof(std::int16_t) <= decoded.value().pcm.size();
         i += sizeof(std::int16_t)) {
        std::int16_t sample{};
        std::memcpy(&sample, decoded.value().pcm.data() + i, sizeof(sample));
        if (sample != 0) {
            non_silent = true;
        }
    }
    EXPECT_TRUE(non_silent);
    EXPECT_EQ(decoded.value().pcm.size() % (sizeof(std::int16_t) * 2), 0U);
}

TEST(AudioPipelineDecodeSuite, AacProbeReportsRuntimeAvailabilityAccurately) {
    aa::audio::AudioDecoder::Config config;
    config.codec = aa::audio::Codec::aac_lc_adts;
    config.input_format = stereo_48k();
    config.output_format = stereo_48k();

    const auto report = aa::audio::AudioDecoder::probe(aa::audio::Codec::aac_lc_adts);
    for (const auto& candidate : report.candidates) {
        EXPECT_FALSE(candidate.cause.empty());
        if (!candidate.available) {
            EXPECT_NE(candidate.cause.find("not found"), std::string::npos);
        }
        if (candidate.viable) {
            EXPECT_TRUE(candidate.available);
        }
    }

    auto decoder = aa::audio::AudioDecoder::open(config);
    EXPECT_EQ(report.usable, decoder.has_value());
    if (report.usable) {
        EXPECT_FALSE(report.selected_element.empty());
        bool selected_is_viable = false;
        for (const auto& candidate : report.candidates) {
            if (candidate.element == report.selected_element && candidate.viable) {
                selected_is_viable = true;
            }
        }
        EXPECT_TRUE(selected_is_viable);
    } else {
        EXPECT_FALSE(report.cause.empty());
    }

    const auto selection = aa::audio::select_decode_backend(report);
    EXPECT_EQ(selection.usable, report.usable);
    EXPECT_EQ(selection.element, report.selected_element);
}

TEST(AudioPipelineDecodeSuite, DecodeSelectionPolicyRecordsAbsentDecoders) {
    aa::audio::DecodeReport report;
    report.codec = aa::audio::Codec::aac_lc_adts;
    report.candidates.push_back(
        {"audioconvert", true, true, "Instantiated and transitioned to READY successfully"});
    report.candidates.push_back(
        {"audioresample", true, true, "Instantiated and transitioned to READY successfully"});
    report.candidates.push_back(
        {"aacparse", true, true, "Instantiated and transitioned to READY successfully"});
    report.candidates.push_back(
        {"faad", false, false, "Element not found in GStreamer registry"});
    report.candidates.push_back(
        {"avdec_aac", false, false, "Element not found in GStreamer registry"});

    const auto selection = aa::audio::select_decode_backend(report);
    EXPECT_FALSE(selection.usable);
    EXPECT_TRUE(selection.element.empty());
    EXPECT_NE(selection.cause.find("disabled:"), std::string::npos);
    EXPECT_NE(selection.cause.find("faad absent"), std::string::npos);
    EXPECT_NE(selection.cause.find("avdec_aac absent"), std::string::npos);
}

TEST(AudioPipelineDecodeSuite, DecodeSelectionPolicyHandlesInstalledAvdec) {
    aa::audio::DecodeReport report;
    report.codec = aa::audio::Codec::aac_lc_adts;
    report.candidates.push_back(
        {"audioconvert", true, true, "Instantiated and transitioned to READY successfully"});
    report.candidates.push_back(
        {"audioresample", true, true, "Instantiated and transitioned to READY successfully"});
    report.candidates.push_back(
        {"aacparse", true, true, "Instantiated and transitioned to READY successfully"});
    report.candidates.push_back(
        {"faad", false, false, "Element not found in GStreamer registry"});
    report.candidates.push_back(
        {"avdec_aac", true, true, "Instantiated and transitioned to READY successfully"});

    const auto selection = aa::audio::select_decode_backend(report);
    EXPECT_TRUE(selection.usable);
    EXPECT_EQ(selection.element, "avdec_aac");
    EXPECT_TRUE(selection.cause.empty());
}

TEST(AudioPipelineDecodeSuite, RawAacWithoutAsciFailsTyped) {
    aa::audio::AudioDecoder::Config config;
    config.codec = aa::audio::Codec::aac_lc;
    config.input_format = stereo_48k();
    config.output_format = stereo_48k();
    auto decoder = aa::audio::AudioDecoder::open(config);
    if (!decoder.has_value()) {
        EXPECT_EQ(decoder.error().code(), aa::ErrorCode::audio_format_unsupported);
        return;
    }
    const std::vector<std::byte> payload = read_file(fixture_path("speech_aac_lc_adts_48k_stereo.aac"));
    const aa::audio::EncodedChunk chunk{aa::audio::Role::speech,
                                        aa::audio::Codec::aac_lc,
                                        stereo_48k(),
                                        0,
                                        aa::core::Nanoseconds{0},
                                        payload,
                                        {}};
    auto decoded = decoder.value().decode(chunk);
    ASSERT_FALSE(decoded.has_value());
    EXPECT_EQ(decoded.error().code(), aa::ErrorCode::audio_format_unsupported);
    EXPECT_FALSE(decoder.value().report().cause.empty());
}

TEST(AudioPipelineDecodeSuite, MalformedPayloadFailsTypedWithoutCrash) {
    aa::audio::AudioDecoder::Config config;
    config.codec = aa::audio::Codec::aac_lc_adts;
    config.input_format = stereo_48k();
    config.output_format = stereo_48k();
    const auto report = aa::audio::AudioDecoder::probe(aa::audio::Codec::aac_lc_adts);
    if (!report.usable) {
        GTEST_SKIP() << "no viable AAC decoder: " << report.cause;
    }
    auto decoder = aa::audio::AudioDecoder::open(config);
    ASSERT_TRUE(decoder.has_value());

    std::vector<std::byte> garbage(256, std::byte{0xAB});
    const aa::audio::EncodedChunk chunk{aa::audio::Role::speech,
                                        aa::audio::Codec::aac_lc_adts,
                                        stereo_48k(),
                                        0,
                                        aa::core::Nanoseconds{0},
                                        garbage,
                                        {}};
    auto decoded = decoder.value().decode(chunk);
    ASSERT_FALSE(decoded.has_value());
    EXPECT_TRUE(decoded.error().code() == aa::ErrorCode::audio_decode_failed
                || decoded.error().code() == aa::ErrorCode::timeout);
    decoder.value().close();
    decoder.value().close();
}

TEST(AudioPipelineDecodeSuite, PostCloseDecodeFailsTyped) {
    aa::audio::AudioDecoder::Config config;
    config.codec = aa::audio::Codec::pcm;
    config.input_format = stereo_48k();
    config.output_format = stereo_48k();
    auto decoder = aa::audio::AudioDecoder::open(config);
    ASSERT_TRUE(decoder.has_value());
    decoder.value().close();

    const std::vector<std::byte> payload = read_file(fixture_path("media_48k_s16le_stereo.pcm"));
    const aa::audio::EncodedChunk chunk{aa::audio::Role::media,
                                        aa::audio::Codec::pcm,
                                        stereo_48k(),
                                        0,
                                        aa::core::Nanoseconds{0},
                                        payload,
                                        {}};
    auto decoded = decoder.value().decode(chunk);
    ASSERT_FALSE(decoded.has_value());
    EXPECT_EQ(decoded.error().code(), aa::ErrorCode::invalid_argument);
}

TEST(AudioPipelineSinkBackend, ProbeReportsEveryKnownRoutingElement) {
    const auto report = aa::audio::probe_sinks();
    std::vector<std::string> elements;
    for (const auto& candidate : report.candidates) {
        elements.push_back(candidate.element);
        EXPECT_FALSE(candidate.cause.empty());
    }
    EXPECT_NE(std::find(elements.begin(), elements.end(), "pipewiresink"), elements.end());
    EXPECT_NE(std::find(elements.begin(), elements.end(), "pulsesink"), elements.end());
}

TEST(AudioPipelineSinkBackend, ExplicitRequestNeverDowngradesSilently) {
    aa::audio::SinkReport report;
    report.candidates.push_back({"pipewiresink", true, true, false, "Element not found in GStreamer registry"});
    report.candidates.push_back({"pulsesink", true, true, true, "Instantiated and transitioned to READY successfully"});

    const auto explicit_pipewire = aa::audio::select_sink_backend(report, aa::audio::SinkBackend::pipewire);
    EXPECT_FALSE(explicit_pipewire.usable);
    EXPECT_NE(explicit_pipewire.cause.find("disabled:"), std::string::npos);
    EXPECT_NE(explicit_pipewire.cause.find("pipewiresink"), std::string::npos);

    const auto automatic = aa::audio::select_sink_backend(report, aa::audio::SinkBackend::automatic);
    EXPECT_TRUE(automatic.usable);
    EXPECT_EQ(automatic.element, "pulsesink");
    EXPECT_NE(automatic.cause.find("falling back"), std::string::npos);
}

TEST(AudioPipelineSinkBackend, MissingPipeWireElementsRecordDisabledCause) {
    aa::audio::SinkReport report;
    report.candidates.push_back({"pipewiresink", true, false, false, "Element not found in GStreamer registry"});
    report.candidates.push_back({"pulsesink", true, false, false, "Element not found in GStreamer registry"});

    for (const auto backend :
         {aa::audio::SinkBackend::automatic, aa::audio::SinkBackend::pipewire,
          aa::audio::SinkBackend::pulse}) {
        const auto selection = aa::audio::select_sink_backend(report, backend);
        EXPECT_FALSE(selection.usable);
        EXPECT_TRUE(selection.element.empty());
        EXPECT_NE(selection.cause.find("disabled:"), std::string::npos);
    }
}

// #80 production policy: these tests need no audio daemon. pipewiresink is
// refused outright (no rate control on PipeWire 1.0.5 / Ubuntu noble);
// pulsesink is the default and only supported real-time route.
TEST(AudioPipelineSinkPolicy, DefaultAndPulseRoutesSelectPulseSinkAndRefusePipeWire) {
    EXPECT_EQ(aa::audio::RouterConfig{}.backend, aa::audio::SinkBackend::automatic);
    EXPECT_EQ(aa::audio::detail::SinkOpenRequest{}.backend, aa::audio::SinkBackend::automatic);

    aa::audio::SinkReport report;
    report.candidates = {{"pipewiresink", true, true, true, "READY"},
                         {"pulsesink", true, true, true, "READY"}};
    const auto native = aa::audio::select_sink_backend(report, aa::audio::SinkBackend::pipewire);
    EXPECT_FALSE(native.usable);
    EXPECT_TRUE(native.element.empty());
    EXPECT_NE(native.cause.find("pipewiresink_unsupported"), std::string::npos);
    EXPECT_NE(native.cause.find("pipewiresink"), std::string::npos);
    EXPECT_NE(native.cause.find("pulsesink"), std::string::npos);
    EXPECT_EQ(native.cause.find("unverified"), std::string::npos);

    const auto automatic = aa::audio::select_sink_backend(report, aa::audio::SinkBackend::automatic);
    EXPECT_TRUE(automatic.usable);
    EXPECT_EQ(automatic.element, "pulsesink");
    EXPECT_NE(automatic.cause.find("pipewiresink_unsupported"), std::string::npos);
    EXPECT_EQ(automatic.cause.find("unverified"), std::string::npos);

    const auto pulse = aa::audio::select_sink_backend(report, aa::audio::SinkBackend::pulse);
    EXPECT_TRUE(pulse.usable);
    EXPECT_EQ(pulse.element, "pulsesink");
}

TEST(AudioPipelineSinkPolicy, NativeOnlyHostNeverFallsBackSilentlyToPipewireSink) {
    aa::audio::SinkReport report;
    report.candidates = {{"pipewiresink", true, true, true, "READY"},
                         {"pulsesink", true, false, false, "absent"}};
    for (const auto backend :
         {aa::audio::SinkBackend::automatic, aa::audio::SinkBackend::pipewire,
          aa::audio::SinkBackend::pulse}) {
        const auto selection = aa::audio::select_sink_backend(report, backend);
        EXPECT_FALSE(selection.usable);
        EXPECT_TRUE(selection.element.empty());
        EXPECT_NE(selection.cause.find("pulsesink"), std::string::npos);
        EXPECT_EQ(selection.cause.find("unverified"), std::string::npos);
    }
    const auto automatic = aa::audio::select_sink_backend(report, aa::audio::SinkBackend::automatic);
    EXPECT_NE(automatic.cause.find("pipewiresink_unsupported"), std::string::npos);
    EXPECT_NE(automatic.cause.find("pulsesink absent"), std::string::npos);
}

TEST(AudioPipelineSinkPolicy, SinkOpenRefusesExplicitPipewireRequestBeforeRendering) {
    aa::audio::detail::SinkOpenRequest request;
    request.format = stereo_48k();
    request.backend = aa::audio::SinkBackend::pipewire;
    std::uint64_t writes = 0;
    request.on_write_progress = [&](const auto&) { ++writes; };
    const auto opened = aa::audio::detail::open_default_sink(request);
    EXPECT_FALSE(opened.sink);
    EXPECT_FALSE(opened.report.usable);
    EXPECT_TRUE(opened.report.selected_element.empty());
    EXPECT_NE(opened.report.cause.find("pipewiresink_unsupported"), std::string::npos);
    EXPECT_NE(opened.report.cause.find("pulsesink"), std::string::npos);
    EXPECT_EQ(opened.report.cause.find("unverified"), std::string::npos);
    EXPECT_EQ(writes, 0U);
}

TEST(AudioPipelineSinkPolicy, RouterRefusesExplicitPipewireWithTypedDegradation) {
    auto config = pcm_router_config();
    config.backend = aa::audio::SinkBackend::pipewire;
    auto opened = aa::audio::AudioRouter::open(config);
    ASSERT_TRUE(opened);
    EXPECT_EQ(opened.value().state(), aa::audio::RouterState::degraded);
    EXPECT_FALSE(opened.value().report().sink.usable);
    EXPECT_TRUE(opened.value().report().sink.selected_element.empty());
    EXPECT_NE(opened.value().report().sink.cause.find("pipewiresink_unsupported"),
              std::string::npos);
    EXPECT_NE(opened.value().report().sink.cause.find("pulsesink"), std::string::npos);
    EXPECT_EQ(opened.value().report().sink.cause.find("unverified"), std::string::npos);

    const auto pcm = constant_pcm({kMediaLevel, 960});
    const auto result = opened.value().submit(make_message(
        aa::protocol::ChannelRole::media_audio, 0, now_ns(), pcm));
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code(), aa::ErrorCode::audio_sink_unavailable);
    auto pumped = opened.value().pump();
    ASSERT_FALSE(pumped);
    EXPECT_EQ(pumped.error().code(), aa::ErrorCode::audio_sink_unavailable);
    EXPECT_EQ(opened.value().report().stats.frames_written, 0U);
}

TEST(AudioPipelineSinkBackend, DefaultAutomaticOpenSelectsPulseSinkWhenRuntimeAvailable) {
    aa::audio::detail::SinkOpenRequest request;
    request.format = stereo_48k();
    request.start_timeout = aa::core::Milliseconds{2000};
    request.write_timeout = aa::core::Milliseconds{200};
    auto outcome = aa::audio::detail::open_default_sink(request);
    if (!outcome.sink) {
        if (std::getenv("CI") != nullptr) {
            FAIL() << "pulsesink runtime unavailable in CI: " << outcome.report.cause;
        }
        GTEST_SKIP() << "pulsesink runtime unavailable on this host: " << outcome.report.cause;
    }
    EXPECT_TRUE(outcome.report.usable);
    EXPECT_EQ(outcome.report.selected_element, "pulsesink");
}

TEST(AudioPipelineSinkBackend, FullAppsrcQueueHonorsSingleWriteDeadline) {
    // Gate fix 3 regression: with the appsrc queue prefilled to its byte
    // threshold and downstream blocked after readiness, a real write() must
    // fail at ADMISSION within ONE deadline (~200 ms). It must not hang in the
    // enqueue (the old blocking appsrc push never returned), must not push
    // into the full queue (queue levels stay exactly as prefilled), and must
    // not wait for a handoff that cannot come (no acknowledged handoffs).
    aa::audio::detail::SinkOpenRequest request;
    // Uses pulsesink: the only supported production real-time route (#80).
    request.backend = aa::audio::SinkBackend::pulse;
    request.format = stereo_48k();
    request.start_timeout = aa::core::Milliseconds{2000};
    request.write_timeout = aa::core::Milliseconds{200};
    std::mutex progress_mutex;
    std::optional<aa::audio::SinkWriteProgress> progress;
    request.on_write_progress = [&](const aa::audio::SinkWriteProgress& value) {
        std::lock_guard lock(progress_mutex);
        progress = value;
    };
    auto outcome = aa::audio::detail::open_default_sink(request);
    ASSERT_TRUE(outcome.sink != nullptr) << outcome.report.cause;

    // Releases the downstream block on EVERY exit path, including assertion
    // failures: the guard is declared after the sink so it runs first.
    struct UnblockGuard final {
        aa::audio::detail::ObservedAudioSink* sink{nullptr};
        ~UnblockGuard() {
            if (sink != nullptr) {
                aa::audio::detail::unblock_downstream_for_test(*sink);
            }
        }
    };
    UnblockGuard unblock{outcome.sink.get()};

    ASSERT_TRUE(aa::audio::detail::block_downstream_for_test(*outcome.sink));
    // One trigger buffer makes the block hold real data - the stall is
    // engaged and later pushes can only accumulate in the appsrc queue.
    (void)aa::audio::detail::prefill_appsrc_for_test(*outcome.sink, 4);
    ASSERT_TRUE(aa::audio::detail::wait_downstream_block_entered_for_test(
        *outcome.sink, aa::core::Milliseconds{2000}));

    const aa::audio::detail::AppsrcQueueLevel limits =
        aa::audio::detail::appsrc_queue_level_for_test(*outcome.sink);
    EXPECT_EQ(limits.max_bytes, 65536U);
    EXPECT_EQ(limits.max_buffers, 2U);
    // Prefill the appsrc to its byte threshold (the block holds the streaming
    // thread, so the fill behind it is deterministic).
    const std::uint64_t filled =
        aa::audio::detail::prefill_appsrc_for_test(*outcome.sink, limits.max_bytes);
    EXPECT_GE(filled, limits.max_bytes);
    const aa::audio::detail::AppsrcQueueLevel before =
        aa::audio::detail::appsrc_queue_level_for_test(*outcome.sink);
    const auto handoffs_before = outcome.sink->render_stats().handoff_events;

    const std::vector<std::byte> pcm = constant_pcm({kMediaLevel, 960});
    const aa::audio::MixedChunk chunk{stereo_48k(), now_ns(),
                                      std::span<const std::byte>{pcm}};
    const auto started = std::chrono::steady_clock::now();
    auto written = outcome.sink->write(chunk);
    const auto elapsed = std::chrono::steady_clock::now() - started;

    ASSERT_FALSE(written.has_value());
    EXPECT_EQ(written.error().code(), aa::ErrorCode::timeout);
    // ONE deadline of 200 ms shared by admission, chain settlement and the
    // handoff: bounded below anything that would stack a second timeout
    // window - and a blocking enqueue would never have returned at all.
    EXPECT_LT(elapsed, std::chrono::milliseconds{350});
    EXPECT_GE(elapsed, std::chrono::milliseconds{150});
    EXPECT_EQ(outcome.sink->render_stats().handoff_events, handoffs_before);
    const aa::audio::detail::AppsrcQueueLevel after =
        aa::audio::detail::appsrc_queue_level_for_test(*outcome.sink);
    // The write never entered the queue: no queue-limit growth at all.
    EXPECT_EQ(after.queued_bytes, before.queued_bytes);
    EXPECT_EQ(after.queued_buffers, before.queued_buffers);
    EXPECT_LE(after.queued_bytes, after.max_bytes);
    EXPECT_LE(after.queued_buffers, after.max_buffers);
    std::lock_guard lock(progress_mutex);
    ASSERT_TRUE(progress);
    EXPECT_FALSE(progress->readiness_probe);
    EXPECT_EQ(progress->effective_scheduled, chunk.timestamp);
    EXPECT_EQ(progress->deadline.count - progress->entered_at.count, 200'000'000);
    EXPECT_TRUE(progress->levels_valid);
    EXPECT_GE(progress->queued_bytes, limits.max_bytes);
    EXPECT_FALSE(progress->admitted_at);
    EXPECT_FALSE(progress->pair_pushed_at);
    EXPECT_FALSE(progress->ingress_at);
    EXPECT_FALSE(progress->marker_at);
    EXPECT_FALSE(progress->handoff_at);
    EXPECT_EQ(progress->outcome, aa::ErrorCode::timeout);
    ASSERT_TRUE(progress->completed_at);
    EXPECT_GE(*progress->completed_at, progress->deadline);
    // The guard releases the downstream block here on every exit path.
}

TEST(AudioPipelineSinkBackend, FrozenProgressDistinguishesPushedFromIngressAndSurvivesTeardown) {
    std::mutex mutex;
    std::condition_variable cv;
    std::optional<aa::audio::SinkWriteProgress> latest;
    aa::audio::detail::SinkOpenRequest request;
    // Uses pulsesink: the only supported production real-time route (#80).
    request.backend = aa::audio::SinkBackend::pulse;
    request.format = stereo_48k();
    request.write_timeout = aa::core::Milliseconds{2000};
    request.on_write_progress = [&](const aa::audio::SinkWriteProgress& value) {
        std::lock_guard lock(mutex);
        latest = value;
        cv.notify_all();
    };
    auto outcome = aa::audio::detail::open_default_sink(request);
    ASSERT_TRUE(outcome.sink);
    {
        std::lock_guard lock(mutex);
        ASSERT_TRUE(latest);
        EXPECT_TRUE(latest->readiness_probe);
    }
    ASSERT_TRUE(aa::audio::detail::block_downstream_for_test(*outcome.sink));
    const auto pcm = constant_pcm({kMediaLevel, 960});
    const aa::audio::MixedChunk chunk{stereo_48k(), now_ns(), pcm};
    aa::core::Result<void> written;
    std::thread writer([&] { written = outcome.sink->write(chunk); });
    // Always release the real block and join before sink/callback destruction.
    struct Release final {
        aa::audio::detail::ObservedAudioSink* sink;
        std::thread& writer;
        ~Release() {
            if (sink) { aa::audio::detail::unblock_downstream_for_test(*sink); }
            if (writer.joinable()) { writer.join(); }
        }
    } release{outcome.sink.get(), writer};
    ASSERT_TRUE(aa::audio::detail::wait_downstream_block_entered_for_test(
        *outcome.sink, aa::core::Milliseconds{1000}));
    aa::audio::SinkWriteProgress frozen;
    {
        std::unique_lock lock(mutex);
        ASSERT_TRUE(cv.wait_for(lock, std::chrono::seconds{1}, [&] {
            return latest && !latest->readiness_probe && latest->pair_pushed_at;
        }));
        frozen = *latest;
    }
    ASSERT_TRUE(frozen.admitted_at);
    ASSERT_TRUE(frozen.pair_pushed_at);
    EXPECT_LE(frozen.entered_at, *frozen.admitted_at);
    EXPECT_LE(*frozen.admitted_at, *frozen.pair_pushed_at);
    EXPECT_FALSE(frozen.ingress_at);
    EXPECT_FALSE(frozen.marker_at);
    EXPECT_FALSE(frozen.handoff_at);
    EXPECT_FALSE(frozen.completed_at);
    EXPECT_FALSE(frozen.outcome);
    EXPECT_EQ(frozen.effective_scheduled, chunk.timestamp);
    EXPECT_TRUE(frozen.pts_ns);
    EXPECT_TRUE(frozen.pts_origin_ns);
    ASSERT_TRUE(frozen.clock_ns);
    ASSERT_TRUE(frozen.base_time_ns);
    ASSERT_TRUE(frozen.running_time_ns);
    EXPECT_EQ(*frozen.running_time_ns, *frozen.clock_ns - *frozen.base_time_ns);
    const auto retained = frozen;
    EXPECT_EQ(outcome.sink->render_stats().handoff_events, 0U);
    const auto levels = aa::audio::detail::appsrc_queue_level_for_test(*outcome.sink);
    EXPECT_EQ(levels.max_bytes, 65536U);
    EXPECT_EQ(levels.max_buffers, 2U);
    aa::audio::detail::unblock_downstream_for_test(*outcome.sink);
    writer.join();
    EXPECT_TRUE(written);
    {
        std::lock_guard lock(mutex);
        ASSERT_TRUE(latest->ingress_at);
        ASSERT_TRUE(latest->marker_at);
        ASSERT_TRUE(latest->handoff_at);
        ASSERT_TRUE(latest->completed_at);
        EXPECT_LE(*latest->pair_pushed_at, *latest->ingress_at);
        EXPECT_LE(*latest->ingress_at, *latest->handoff_at);
        EXPECT_LE(*latest->handoff_at, *latest->marker_at);
        EXPECT_LE(*latest->marker_at, *latest->completed_at);
        EXPECT_FALSE(latest->outcome);
    }
    EXPECT_EQ(outcome.sink->render_stats().handoff_events, 1U);
    EXPECT_EQ(outcome.sink->render_stats().chain_return_buffers, 1U);
    EXPECT_TRUE(outcome.sink->drain(aa::core::Milliseconds{2000}));
    release.sink = nullptr;
    outcome.sink.reset();
    EXPECT_EQ(frozen, retained);
}

namespace {

[[nodiscard]] aa::audio::detail::SinkOpenOutcome open_slot_test_sink() {
    aa::audio::detail::SinkOpenRequest request;
    // Uses pulsesink: the only supported production real-time route (#80).
    request.backend = aa::audio::SinkBackend::pulse;
    request.format = stereo_48k();
    request.start_timeout = aa::core::Milliseconds{2000};
    request.write_timeout = aa::core::Milliseconds{200};
    return aa::audio::detail::open_default_sink(request);
}

// Paced 20 ms writes on one sink, with the playback-slot answer of each.
class SlotWriter final {
public:
    explicit SlotWriter(aa::audio::detail::ObservedAudioSink& sink) : sink_(&sink) {}

    [[nodiscard]] aa::core::Result<void> write_next() {
        const aa::audio::MixedChunk chunk{stereo_48k(), aa::core::Nanoseconds{timestamp_},
                                          std::span<const std::byte>{pcm_}};
        timestamp_ += 20'000'000;
        return sink_->write(chunk);
    }

    [[nodiscard]] bool slot_returns_within(int writes) {
        for (int i = 0; i < writes; ++i) {
            if (!write_next().has_value()) { return false; }
            if (sink_->last_playback_slot()) { return true; }
        }
        return false;
    }

private:
    aa::audio::detail::ObservedAudioSink* sink_;
    std::vector<std::byte> pcm_{constant_pcm({kMediaLevel, 960})};
    std::int64_t timestamp_{now_ns().count};
};

} // namespace

TEST(AudioPipelineSinkBackend, DelayedPlayingDoesNotBecomeMediaPresentationOffset) {
    for (const auto delay_ms : {75, 25}) {
        SCOPED_TRACE(delay_ms);
        aa::audio::detail::SinkOpenRequest request;
        request.backend = aa::audio::SinkBackend::pulse;
        request.format = stereo_48k();
        request.readiness_delay_for_test = aa::core::Milliseconds{delay_ms};
        std::optional<aa::audio::SinkWriteProgress> latest;
        request.on_write_progress = [&](const auto& progress) { latest = progress; };
        const auto started = now_ns();
        auto outcome = aa::audio::detail::open_default_sink(request);
        ASSERT_TRUE(outcome.sink) << outcome.report.cause;
        EXPECT_GE(now_ns().count - started.count, delay_ms * 1'000'000);
        ASSERT_TRUE(latest);
        EXPECT_TRUE(latest->readiness_probe);
        ASSERT_TRUE(latest->pts_ns);
        const auto pcm = constant_pcm({kMediaLevel, 960});
        for (int epoch = 0; epoch < 2; ++epoch) {
            // A future sample avoids measuring already-late data/clamping.
            const aa::core::Nanoseconds scheduled{now_ns().count + 80'000'000};
            ASSERT_TRUE(outcome.sink->write({stereo_48k(), scheduled, pcm}));
            ASSERT_TRUE(outcome.sink->last_playback_slot());
            EXPECT_LT(std::abs(outcome.sink->last_playback_slot()->count - scheduled.count),
                      10'000'000);
            ASSERT_TRUE(latest->pts_origin_ns);
            ASSERT_TRUE(latest->pts_ns);
            EXPECT_EQ(*latest->pts_ns, static_cast<std::uint64_t>(
                scheduled.count - *latest->pts_origin_ns));
            if (epoch == 0) {
                ASSERT_TRUE(aa::audio::detail::replay_sink_for_test(
                    *outcome.sink, aa::core::Milliseconds{75}));
            }
        }
    }
}

TEST(AudioPipelineSinkBackend, PlaybackSlotQueryAbsenceReasons) {
    using Reason = aa::audio::PlaybackSlotAbsence;
    using aa::audio::detail::query_playback_slot_for_test;
    EXPECT_EQ(query_playback_slot_for_test(0, UINT64_MAX), Reason::invalid_pts);
    EXPECT_EQ(query_playback_slot_for_test(0, static_cast<std::uint64_t>(INT64_MAX) + 1),
              Reason::invalid_pts);
    EXPECT_EQ(query_playback_slot_for_test(0, static_cast<std::uint64_t>(INT64_MAX)),
              Reason::invalid_pts);
    EXPECT_EQ(query_playback_slot_for_test(std::nullopt, 0), Reason::query_failed);
    EXPECT_EQ(query_playback_slot_for_test(-1, 0), Reason::negative_position);
    EXPECT_EQ(query_playback_slot_for_test(0, 0), Reason::none);
    auto outcome = open_slot_test_sink();
    ASSERT_TRUE(outcome.sink);
    EXPECT_EQ(aa::audio::detail::request_playback_slot_for_test(
        *outcome.sink, 0, aa::core::Milliseconds{0}), Reason::deadline_missed);
    aa::audio::detail::stop_playback_slot_worker_for_test(*outcome.sink);
    EXPECT_EQ(aa::audio::detail::request_playback_slot_for_test(
        *outcome.sink, 0, aa::core::Milliseconds{100}), Reason::worker_stopped);
}

TEST(AudioPipelineSinkBackend, StalledPlaybackSlotQueryNeverExtendsTheWrite) {
    // #60 review regression: the element's position query (the playback-slot
    // telemetry) must not hold an acknowledged write. With the query blocked
    // inside the element, writes still succeed within their ONE deadline and
    // report no slot; once released, slots come back.
    auto outcome = open_slot_test_sink();
    ASSERT_TRUE(outcome.sink != nullptr) << outcome.report.cause;
    SlotWriter writer{*outcome.sink};
    ASSERT_TRUE(writer.slot_returns_within(10));

    // Declared after the sink: released (and the worker unblocked) first.
    const auto stall = aa::audio::detail::stall_playback_slot_query_for_test(*outcome.sink);
    ASSERT_TRUE(stall != nullptr);
    for (int i = 0; i < 3; ++i) {
        const auto started = std::chrono::steady_clock::now();
        const auto written = writer.write_next();
        const auto elapsed = std::chrono::steady_clock::now() - started;
        ASSERT_TRUE(written.has_value()) << "write " << i;
        EXPECT_FALSE(outcome.sink->last_playback_slot()) << "write " << i;
        EXPECT_EQ(outcome.sink->last_playback_slot_absence(),
                  i == 0 ? aa::audio::PlaybackSlotAbsence::deadline_missed
                         : aa::audio::PlaybackSlotAbsence::worker_busy);
        // Bounded by the write's own deadline, never by the blocked query.
        EXPECT_LT(elapsed, std::chrono::milliseconds{200}) << "write " << i;
    }
    EXPECT_TRUE(stall->query_entered(aa::core::Milliseconds{1000}));

    stall->release();
    EXPECT_TRUE(writer.slot_returns_within(10));
}

TEST(AudioPipelineSinkBackend, TeardownAndReopenNeverWaitOnBlockedPlaybackSlotQuery) {
    // #60 re-review regression: a position query genuinely in flight and
    // blocked inside GStreamer (gst_element_query_position -> base sink
    // position -> element clock read) must not hold sink teardown, including
    // its NULL state transition on the same element, or a reopen. The
    // blocked worker is left behind with its own element reference and
    // finishes once the query returns, after its sink is gone.
    auto outcome = open_slot_test_sink();
    ASSERT_TRUE(outcome.sink != nullptr) << outcome.report.cause;
    SlotWriter writer{*outcome.sink};
    ASSERT_TRUE(writer.slot_returns_within(10));

    const auto stall = aa::audio::detail::stall_playback_slot_query_for_test(*outcome.sink);
    ASSERT_TRUE(stall != nullptr);
    ASSERT_TRUE(writer.write_next().has_value());
    ASSERT_TRUE(stall->query_entered(aa::core::Milliseconds{1000}));

    // Stop: the old sink's teardown returns with its query still blocked.
    const auto started = std::chrono::steady_clock::now();
    outcome.sink.reset();
    const auto teardown = std::chrono::steady_clock::now() - started;
    // The 2 s drain bound plus the 100 ms worker stop wait, never unbounded.
    EXPECT_LT(teardown, std::chrono::milliseconds{2500});
    // Teardown, NULL transition included, finished while the query was still
    // blocked inside GStreamer.
    EXPECT_FALSE(stall->query_returned());
    EXPECT_FALSE(stall->worker_exited(aa::core::Milliseconds{0}));

    // Reopen: a fresh route works, with slots, while the old query is stuck.
    auto reopened = open_slot_test_sink();
    ASSERT_TRUE(reopened.sink != nullptr);
    SlotWriter reopened_writer{*reopened.sink};
    EXPECT_TRUE(reopened_writer.slot_returns_within(10));
    reopened.sink.reset();
    EXPECT_FALSE(stall->query_returned());

    // The left-behind worker completes on its own once the query returns,
    // against an element already in NULL with its pipeline gone.
    stall->release();
    EXPECT_TRUE(stall->worker_exited(aa::core::Milliseconds{2000}));
}

TEST(AudioPipelineSinkBackend, MissingWriteObservationsRemainUnknown) {
    const aa::audio::SinkWriteProgress value;
    EXPECT_FALSE(value.levels_valid);
    EXPECT_FALSE(value.pts_ns);
    EXPECT_FALSE(value.pts_origin_ns);
    EXPECT_FALSE(value.clock_ns);
    EXPECT_FALSE(value.base_time_ns);
    EXPECT_FALSE(value.running_time_ns);
    EXPECT_FALSE(value.admitted_at);
    EXPECT_FALSE(value.pair_pushed_at);
    EXPECT_FALSE(value.ingress_at);
    EXPECT_FALSE(value.marker_at);
    EXPECT_FALSE(value.handoff_at);
    EXPECT_FALSE(value.completed_at);
    EXPECT_FALSE(value.outcome);
}

TEST_F(AudioPipelineRoute, ProgressCallbackCarriesEffectiveScheduleWithoutChangingReceiptOrTrim) {
    auto config = pcm_router_config();
    config.live_pcm_drift.enabled = true;
    std::mutex mutex;
    std::optional<aa::audio::SinkWriteProgress> latest;
    std::uint64_t readiness_sequence = 0;
    config.on_sink_write_progress = [&](const auto& value) {
        std::lock_guard lock(mutex);
        latest = value;
        if (value.readiness_probe) { readiness_sequence = value.sequence; }
    };
    auto opened = aa::audio::AudioRouter::open(config);
    ASSERT_TRUE(opened);
    auto& router = opened.value();
    ASSERT_EQ(router.state(), aa::audio::RouterState::active);
    const auto scheduled = now_ns();
    const auto pcm = constant_pcm({kMediaLevel, 2048});
    const auto receipt = aa::core::Nanoseconds{scheduled.count - 40'000'000};
    ASSERT_TRUE(router.submit(make_message(aa::protocol::ChannelRole::media_audio, 1, scheduled, pcm), receipt));
    ASSERT_TRUE(router.pump());
    ASSERT_TRUE(router.submit(make_message(aa::protocol::ChannelRole::media_audio, 2, scheduled, pcm), receipt));
    ASSERT_TRUE(router.pump());
    aa::audio::SinkWriteProgress frozen;
    {
        std::lock_guard lock(mutex);
        ASSERT_TRUE(latest);
        frozen = *latest;
    }
    EXPECT_GT(frozen.sequence, readiness_sequence);
    EXPECT_FALSE(frozen.readiness_probe);
    EXPECT_EQ(frozen.effective_scheduled.count, scheduled.count + 42'666'666);
    EXPECT_EQ(router.report().stats.frames_decoded, 4096U);
    EXPECT_EQ(router.report().stats.frames_trimmed, 240U);
    EXPECT_EQ(router.report().stats.frames_retained, 3856U);
    ASSERT_TRUE(frozen.completed_at);
    EXPECT_GE(router.report().stats.last_path_latency.count,
              frozen.completed_at->count - receipt.count);
    router.close();
    EXPECT_TRUE(frozen.completed_at);
    EXPECT_EQ(frozen.effective_scheduled.count, scheduled.count + 42'666'666);
}

TEST(MicrophoneContract, DisabledByDefaultWithTypedRefusal) {
    aa::audio::MicrophoneGate gate;
    EXPECT_EQ(gate.state(), aa::audio::MicrophoneState::disabled);
    auto admitted = gate.admit();
    ASSERT_FALSE(admitted.has_value());
    EXPECT_EQ(admitted.error().code(), aa::ErrorCode::audio_mic_disabled);
}

TEST(MicrophoneContract, ExplicitEnableIsTheOnlyWayIn) {
    aa::audio::MicrophoneGate gate;
    gate.enable_explicitly();
    EXPECT_EQ(gate.state(), aa::audio::MicrophoneState::enabled);
    EXPECT_TRUE(gate.admit().has_value());
    gate.disable();
    auto admitted = gate.admit();
    ASSERT_FALSE(admitted.has_value());
    EXPECT_EQ(admitted.error().code(), aa::ErrorCode::audio_mic_disabled);
}

namespace {

class FakeMicrophoneSource final : public aa::audio::MicrophoneSource {
public:
    [[nodiscard]] aa::core::Result<void> write(const aa::audio::PcmChunk& chunk) override {
        if (chunk.pcm.empty()) {
            return aa::Error{aa::ErrorCode::invalid_argument};
        }
        chunks.push_back(chunk.pcm.size());
        return {};
    }
    void flush() noexcept override { flushed = true; }

    std::vector<std::size_t> chunks{};
    bool flushed{false};
};

} // namespace

TEST(MicrophoneContract, FixtureSamplesRoundTripThroughEnabledSeam) {
    aa::audio::MicrophoneGate gate;
    FakeMicrophoneSource source;

    const std::vector<std::byte> payload = read_file(fixture_path("speech_48k_s16le_stereo.pcm"));
    const aa::audio::PcmChunk chunk{aa::audio::Role::speech, stereo_48k(), now_ns(),
                                    std::span<const std::byte>{payload}};

    ASSERT_FALSE(gate.admit().has_value());
    gate.enable_explicitly();
    ASSERT_TRUE(gate.admit().has_value());
    ASSERT_TRUE(source.write(chunk).has_value());
    source.flush();

    ASSERT_EQ(source.chunks.size(), 1U);
    EXPECT_EQ(source.chunks[0], payload.size());
    EXPECT_TRUE(source.flushed);
}

TEST(MicrophoneContract, RouterOperationsNeverEnableTheGate) {
    aa::audio::MicrophoneGate gate;
    auto router = aa::audio::AudioRouter::open(pcm_router_config());
    ASSERT_TRUE(router.has_value());

    const std::vector<std::byte> payload = read_file(fixture_path("media_48k_s16le_stereo.pcm"));
    const auto message =
        make_message(aa::protocol::ChannelRole::media_audio, 0, now_ns(), payload);
    ASSERT_TRUE(router.value().submit(message).has_value());
    ASSERT_TRUE(router.value().pump().has_value());

    EXPECT_EQ(gate.state(), aa::audio::MicrophoneState::disabled);

    const auto mic_message =
        make_message(aa::protocol::ChannelRole::microphone, 1, now_ns(), payload);
    auto mic_submit = router.value().submit(mic_message);
    ASSERT_FALSE(mic_submit.has_value());
    EXPECT_EQ(mic_submit.error().code(), aa::ErrorCode::audio_mic_disabled);
    EXPECT_EQ(gate.state(), aa::audio::MicrophoneState::disabled);
}

namespace {

std::string synthetic_scratch_dir() {
    char templ[] = "/tmp/aa-task25-cfg-XXXXXX";
    const char* dir = mkdtemp(templ);
    return dir == nullptr ? std::string{} : std::string{dir};
}

std::string read_text_file(const std::string& path) {
    std::ifstream input{path};
    return {std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}

} // namespace

TEST_F(AudioPipelineRoute, RestrictedDaemonConfigSyntheticTemplateSucceeds) {
    const std::string scratch = synthetic_scratch_dir();
    ASSERT_FALSE(scratch.empty());
    const std::string templ = scratch + "/template.conf";
    {
        std::ofstream out{templ};
        out << "context.modules = [\n"
               "    { name = libpipewire-module-rtkit\n"
               "        flags = [ ifexists nofail ]\n"
               "    }\n"
               "    { name = libpipewire-module-portal\n"
               "        args = { }\n"
               "    }\n"
               "]\n"
               "context.objects = [\n"
               "]\n";
        ASSERT_TRUE(out.good());
    }
    const std::string dest = scratch + "/restricted.conf";
    const auto written = IsolatedPipeWire::write_restricted_daemon_config(templ, dest, true);
    EXPECT_EQ(written.stage, IsolatedPipeWire::ConfigOutcome::Stage::ok) << written.detail;
    const std::string body = read_text_file(dest);
    EXPECT_NE(body.find("libpipewire-module-rtkit"), std::string::npos);
    EXPECT_EQ(body.find("libpipewire-module-portal"), std::string::npos);
    EXPECT_NE(body.find("auto_null"), std::string::npos);

    const std::string bare = scratch + "/bare.conf";
    const auto no_sink =
        IsolatedPipeWire::write_restricted_daemon_config(templ, bare, false);
    EXPECT_EQ(no_sink.stage, IsolatedPipeWire::ConfigOutcome::Stage::ok) << no_sink.detail;
    EXPECT_EQ(read_text_file(bare).find("auto_null"), std::string::npos);
}

TEST_F(AudioPipelineRoute, RestrictedDaemonConfigMissingTemplateReportsTemplateRead) {
    const auto out = IsolatedPipeWire::write_restricted_daemon_config(
        "/nonexistent/aa-task25-template.conf", "/tmp/aa-task25-unused.conf", false);
    EXPECT_EQ(out.stage, IsolatedPipeWire::ConfigOutcome::Stage::template_read);
    EXPECT_NE(out.detail.find("/nonexistent/aa-task25-template.conf"), std::string::npos);
}

TEST_F(AudioPipelineRoute, RestrictedDaemonConfigUnwritableDestinationReportsDestinationOpen) {
    const std::string scratch = synthetic_scratch_dir();
    ASSERT_FALSE(scratch.empty());
    const std::string templ = scratch + "/template.conf";
    {
        std::ofstream out{templ};
        out << "context.modules = [\n]\n";
        ASSERT_TRUE(out.good());
    }
    const auto out = IsolatedPipeWire::write_restricted_daemon_config(templ, scratch, false);
    EXPECT_EQ(out.stage, IsolatedPipeWire::ConfigOutcome::Stage::destination_open);
    EXPECT_NE(out.detail.find(scratch), std::string::npos);
}

TEST_F(AudioPipelineRoute, RestrictedDaemonConfigRetainedForbiddenModuleIsRejected) {
    const std::string scratch = synthetic_scratch_dir();
    ASSERT_FALSE(scratch.empty());
    const std::string templ = scratch + "/template.conf";
    {
        std::ofstream out{templ};
        out << "context.modules = [\n"
               "    { name = \"libpipewire-module-portal\" }\n"
               "]\n";
        ASSERT_TRUE(out.good());
    }
    const auto out =
        IsolatedPipeWire::write_restricted_daemon_config(templ, scratch + "/out.conf", false);
    EXPECT_EQ(out.stage, IsolatedPipeWire::ConfigOutcome::Stage::denied_module_verify);
    EXPECT_NE(out.detail.find("libpipewire-module-portal"), std::string::npos);
}

TEST_F(AudioPipelineRoute, RestrictedDaemonConfigDeferredWriteFailureReportsWriteClose) {
    // Deferred output failure regression: /dev/full accepts the open and
    // buffers small writes - the ENOSPC surfaces only at flush/close. BOTH
    // output branches must close() explicitly and report Stage::write_close;
    // a pre-close-only stream check (or a destructor-discovered failure)
    // reports success while the bytes are lost.
    const std::string scratch = synthetic_scratch_dir();
    ASSERT_FALSE(scratch.empty());
    const std::string templ = scratch + "/template.conf";
    {
        std::ofstream out{templ};
        out << "context.modules = [\n]\ncontext.objects = [\n]\n";
        ASSERT_TRUE(out.good());
    }
    const auto with_sink =
        IsolatedPipeWire::write_restricted_daemon_config(templ, "/dev/full", true);
    EXPECT_EQ(with_sink.stage, IsolatedPipeWire::ConfigOutcome::Stage::write_close);
    EXPECT_NE(with_sink.detail.find("/dev/full"), std::string::npos);
    const auto bare =
        IsolatedPipeWire::write_restricted_daemon_config(templ, "/dev/full", false);
    EXPECT_EQ(bare.stage, IsolatedPipeWire::ConfigOutcome::Stage::write_close);
    EXPECT_NE(bare.detail.find("/dev/full"), std::string::npos);
}

namespace {

// Counters and captured output live in TEST-OWNED state shared with the fake
// sink (gate round finding: AudioRouter::close() destroys the sink, so every
// post-close assertion must read state that outlives it). Tests capture it as
// `auto rec = sink->state;` BEFORE the sink is moved into the router.
struct RecordingSinkState final {
    std::vector<std::vector<std::byte>> writes{};
    std::vector<std::int64_t> write_ns{};
    std::uint64_t drain_calls{0};
    std::uint64_t drains_completed{0};
    bool fail_drain{false};
    bool fail_write{false};
    std::function<void()> during_write{};
    // Sees every chunk before it is recorded; on_hold answers hold_stream().
    std::function<void(const aa::audio::MixedChunk&)> on_write{};
    std::function<bool(aa::core::Nanoseconds)> on_hold{};
    std::optional<aa::core::Nanoseconds> playback_slot{};
    aa::audio::PlaybackSlotAbsence slot_absence{aa::audio::PlaybackSlotAbsence::unavailable};
};

class RecordingAudioSink final : public aa::audio::detail::ObservedAudioSink {
public:
    std::shared_ptr<RecordingSinkState> state{std::make_shared<RecordingSinkState>()};

    aa::core::Result<void> write(const aa::audio::MixedChunk& chunk) override {
        if (state->during_write) { state->during_write(); }
        if (state->on_write) { state->on_write(chunk); }
        state->writes.emplace_back(chunk.pcm.begin(), chunk.pcm.end());
        state->write_ns.push_back(chunk.timestamp.count);
        if (state->fail_write) { return aa::Error{aa::ErrorCode::audio_sink_unavailable}; }
        return {};
    }
    aa::core::Result<void> drain(aa::core::Milliseconds) override {
        state->drain_calls += 1;
        if (state->fail_drain) {
            return aa::Error{aa::ErrorCode::audio_sink_unavailable};
        }
        state->drains_completed += 1;
        return {};
    }
    aa::audio::SinkProbeStats probe_stats() const override { return {}; }
    aa::audio::RenderBarrierStats render_stats() const override {
        aa::audio::RenderBarrierStats stats;
        stats.drains_completed = state->drains_completed;
        return stats;
    }
    std::optional<aa::core::Nanoseconds> last_playback_slot() const override {
        return state->playback_slot;
    }
    aa::audio::PlaybackSlotAbsence last_playback_slot_absence() const override {
        return state->playback_slot ? aa::audio::PlaybackSlotAbsence::none : state->slot_absence;
    }
    bool hold_stream(aa::core::Nanoseconds by) override {
        return state->on_hold ? state->on_hold(by) : false;
    }
};

std::vector<std::int16_t> flatten_i16(const std::vector<std::vector<std::byte>>& writes) {
    std::vector<std::int16_t> out;
    for (const auto& w : writes) {
        for (std::size_t i = 0; i + 1 < w.size(); i += 2) {
            std::int16_t value = 0;
            std::memcpy(&value, &w[i], sizeof value);
            out.push_back(value);
        }
    }
    return out;
}

} // namespace

TEST_F(AudioPipelineRoute, ExplicitReceiptIncludesQueueWaitWithoutMovingSchedule) {
    auto sink = std::make_unique<RecordingAudioSink>();
    const auto observed = sink->state;
    auto config = pcm_router_config();
    std::optional<aa::audio::WriteCompletion> completion;
    config.on_write_completed = [&](const auto& value) { completion = value; };
    auto router = aa::audio::detail::open_router_with_sink_for_test(config, std::move(sink));
    ASSERT_TRUE(router);
    const auto scheduled = now_ns();
    const aa::core::Nanoseconds receipt{scheduled.count - 75'000'000};
    const auto pcm = constant_pcm({kMediaLevel, 960});
    ASSERT_TRUE(router.value().submit(
        make_message(aa::protocol::ChannelRole::media_audio, 42, scheduled, pcm), receipt));
    ASSERT_TRUE(router.value().pump());
    ASSERT_TRUE(completion);
    EXPECT_EQ(completion->scheduled, scheduled);
    EXPECT_EQ(observed->write_ns.front(), scheduled.count);
    EXPECT_EQ(router.value().report().stats.last_path_latency.count,
              completion->completed.count - receipt.count);
    EXPECT_GE(router.value().report().stats.last_path_latency.count, 75'000'000);
    EXPECT_LT(router.value().report().stats.last_completion_skew.count,
              router.value().report().stats.last_path_latency.count);
}

// #60: the completion carries when the chunk starts playing, max(handoff,
// the sink's playback slot); a sink without a position reports none.
TEST_F(AudioPipelineRoute, CompletionPresentedIsLaterOfHandoffAndPlaybackSlot) {
    const auto scheduled = now_ns();
    const auto run = [&](std::optional<aa::core::Nanoseconds> slot) {
        auto sink = std::make_unique<RecordingAudioSink>();
        sink->state->playback_slot = slot;
        auto config = pcm_router_config();
        std::optional<aa::audio::WriteCompletion> completion;
        config.on_write_completed = [&](const auto& value) { completion = value; };
        auto router = aa::audio::detail::open_router_with_sink_for_test(config, std::move(sink));
        EXPECT_TRUE(router);
        const auto pcm = constant_pcm({kMediaLevel, 960});
        EXPECT_TRUE(router.value().submit(
            make_message(aa::protocol::ChannelRole::media_audio, 42, scheduled, pcm), scheduled));
        EXPECT_TRUE(router.value().pump());
        EXPECT_TRUE(completion);
        return completion.value_or(aa::audio::WriteCompletion{});
    };
    // Committed ahead of playback (pulsesink): presented is the slot.
    const aa::core::Nanoseconds later{scheduled.count + 3'000'000'000};
    const auto early = run(later);
    ASSERT_TRUE(early.presented);
    EXPECT_LT(early.completed.count, later.count);
    EXPECT_EQ(early.presented->count, later.count);
    // Handed off after its slot (a stalled sink): presented is the handoff,
    // so lateness is never hidden.
    const auto late = run(aa::core::Nanoseconds{scheduled.count - 3'000'000'000});
    ASSERT_TRUE(late.presented);
    EXPECT_EQ(late.presented->count, late.completed.count);
    // No position reported: no presented time is invented.
    EXPECT_FALSE(run(std::nullopt).presented);
}

TEST_F(AudioPipelineRoute, CompletionPreservesEveryPlaybackSlotAbsenceReason) {
    using Reason = aa::audio::PlaybackSlotAbsence;
    for (const auto reason : {Reason::unavailable, Reason::worker_busy, Reason::worker_stopped,
                             Reason::deadline_missed, Reason::query_failed,
                             Reason::negative_position, Reason::invalid_pts}) {
        for (const bool queued : {false, true}) {
            SCOPED_TRACE(std::string{aa::audio::to_string(reason)});
            SCOPED_TRACE(queued);
            auto sink = std::make_unique<RecordingAudioSink>();
            sink->state->slot_absence = reason;
            auto config = pcm_router_config();
            std::optional<aa::audio::WriteCompletion> completion;
            config.on_write_completed = [&](const auto& value) { completion = value; };
            auto opened = aa::audio::detail::open_router_with_sink_for_test(config, std::move(sink));
            ASSERT_TRUE(opened);
            auto& router = opened.value();
            const auto pcm = constant_pcm({kMediaLevel, 960});
            ASSERT_TRUE(router.submit(make_message(aa::protocol::ChannelRole::media_audio,
                                                   1, now_ns(), pcm)));
            if (queued) {
                ASSERT_TRUE(router.render_step());
            } else {
                ASSERT_TRUE(router.pump());
            }
            ASSERT_TRUE(completion);
            EXPECT_FALSE(completion->presented);
            EXPECT_EQ(completion->playback_slot_absence, reason);
        }
    }
}

TEST_F(AudioPipelineRoute, PendingRunRejectionRecordsBoundedOccupancyUntilNextSubmitOrClose) {
    auto sink = std::make_unique<RecordingAudioSink>();
    const auto observed = sink->state;
    auto router = aa::audio::detail::open_router_with_sink_for_test(pcm_router_config(), std::move(sink));
    ASSERT_TRUE(router);
    const auto role = aa::protocol::ChannelRole::media_audio;
    const std::vector<std::byte> pcm(4, std::byte{1});
    const auto message = make_message(role, 1, now_ns(), pcm);
    for (int i = 0; i < 512; ++i) { ASSERT_TRUE(router.value().submit(message)); }
    const auto rejected = router.value().submit(message);
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().code(), aa::ErrorCode::ipc_queue_full);
    const auto snapshot = router.value().report().queue_rejection;
    ASSERT_TRUE(snapshot);
    EXPECT_EQ(snapshot->source, aa::diagnostics::QueueSource::audio_router);
    EXPECT_EQ(snapshot->reason, aa::diagnostics::QueueReason::pending_runs);
    EXPECT_EQ(snapshot->role, role);
    EXPECT_EQ(snapshot->items, 512U);
    EXPECT_EQ(snapshot->capacity, 512U);
    EXPECT_EQ(snapshot->requested_bytes, 4U);
    EXPECT_EQ(router.value().report().stats.chunks_submitted, 512U);
    EXPECT_TRUE(observed->writes.empty());
    router.value().close();
    EXPECT_EQ(router.value().report().queue_rejection, snapshot);
    const auto closed = router.value().submit(message);
    ASSERT_FALSE(closed);
    EXPECT_EQ(closed.error().code(), aa::ErrorCode::invalid_argument);
    EXPECT_FALSE(router.value().report().queue_rejection);
}

TEST_F(AudioPipelineRoute, MultiPumpPreservesTimestampGap) {
    const auto config = pcm_router_config();
    const std::int64_t rate = static_cast<std::int64_t>(config.output_format.sample_rate_hz);
    const std::size_t ch = config.output_format.channels;
    auto sink = std::make_unique<RecordingAudioSink>();
    auto rec = sink->state;
    auto router =
        aa::audio::detail::open_router_with_sink_for_test(config, std::move(sink));
    ASSERT_TRUE(router.has_value());

    const aa::core::Nanoseconds base{1'000'000'000LL};
    const std::vector<std::byte> media = constant_pcm({kMediaLevel, 960});
    const std::vector<std::byte> guidance = constant_pcm({kGuidanceLevel, 960});
    ASSERT_TRUE(router.value()
                    .submit(make_message(aa::protocol::ChannelRole::media_audio, 0, base, media))
                    .has_value());
    ASSERT_TRUE(router.value().pump().has_value());
    ASSERT_TRUE(router.value()
                    .submit(make_message(aa::protocol::ChannelRole::guidance_audio, 1,
                                         aa::core::Nanoseconds{base.count + 30'000'000LL},
                                         guidance))
                    .has_value());
    ASSERT_TRUE(router.value().pump().has_value());

    const std::int64_t silence_frames = 30'000'000LL * rate / 1'000'000'000LL - 960;
    ASSERT_GT(silence_frames, 0);
    const auto flat = flatten_i16(rec->writes);
    ASSERT_EQ(flat.size(), static_cast<std::size_t>(1920 + silence_frames) * ch);
    for (std::size_t i = 0; i < 960 * ch; ++i) {
        ASSERT_EQ(flat[i], kMediaLevel) << i;
    }
    for (std::size_t i = 960 * ch; i < static_cast<std::size_t>(960 + silence_frames) * ch; ++i) {
        ASSERT_EQ(flat[i], 0) << i;
    }
    for (std::size_t i = static_cast<std::size_t>(960 + silence_frames) * ch; i < flat.size(); ++i) {
        ASSERT_EQ(flat[i], kGuidanceLevel) << i;
    }
    ASSERT_FALSE(rec->write_ns.empty());
    EXPECT_EQ(rec->write_ns.back(), base.count + 30'000'000LL);
}

TEST_F(AudioPipelineRoute, MultiPumpMatchesSinglePumpNonOverlappingTimeline) {
    const auto config = pcm_router_config();
    const std::vector<std::byte> media = constant_pcm({kMediaLevel, 960});
    const std::vector<std::byte> guidance = constant_pcm({kGuidanceLevel, 960});
    const aa::core::Nanoseconds base{2'000'000'000LL};
    const aa::core::Nanoseconds second{base.count + 30'000'000LL};

    auto sink_a = std::make_unique<RecordingAudioSink>();
    auto rec_a = sink_a->state;
    auto batched =
        aa::audio::detail::open_router_with_sink_for_test(config, std::move(sink_a));
    ASSERT_TRUE(batched.has_value());
    ASSERT_TRUE(batched.value()
                    .submit(make_message(aa::protocol::ChannelRole::media_audio, 0, base, media))
                    .has_value());
    ASSERT_TRUE(batched.value()
                    .submit(
                        make_message(aa::protocol::ChannelRole::guidance_audio, 1, second, guidance))
                    .has_value());
    ASSERT_TRUE(batched.value().pump().has_value());

    auto sink_b = std::make_unique<RecordingAudioSink>();
    auto rec_b = sink_b->state;
    auto split = aa::audio::detail::open_router_with_sink_for_test(config, std::move(sink_b));
    ASSERT_TRUE(split.has_value());
    ASSERT_TRUE(split.value()
                    .submit(make_message(aa::protocol::ChannelRole::media_audio, 0, base, media))
                    .has_value());
    ASSERT_TRUE(split.value().pump().has_value());
    ASSERT_TRUE(split.value()
                    .submit(
                        make_message(aa::protocol::ChannelRole::guidance_audio, 1, second, guidance))
                    .has_value());
    ASSERT_TRUE(split.value().pump().has_value());

    EXPECT_EQ(flatten_i16(rec_a->writes), flatten_i16(rec_b->writes));
    EXPECT_EQ(rec_a->write_ns, rec_b->write_ns);
}

TEST_F(AudioPipelineRoute, MultiPumpContiguousRunsDoNotReplayOrInsertSilence) {
    const auto config = pcm_router_config();
    const std::int64_t rate = static_cast<std::int64_t>(config.output_format.sample_rate_hz);
    const std::size_t ch = config.output_format.channels;
    auto sink = std::make_unique<RecordingAudioSink>();
    auto rec = sink->state;
    auto router = aa::audio::detail::open_router_with_sink_for_test(config, std::move(sink));
    ASSERT_TRUE(router.has_value());

    const aa::core::Nanoseconds base{3'000'000'000LL};
    const aa::core::Nanoseconds next{base.count + 960 * 1'000'000'000LL / rate};
    const std::vector<std::byte> first = constant_pcm({kMediaLevel, 960});
    const std::vector<std::byte> second = constant_pcm({3000, 960});
    ASSERT_TRUE(router.value()
                    .submit(make_message(aa::protocol::ChannelRole::media_audio, 0, base, first))
                    .has_value());
    ASSERT_TRUE(router.value().pump().has_value());
    ASSERT_TRUE(router.value()
                    .submit(make_message(aa::protocol::ChannelRole::media_audio, 1, next, second))
                    .has_value());
    ASSERT_TRUE(router.value().pump().has_value());

    const auto flat = flatten_i16(rec->writes);
    ASSERT_EQ(flat.size(), 1920U * ch);
    for (std::size_t i = 0; i < 960 * ch; ++i) {
        ASSERT_EQ(flat[i], kMediaLevel) << i;
    }
    for (std::size_t i = 960 * ch; i < 1920 * ch; ++i) {
        ASSERT_EQ(flat[i], 3000) << i;
    }
}

TEST_F(AudioPipelineRoute, MultiPumpLateRunsSequentializeWithoutDroppingSamples) {
    const auto config = pcm_router_config();
    const std::size_t ch = config.output_format.channels;
    auto sink = std::make_unique<RecordingAudioSink>();
    auto rec = sink->state;
    auto router = aa::audio::detail::open_router_with_sink_for_test(config, std::move(sink));
    ASSERT_TRUE(router.has_value());

    const aa::core::Nanoseconds base{4'000'000'000LL};
    const std::vector<std::byte> first = constant_pcm({kMediaLevel, 960});
    const std::vector<std::byte> late = constant_pcm({3000, 960});
    ASSERT_TRUE(router.value()
                    .submit(make_message(aa::protocol::ChannelRole::media_audio, 0, base, first))
                    .has_value());
    ASSERT_TRUE(router.value().pump().has_value());
    ASSERT_TRUE(router.value()
                    .submit(make_message(aa::protocol::ChannelRole::media_audio, 1, base, late))
                    .has_value());
    ASSERT_TRUE(router.value().pump().has_value());

    const auto flat = flatten_i16(rec->writes);
    ASSERT_EQ(flat.size(), 1920U * ch);
    for (std::size_t i = 0; i < 960 * ch; ++i) {
        ASSERT_EQ(flat[i], kMediaLevel) << i;
    }
    for (std::size_t i = 960 * ch; i < 1920 * ch; ++i) {
        ASSERT_EQ(flat[i], 3000) << i;
    }
    for (std::size_t i = 1; i < rec->write_ns.size(); ++i) {
        ASSERT_GE(rec->write_ns[i], rec->write_ns[i - 1]) << i;
    }
}

TEST_F(AudioPipelineRoute, DriftPolicyTrimMathMatchesWatermarkTargetAndCaps) {
    aa::audio::LivePcmDriftPolicy policy;
    policy.enabled = true;
    const aa::audio::LivePcmDriftCorrector corrector{policy, 48000};
    EXPECT_EQ(corrector.trim_frames(aa::core::Nanoseconds{0}, 960), 0U);
    EXPECT_EQ(corrector.trim_frames(aa::core::Nanoseconds{20'000'000LL}, 960), 0U);
    EXPECT_EQ(corrector.trim_frames(aa::core::Nanoseconds{21'000'000LL}, 960), 240U);
    EXPECT_EQ(corrector.trim_frames(aa::core::Nanoseconds{30'000'000LL}, 960), 240U);
    EXPECT_EQ(corrector.trim_frames(aa::core::Nanoseconds{16'000'000LL}, 960), 0U);
    EXPECT_EQ(corrector.trim_frames(aa::core::Nanoseconds{-5'000'000LL}, 960), 0U);
    EXPECT_EQ(corrector.trim_frames(aa::core::Nanoseconds{30'000'000LL}, 100), 100U);
    const aa::audio::LivePcmDriftCorrector at_96k{policy, 96000};
    EXPECT_EQ(at_96k.trim_frames(aa::core::Nanoseconds{21'000'000LL}, 960), 480U);
    aa::audio::LivePcmDriftPolicy custom;
    custom.enabled = true;
    custom.high_watermark = aa::core::Nanoseconds{10'000'000LL};
    custom.target = aa::core::Nanoseconds{8'000'000LL};
    custom.max_trim = aa::core::Nanoseconds{1'000'000LL};
    const aa::audio::LivePcmDriftCorrector custom_corrector{custom, 48000};
    EXPECT_EQ(custom_corrector.trim_frames(aa::core::Nanoseconds{11'000'000LL}, 960), 48U);
    aa::audio::LivePcmDriftPolicy lossless;
    lossless.high_watermark = aa::core::Nanoseconds{1};
    const aa::audio::LivePcmDriftCorrector disabled{lossless, 48000};
    EXPECT_EQ(disabled.trim_frames(aa::core::Nanoseconds{30'000'000LL}, 960), 0U);
}

namespace {

constexpr std::uint64_t kSimChunkFrames = 960;
constexpr std::int64_t kSimChunkNs = 20'000'000LL;
constexpr std::uint64_t kSimRate = 48000;
constexpr std::uint64_t kSimQueueSlots = 4;

struct DriftSim final {
    std::uint64_t produced{};
    std::uint64_t admitted{};
    std::uint64_t acked{};
    std::uint64_t rejected{};
    std::uint64_t decoded{};
    std::uint64_t retained{};
    std::uint64_t trimmed{};
    std::uint64_t pending{};
    std::uint64_t silence{};
    std::uint64_t output{};
    std::uint64_t trims{};
    std::uint64_t rendered{};
    std::uint64_t max_unacked{};
    std::uint64_t max_queue_slots{};
    std::uint64_t max_queue_frames{};
    std::int64_t max_phase_ns{};
    bool cursor_monotonic{true};
    bool accounting_holds{true};
};

[[nodiscard]] std::uint64_t sim_intended(std::int64_t timestamp_ns) {
    const auto t = static_cast<std::uint64_t>(timestamp_ns);
    return (t / 1'000'000'000ULL) * kSimRate + ((t % 1'000'000'000ULL) * kSimRate) / 1'000'000'000ULL;
}

[[nodiscard]] std::int64_t sim_frame_time(std::uint64_t frame) {
    return static_cast<std::int64_t>((frame / kSimRate) * 1'000'000'000ULL
                                     + ((frame % kSimRate) * 1'000'000'000ULL) / kSimRate);
}

// Virtual-clock model of phone -> admission queue -> router: chunks of
// kSimChunkFrames frames are produced every kSimChunkNs/(1+ppm) of the OUTPUT
// clock and stamped with the production time (the common monotonic clock);
// each chunk is admitted and credited immediately (ACK-on-admission) into the
// bounded queue and rendered FIFO at the output rate while the router policy
// trims over-watermark runs. No PCM is materialized - only counters and
// virtual time. The exact rational production spacing keeps the run
// deterministic.
[[nodiscard]] DriftSim run_drift_sim(std::int64_t ppm, // NOLINT(bugprone-easily-swappable-parameters): virtual clock drift in ppm and a chunk count are distinct units
                                     std::uint64_t chunks) {
    aa::audio::LivePcmDriftPolicy policy;
    policy.enabled = true;
    const aa::audio::LivePcmDriftCorrector corrector{policy, 48000};
    DriftSim sim;
    const std::int64_t denominator = 1'000'000LL + ppm;
    std::int64_t remainder = 0;
    std::int64_t produced_at_ns = 0;
    std::uint64_t role_end = 0;
    std::int64_t previous_render_done_ns = 0;
    std::deque<std::int64_t> render_done;
    for (std::uint64_t n = 0; n < chunks; ++n) {
        ++sim.produced;
        if (sim.produced - sim.acked > sim.max_unacked) {
            sim.max_unacked = sim.produced - sim.acked;
        }
        while (!render_done.empty() && render_done.front() <= produced_at_ns) {
            render_done.pop_front();
        }
        if (render_done.size() + 1 > sim.max_queue_slots) {
            sim.max_queue_slots = render_done.size() + 1;
        }
        const std::uint64_t queue_frames =
            static_cast<std::uint64_t>(render_done.size() + 1) * kSimChunkFrames;
        if (queue_frames > sim.max_queue_frames) {
            sim.max_queue_frames = queue_frames;
        }
        if (render_done.size() + 1 > kSimQueueSlots) {
            ++sim.rejected;
        } else {
            ++sim.admitted;
            ++sim.acked;
            sim.decoded += kSimChunkFrames;
            sim.pending += kSimChunkFrames;

            const std::uint64_t intended = sim_intended(produced_at_ns);
            const std::uint64_t placed = std::max(intended, role_end);
            const std::int64_t phase = sim_frame_time(placed) - produced_at_ns;
            if (phase > sim.max_phase_ns) {
                sim.max_phase_ns = phase;
            }
            const std::uint64_t trim = corrector.trim_frames(aa::core::Nanoseconds{phase},
                                                             kSimChunkFrames);
            if (trim != 0) {
                ++sim.trims;
            }
            const std::uint64_t retained = kSimChunkFrames - trim;
            const std::uint64_t gap = placed - role_end;
            if (placed + retained < placed || placed < role_end) {
                sim.cursor_monotonic = false;
            }
            role_end = placed + retained;
            sim.retained += retained;
            sim.trimmed += trim;
            sim.pending -= kSimChunkFrames;
            sim.silence += gap;
            sim.output += gap + retained;
            if (sim.decoded != sim.retained + sim.trimmed + sim.pending) {
                sim.accounting_holds = false;
            }

            const std::int64_t render_start_ns =
                std::max<std::int64_t>(produced_at_ns, previous_render_done_ns);
            previous_render_done_ns = render_start_ns + sim_frame_time(gap + retained);
            render_done.push_back(previous_render_done_ns);
            ++sim.rendered;
        }
        remainder += kSimChunkNs * 1'000'000LL;
        produced_at_ns += remainder / denominator;
        remainder %= denominator;
    }
    return sim;
}

} // namespace

TEST_F(AudioPipelineRoute, LivePcmLongRunDriftAtPlus200PpmStaysBounded) {
    const auto sim = run_drift_sim(200, 3ULL * 3600ULL * 50ULL);
    EXPECT_EQ(sim.rejected, 0U) << "queue-full over simulated hours";
    EXPECT_EQ(sim.produced, 3ULL * 3600ULL * 50ULL);
    EXPECT_EQ(sim.acked, sim.produced);
    EXPECT_LE(sim.max_unacked, 1U);
    EXPECT_LT(sim.max_queue_slots, kSimQueueSlots);
    EXPECT_LT(sim.max_queue_frames, kSimQueueSlots * kSimChunkFrames);
    EXPECT_GT(sim.trims, 0U);
    EXPECT_LT(sim.max_phase_ns, 21'000'000LL);
    EXPECT_TRUE(sim.cursor_monotonic);
    EXPECT_TRUE(sim.accounting_holds);
    EXPECT_EQ(sim.decoded, sim.retained + sim.trimmed + sim.pending);
    EXPECT_EQ(sim.pending, 0U);
}

TEST_F(AudioPipelineRoute, LivePcmLongRunZeroDriftNeverTrims) {
    const auto sim = run_drift_sim(0, 3ULL * 3600ULL * 50ULL);
    EXPECT_EQ(sim.rejected, 0U);
    EXPECT_EQ(sim.trims, 0U);
    EXPECT_EQ(sim.trimmed, 0U);
    EXPECT_EQ(sim.silence, 0U);
    EXPECT_EQ(sim.output, sim.decoded);
    EXPECT_EQ(sim.max_phase_ns, 0);
    EXPECT_EQ(sim.acked, sim.produced);
    EXPECT_TRUE(sim.accounting_holds);
    EXPECT_EQ(sim.decoded, sim.retained + sim.trimmed + sim.pending);
}

TEST_F(AudioPipelineRoute, LivePcmLongRunMinus200PpmKeepsBoundedSilence) {
    const auto chunks = 3ULL * 3600ULL * 50ULL;
    const auto sim = run_drift_sim(-200, chunks);
    EXPECT_EQ(sim.rejected, 0U);
    EXPECT_EQ(sim.trims, 0U);
    EXPECT_EQ(sim.trimmed, 0U);
    EXPECT_LT(sim.max_phase_ns, 20'834LL * 2);
    EXPECT_GT(sim.silence, 0U);
    EXPECT_LT(sim.silence, 2U * chunks);
    EXPECT_EQ(sim.output, sim.retained + sim.silence);
    EXPECT_EQ(sim.acked, sim.produced);
    EXPECT_TRUE(sim.cursor_monotonic);
    EXPECT_TRUE(sim.accounting_holds);
    EXPECT_EQ(sim.decoded, sim.retained + sim.trimmed + sim.pending);
}

TEST_F(AudioPipelineRoute, LivePcmDriftTrimAccountsFramesAndKeepsMonotonicTimeline) {
    auto config = pcm_router_config();
    config.live_pcm_drift.enabled = true;
    auto sink = std::make_unique<RecordingAudioSink>();
    auto rec = sink->state;
    auto router = aa::audio::detail::open_router_with_sink_for_test(config, std::move(sink));
    ASSERT_TRUE(router.has_value());

    const aa::core::Nanoseconds base{3'500'000'000LL};
    const std::vector<std::byte> media = constant_pcm({kMediaLevel, 960});
    for (std::uint64_t n = 0; n < 15; ++n) {
        const aa::core::Nanoseconds stamp{
            base.count + static_cast<std::int64_t>(n) * 18'000'000LL};
        ASSERT_TRUE(router.value()
                        .submit(make_message(aa::protocol::ChannelRole::media_audio, n, stamp,
                                             media))
                        .has_value());
        ASSERT_TRUE(router.value().pump().has_value());
    }

    const auto& stats = router.value().report().stats;
    EXPECT_EQ(stats.frames_decoded, 15U * 960U);
    EXPECT_EQ(stats.frames_trimmed, 480U);
    EXPECT_EQ(stats.frames_retained, 13'920U);
    EXPECT_EQ(stats.frames_silence, 0U);
    EXPECT_EQ(stats.frames_output, 13'920U);
    EXPECT_EQ(stats.frames_decoded, stats.frames_retained + stats.frames_trimmed);
    EXPECT_EQ(flatten_i16(rec->writes).size(), 13'920U * 2U);
    ASSERT_EQ(rec->write_ns.size(), 15U);
    EXPECT_EQ(rec->write_ns.front(), base.count);
    EXPECT_EQ(rec->write_ns.back(), base.count + 270'000'000LL);
    for (std::size_t i = 1; i < rec->write_ns.size(); ++i) {
        ASSERT_GT(rec->write_ns[i], rec->write_ns[i - 1]) << i;
    }
}

TEST_F(AudioPipelineRoute, LivePcmZeroDriftIntegrationTrimsNothing) {
    auto config = pcm_router_config();
    config.live_pcm_drift.enabled = true;
    auto sink = std::make_unique<RecordingAudioSink>();
    auto rec = sink->state;
    auto router = aa::audio::detail::open_router_with_sink_for_test(config, std::move(sink));
    ASSERT_TRUE(router.has_value());

    const aa::core::Nanoseconds base{4'500'000'000LL};
    const std::vector<std::byte> media = constant_pcm({kMediaLevel, 960});
    for (std::uint64_t n = 0; n < 15; ++n) {
        const aa::core::Nanoseconds stamp{
            base.count + static_cast<std::int64_t>(n) * 20'000'000LL};
        ASSERT_TRUE(router.value()
                        .submit(make_message(aa::protocol::ChannelRole::media_audio, n, stamp,
                                             media))
                        .has_value());
        ASSERT_TRUE(router.value().pump().has_value());
    }

    const auto& stats = router.value().report().stats;
    EXPECT_EQ(stats.frames_decoded, 15U * 960U);
    EXPECT_EQ(stats.frames_trimmed, 0U);
    EXPECT_EQ(stats.frames_retained, 15U * 960U);
    EXPECT_EQ(stats.frames_silence, 0U);
    EXPECT_EQ(stats.frames_output, 15U * 960U);
    EXPECT_EQ(flatten_i16(rec->writes).size(), 15U * 960U * 2U);
}

TEST_F(AudioPipelineRoute, EmptyPumpDoesNotRebaseTimeline) {
    const auto config = pcm_router_config();
    auto sink = std::make_unique<RecordingAudioSink>();
    auto rec = sink->state;
    auto router = aa::audio::detail::open_router_with_sink_for_test(config, std::move(sink));
    ASSERT_TRUE(router.has_value());

    const aa::core::Nanoseconds base{5'000'000'000LL};
    const std::vector<std::byte> media = constant_pcm({kMediaLevel, 960});
    const std::vector<std::byte> guidance = constant_pcm({kGuidanceLevel, 960});
    ASSERT_TRUE(router.value()
                    .submit(make_message(aa::protocol::ChannelRole::media_audio, 0, base, media))
                    .has_value());
    ASSERT_TRUE(router.value().pump().has_value());
    ASSERT_TRUE(router.value().pump().has_value());

    ASSERT_TRUE(router.value()
                    .submit(make_message(aa::protocol::ChannelRole::guidance_audio, 1,
                                         aa::core::Nanoseconds{base.count + 30'000'000LL},
                                         guidance))
                    .has_value());
    ASSERT_TRUE(router.value().pump().has_value());

    ASSERT_FALSE(rec->writes.empty());
    EXPECT_EQ(rec->write_ns.back(), base.count + 30'000'000LL);
    const auto flat = flatten_i16(rec->writes);
    ASSERT_GT(flat.size(), 1920U);
}

TEST_F(AudioPipelineRoute, LargeTimestampGapRejectedBeforeAnyWrite) {
    const auto config = pcm_router_config();
    auto sink = std::make_unique<RecordingAudioSink>();
    auto rec = sink->state;
    auto router = aa::audio::detail::open_router_with_sink_for_test(config, std::move(sink));
    ASSERT_TRUE(router.has_value());

    const aa::core::Nanoseconds base{6'000'000'000LL};
    const std::vector<std::byte> media = constant_pcm({kMediaLevel, 960});
    ASSERT_TRUE(router.value()
                    .submit(make_message(aa::protocol::ChannelRole::media_audio, 0, base, media))
                    .has_value());
    ASSERT_TRUE(router.value()
                    .submit(make_message(aa::protocol::ChannelRole::media_audio, 1,
                                         aa::core::Nanoseconds{base.count + 3'600'000'000'000LL},
                                         media))
                    .has_value());
    auto pumped = router.value().pump();
    ASSERT_FALSE(pumped.has_value());
    EXPECT_EQ(pumped.error().code(), aa::ErrorCode::invalid_argument);
    EXPECT_TRUE(rec->writes.empty());
}

TEST_F(AudioPipelineRoute, MultiPumpLargeGapRejectedWithoutAdvancingCursor) {
    const auto config = pcm_router_config();
    const std::int64_t rate = static_cast<std::int64_t>(config.output_format.sample_rate_hz);
    const std::size_t ch = config.output_format.channels;
    auto sink = std::make_unique<RecordingAudioSink>();
    auto rec = sink->state;
    auto router = aa::audio::detail::open_router_with_sink_for_test(config, std::move(sink));
    ASSERT_TRUE(router.has_value());

    const aa::core::Nanoseconds base{7'000'000'000LL};
    const std::vector<std::byte> media = constant_pcm({kMediaLevel, 960});
    ASSERT_TRUE(router.value()
                    .submit(make_message(aa::protocol::ChannelRole::media_audio, 0, base, media))
                    .has_value());
    ASSERT_TRUE(router.value().pump().has_value());
    ASSERT_EQ(flatten_i16(rec->writes).size(), 960U * ch);

    ASSERT_TRUE(router.value()
                    .submit(make_message(aa::protocol::ChannelRole::media_audio, 1,
                                         aa::core::Nanoseconds{base.count + 3'600'000'000'000LL},
                                         media))
                    .has_value());
    auto rejected = router.value().pump();
    ASSERT_FALSE(rejected.has_value());
    EXPECT_EQ(rejected.error().code(), aa::ErrorCode::invalid_argument);
    EXPECT_EQ(flatten_i16(rec->writes).size(), 960U * ch);

    const std::vector<std::byte> tail = constant_pcm({3000, 960});
    const aa::core::Nanoseconds contiguous{base.count + 960 * 1'000'000'000LL / rate};
    ASSERT_TRUE(router.value()
                    .submit(make_message(aa::protocol::ChannelRole::media_audio, 2, contiguous, tail))
                    .has_value());
    ASSERT_TRUE(router.value().pump().has_value());
    const auto flat = flatten_i16(rec->writes);
    ASSERT_EQ(flat.size(), 1920U * ch);
    EXPECT_EQ(flat[1920 * ch - 1], 3000);
    EXPECT_EQ(rec->write_ns.back(), contiguous.count);
}

TEST_F(AudioPipelineRoute, GapAtCapAcceptedAndAboveCapRejected) {
    const auto config = pcm_router_config();
    const std::int64_t rate = static_cast<std::int64_t>(config.output_format.sample_rate_hz);
    const std::size_t ch = config.output_format.channels;
    const std::int64_t cap_frames = rate / 5;
    const std::vector<std::byte> media = constant_pcm({kMediaLevel, 960});
    const std::vector<std::byte> guidance = constant_pcm({kGuidanceLevel, 960});

    auto sink_ok = std::make_unique<RecordingAudioSink>();
    auto rec_ok = sink_ok->state;
    auto accepted =
        aa::audio::detail::open_router_with_sink_for_test(config, std::move(sink_ok));
    ASSERT_TRUE(accepted.has_value());
    const aa::core::Nanoseconds base{8'000'000'000LL};
    ASSERT_TRUE(accepted.value()
                    .submit(make_message(aa::protocol::ChannelRole::media_audio, 0, base, media))
                    .has_value());
    ASSERT_TRUE(accepted.value().pump().has_value());
    const aa::core::Nanoseconds at_cap{
        base.count + (960 + cap_frames) * 1'000'000'000LL / rate};
    ASSERT_TRUE(accepted.value()
                    .submit(make_message(aa::protocol::ChannelRole::guidance_audio, 1, at_cap,
                                         guidance))
                    .has_value());
    ASSERT_TRUE(accepted.value().pump().has_value());
    const std::int64_t at_cap_ns = (960 + cap_frames) * 1'000'000'000LL / rate;
    const std::int64_t at_cap_frames = at_cap_ns * rate / 1'000'000'000LL;
    EXPECT_EQ(flatten_i16(rec_ok->writes).size(),
              static_cast<std::size_t>(960 + (at_cap_frames - 960) + 960) * ch);

    auto sink_bad = std::make_unique<RecordingAudioSink>();
    auto rec_bad = sink_bad->state;
    auto rejected = aa::audio::detail::open_router_with_sink_for_test(config, std::move(sink_bad));
    ASSERT_TRUE(rejected.has_value());
    ASSERT_TRUE(rejected.value()
                    .submit(make_message(aa::protocol::ChannelRole::media_audio, 0, base, media))
                    .has_value());
    ASSERT_TRUE(rejected.value().pump().has_value());
    const aa::core::Nanoseconds past_cap{
        base.count
        + ((960 + cap_frames + 1) * 1'000'000'000LL + rate - 1) / rate};
    ASSERT_TRUE(rejected.value()
                    .submit(make_message(aa::protocol::ChannelRole::guidance_audio, 1, past_cap,
                                         guidance))
                    .has_value());
    auto over = rejected.value().pump();
    ASSERT_FALSE(over.has_value());
    EXPECT_EQ(over.error().code(), aa::ErrorCode::invalid_argument);
    EXPECT_EQ(flatten_i16(rec_bad->writes).size(), 960U * ch);
}

TEST_F(AudioPipelineRoute, TotalGapBudgetPreflightIsAtomic) {
    const auto config = pcm_router_config();
    const std::int64_t rate = static_cast<std::int64_t>(config.output_format.sample_rate_hz);
    const std::int64_t gap_frames = 3 * rate / 20;
    auto sink = std::make_unique<RecordingAudioSink>();
    auto rec = sink->state;
    auto router = aa::audio::detail::open_router_with_sink_for_test(config, std::move(sink));
    ASSERT_TRUE(router.has_value());

    const aa::core::Nanoseconds base{9'000'000'000LL};
    const std::vector<std::byte> chunk = constant_pcm({kMediaLevel, 960});
    ASSERT_TRUE(router.value()
                    .submit(make_message(aa::protocol::ChannelRole::media_audio, 0, base, chunk))
                    .has_value());
    const aa::core::Nanoseconds second{
        base.count + (960 + gap_frames) * 1'000'000'000LL / rate};
    ASSERT_TRUE(router.value()
                    .submit(
                        make_message(aa::protocol::ChannelRole::guidance_audio, 1, second, chunk))
                    .has_value());
    const aa::core::Nanoseconds third{
        second.count + (960 + gap_frames) * 1'000'000'000LL / rate};
    ASSERT_TRUE(router.value()
                    .submit(make_message(aa::protocol::ChannelRole::speech_audio, 2, third, chunk))
                    .has_value());

    auto pumped = router.value().pump();
    ASSERT_FALSE(pumped.has_value());
    EXPECT_EQ(pumped.error().code(), aa::ErrorCode::invalid_argument);
    EXPECT_TRUE(rec->writes.empty());
}

TEST_F(AudioPipelineRoute, TimestampRangeAndEndOverflowFailTyped) {
    const auto config = pcm_router_config();
    const std::vector<std::byte> media = constant_pcm({kMediaLevel, 960});

    auto sink_neg = std::make_unique<RecordingAudioSink>();
    auto rec_neg = sink_neg->state;
    auto negative =
        aa::audio::detail::open_router_with_sink_for_test(config, std::move(sink_neg));
    ASSERT_TRUE(negative.has_value());
    auto denied = negative.value().submit(
        make_message(aa::protocol::ChannelRole::media_audio, 0, aa::core::Nanoseconds{-1}, media));
    ASSERT_FALSE(denied.has_value());
    EXPECT_EQ(denied.error().code(), aa::ErrorCode::invalid_argument);

    auto sink_end = std::make_unique<RecordingAudioSink>();
    auto rec_end = sink_end->state;
    auto overflow =
        aa::audio::detail::open_router_with_sink_for_test(config, std::move(sink_end));
    ASSERT_TRUE(overflow.has_value());
    const aa::core::Nanoseconds near_max{INT64_MAX - 5'000'000LL};
    ASSERT_TRUE(overflow.value()
                    .submit(make_message(aa::protocol::ChannelRole::media_audio, 0, near_max, media))
                    .has_value());
    auto rejected = overflow.value().pump();
    ASSERT_FALSE(rejected.has_value());
    EXPECT_EQ(rejected.error().code(), aa::ErrorCode::invalid_argument);
    EXPECT_TRUE(rec_end->writes.empty());
    EXPECT_TRUE(rec_neg->writes.empty());

    // Large-displacement pair at 48 kHz: timestamps 0 and INT64_MAX-1000
    // reach frame 442721857769989, whose end-frame time conversion overflows
    // on the remainder ADDITION (the quotient product alone fits). The
    // conversion must fail instead of wrapping to a large negative delta, and
    // the whole batch is rejected atomically: not even the leading run at
    // timestamp 0 is written.
    auto sink_span = std::make_unique<RecordingAudioSink>();
    auto rec_span = sink_span->state;
    auto span_router =
        aa::audio::detail::open_router_with_sink_for_test(config, std::move(sink_span));
    ASSERT_TRUE(span_router.has_value());
    ASSERT_TRUE(span_router.value()
                    .submit(make_message(aa::protocol::ChannelRole::media_audio, 0,
                                         aa::core::Nanoseconds{0}, media))
                    .has_value());
    ASSERT_TRUE(span_router.value()
                    .submit(make_message(aa::protocol::ChannelRole::media_audio, 1,
                                         aa::core::Nanoseconds{INT64_MAX - 1000}, media))
                    .has_value());
    auto span_rejected = span_router.value().pump();
    ASSERT_FALSE(span_rejected.has_value());
    EXPECT_EQ(span_rejected.error().code(), aa::ErrorCode::invalid_argument);
    EXPECT_TRUE(rec_span->writes.empty());
    EXPECT_EQ(span_router.value().report().stats.frames_written, 0U);
}

TEST_F(AudioPipelineRoute, SilenceWritesRemainBounded) {
    const auto config = pcm_router_config();
    const std::size_t ch = config.output_format.channels;
    const std::int64_t rate = static_cast<std::int64_t>(config.output_format.sample_rate_hz);
    const std::size_t frame_bytes =
        config.output_format.channels * sizeof(std::int16_t);
    const std::int64_t gap_frames = 3 * rate / 20;
    auto sink = std::make_unique<RecordingAudioSink>();
    auto rec = sink->state;
    auto router = aa::audio::detail::open_router_with_sink_for_test(config, std::move(sink));
    ASSERT_TRUE(router.has_value());

    const aa::core::Nanoseconds base{10'000'000'000LL};
    const std::vector<std::byte> media = constant_pcm({kMediaLevel, 960});
    ASSERT_TRUE(router.value()
                    .submit(make_message(aa::protocol::ChannelRole::media_audio, 0, base, media))
                    .has_value());
    ASSERT_TRUE(router.value().pump().has_value());
    const aa::core::Nanoseconds later{
        base.count + (960 + gap_frames) * 1'000'000'000LL / rate};
    const std::vector<std::byte> guidance = constant_pcm({kGuidanceLevel, 960});
    ASSERT_TRUE(router.value()
                    .submit(
                        make_message(aa::protocol::ChannelRole::guidance_audio, 1, later, guidance))
                    .has_value());
    ASSERT_TRUE(router.value().pump().has_value());

    for (const auto& write : rec->writes) {
        EXPECT_LE(write.size(), std::size_t{64} * 1024);
        EXPECT_EQ(write.size() % frame_bytes, 0U);
    }
    const auto flat = flatten_i16(rec->writes);
    ASSERT_EQ(flat.size(), static_cast<std::size_t>(1920 + gap_frames) * ch);
    std::size_t zeros = 0;
    for (std::int16_t sample : flat) {
        if (sample == 0) {
            ++zeros;
        }
    }
    EXPECT_EQ(zeros, static_cast<std::size_t>(gap_frames) * ch);
}

TEST_F(AudioPipelineRoute, RawAacRoleRejectedAtOpen) {
    // Gate fix 5 (router codec contract): RoleStreamConfig carries no
    // AudioSpecificConfig and the decoder requires EncodedChunk::codec_data
    // for raw AAC-LC, so ANY role configured Codec::aac_lc is rejected at
    // open with audio_format_unsupported - before any decoder or sink is
    // opened. The recording-fake opener makes the rejection deterministic
    // with no live sink: open() itself must fail typed.
    for (std::size_t role_index = 0; role_index < 4U; ++role_index) {
        SCOPED_TRACE("role index " + std::to_string(role_index));
        auto config = pcm_router_config();
        config.roles[role_index].codec = aa::audio::Codec::aac_lc;
        auto router = aa::audio::detail::open_router_with_sink_for_test(
            config, std::make_unique<RecordingAudioSink>());
        ASSERT_FALSE(router.has_value());
        EXPECT_EQ(router.error().code(), aa::ErrorCode::audio_format_unsupported);
    }
}

TEST_F(AudioPipelineRoute, AacAdtsRoleRemainsSupported) {
    // Gate fix 5 regression guard: the raw-AAC rejection must NOT touch
    // AAC-LC ADTS. The existing ADTS fixture routes through the speech role
    // and the routed output is nonempty and non-silent whenever the decoder
    // backend is available (the standalone decoder's raw-AAC capability is
    // unchanged).
    auto config = pcm_router_config();
    config.roles[3].codec = aa::audio::Codec::aac_lc_adts;
    const auto decode_report = aa::audio::AudioDecoder::probe(aa::audio::Codec::aac_lc_adts);
    if (!decode_report.usable) {
        GTEST_SKIP() << "no viable AAC decoder: " << decode_report.cause;
    }
    auto sink = std::make_unique<RecordingAudioSink>();
    auto rec = sink->state;
    auto router = aa::audio::detail::open_router_with_sink_for_test(config, std::move(sink));
    ASSERT_TRUE(router.has_value());

    const std::vector<std::byte> stream =
        read_file(fixture_path("speech_aac_lc_adts_48k_stereo.aac"));
    ASSERT_FALSE(stream.empty());
    const auto message =
        make_message(aa::protocol::ChannelRole::speech_audio, 0, now_ns(), stream);
    ASSERT_TRUE(router.value().submit(message).has_value());
    ASSERT_TRUE(router.value().pump().has_value());

    const auto flat = flatten_i16(rec->writes);
    ASSERT_FALSE(flat.empty());
    bool non_silent = false;
    for (std::int16_t sample : flat) {
        if (sample != 0) {
            non_silent = true;
            break;
        }
    }
    EXPECT_TRUE(non_silent);
}

TEST_F(AudioPipelineRoute, SubmitAfterFlushRejectedBeforeDecode) {
    // Gate fix 6 (terminal flush state): after the terminal flush every
    // submit() is refused BEFORE decoding or sink access - no accepted-chunk
    // count change, no queued audio, no sink write. A one-byte payload that
    // would pass the gate and fail typed AFTER decode (no whole frame ->
    // audio_format_unsupported) instead fails at the terminal gate with
    // invalid_argument, proving the refusal runs before the decode path.
    const auto config = pcm_router_config();
    auto sink = std::make_unique<RecordingAudioSink>();
    auto rec = sink->state;
    auto router = aa::audio::detail::open_router_with_sink_for_test(config, std::move(sink));
    ASSERT_TRUE(router.has_value());

    const aa::core::Nanoseconds base{11'000'000'000LL};
    const std::vector<std::byte> media = constant_pcm({kMediaLevel, 960});
    ASSERT_TRUE(router.value()
                    .submit(make_message(aa::protocol::ChannelRole::media_audio, 0, base, media))
                    .has_value());
    ASSERT_TRUE(router.value().flush().has_value());

    const std::uint64_t chunks_before = router.value().report().stats.chunks_submitted;
    const std::uint64_t mixed_before = router.value().report().stats.frames_mixed;
    const std::uint64_t written_before = router.value().report().stats.frames_written;
    const std::size_t writes_before = rec->writes.size();
    ASSERT_EQ(chunks_before, 1U);
    ASSERT_EQ(writes_before, 1U);

    const std::vector<std::byte> one_byte{std::byte{0x11}};
    auto undecodable = router.value().submit(
        make_message(aa::protocol::ChannelRole::media_audio, 1, base, one_byte));
    ASSERT_FALSE(undecodable.has_value());
    EXPECT_EQ(undecodable.error().code(), aa::ErrorCode::invalid_argument);

    auto refused = router.value().submit(
        make_message(aa::protocol::ChannelRole::media_audio, 2, base, media));
    ASSERT_FALSE(refused.has_value());
    EXPECT_EQ(refused.error().code(), aa::ErrorCode::invalid_argument);

    // No accepted-chunk count change (submit() couples acceptance with
    // queueing), no queued audio advanced the mix/write path, no sink write.
    EXPECT_EQ(router.value().report().stats.chunks_submitted, chunks_before);
    EXPECT_EQ(router.value().report().stats.frames_mixed, mixed_before);
    EXPECT_EQ(router.value().report().stats.frames_written, written_before);
    EXPECT_EQ(rec->writes.size(), writes_before);
    router.value().close();
}

TEST_F(AudioPipelineRoute, PumpAfterFlushFailsTyped) {
    // Gate fix 6: after the terminal flush pump() fails typed
    // (invalid_argument) BEFORE any sink access - no write and no drain are
    // attempted and the refusal does not degrade the route.
    const auto config = pcm_router_config();
    auto sink = std::make_unique<RecordingAudioSink>();
    auto rec = sink->state;
    auto router = aa::audio::detail::open_router_with_sink_for_test(config, std::move(sink));
    ASSERT_TRUE(router.has_value());

    const aa::core::Nanoseconds base{12'000'000'000LL};
    const std::vector<std::byte> media = constant_pcm({kMediaLevel, 960});
    ASSERT_TRUE(router.value()
                    .submit(make_message(aa::protocol::ChannelRole::media_audio, 0, base, media))
                    .has_value());
    ASSERT_TRUE(router.value().flush().has_value());

    const std::size_t writes_before = rec->writes.size();
    const std::uint64_t drains_before = rec->drain_calls;
    auto pumped = router.value().pump();
    ASSERT_FALSE(pumped.has_value());
    EXPECT_EQ(pumped.error().code(), aa::ErrorCode::invalid_argument);
    EXPECT_EQ(rec->writes.size(), writes_before);
    EXPECT_EQ(rec->drain_calls, drains_before);
    EXPECT_EQ(router.value().state(), aa::audio::RouterState::active);
    router.value().close();
}

TEST_F(AudioPipelineRoute, RepeatedFlushDoesNotSendSecondEos) {
    // Gate fix 6: repeated flush() on a terminal route is idempotent - it
    // returns success and never sends a second EOS. Exactly one completed
    // drain for the route, including across close() (teardown never drains a
    // flushed route again).
    const auto config = pcm_router_config();
    auto sink = std::make_unique<RecordingAudioSink>();
    auto rec = sink->state;
    auto router = aa::audio::detail::open_router_with_sink_for_test(config, std::move(sink));
    ASSERT_TRUE(router.has_value());

    const aa::core::Nanoseconds base{13'000'000'000LL};
    const std::vector<std::byte> media = constant_pcm({kMediaLevel, 960});
    ASSERT_TRUE(router.value()
                    .submit(make_message(aa::protocol::ChannelRole::media_audio, 0, base, media))
                    .has_value());
    ASSERT_TRUE(router.value().flush().has_value());
    ASSERT_TRUE(router.value().flush().has_value());
    ASSERT_TRUE(router.value().flush().has_value());

    EXPECT_EQ(rec->drain_calls, 1U);
    EXPECT_EQ(rec->drains_completed, 1U);
    EXPECT_EQ(router.value().report().render_barrier.drains_completed, 1U);
    router.value().close();
    EXPECT_EQ(rec->drain_calls, 1U);
    EXPECT_EQ(router.value().report().render_barrier.drains_completed, 1U);
}

TEST_F(AudioPipelineRoute, FailedFlushLeavesRouteTerminalAndDegraded) {
    // Gate fix 6: when the terminal EOS drain cannot complete, the route
    // stays TERMINAL and degrades typed with the recorded cause. Every later
    // operation keeps the degraded refusal (audio_sink_unavailable), the
    // recorded cause is preserved, and the spent EOS drain is never retried.
    const auto config = pcm_router_config();
    auto sink = std::make_unique<RecordingAudioSink>();
    auto rec = sink->state;
    rec->fail_drain = true;
    auto router = aa::audio::detail::open_router_with_sink_for_test(config, std::move(sink));
    ASSERT_TRUE(router.has_value());

    const aa::core::Nanoseconds base{14'000'000'000LL};
    const std::vector<std::byte> media = constant_pcm({kMediaLevel, 960});
    ASSERT_TRUE(router.value()
                    .submit(make_message(aa::protocol::ChannelRole::media_audio, 0, base, media))
                    .has_value());
    auto flushed = router.value().flush();
    ASSERT_FALSE(flushed.has_value());
    EXPECT_EQ(flushed.error().code(), aa::ErrorCode::audio_sink_unavailable);

    ASSERT_EQ(router.value().state(), aa::audio::RouterState::degraded);
    const std::string recorded_cause = router.value().report().sink.cause;
    EXPECT_FALSE(recorded_cause.empty());
    EXPECT_NE(recorded_cause.find("disabled"), std::string::npos);
    EXPECT_NE(recorded_cause.find("drain failed"), std::string::npos);

    const auto late =
        make_message(aa::protocol::ChannelRole::media_audio, 1, base, media);
    auto refused = router.value().submit(late);
    ASSERT_FALSE(refused.has_value());
    EXPECT_EQ(refused.error().code(), aa::ErrorCode::audio_sink_unavailable);
    auto pumped = router.value().pump();
    ASSERT_FALSE(pumped.has_value());
    EXPECT_EQ(pumped.error().code(), aa::ErrorCode::audio_sink_unavailable);
    auto reflushed = router.value().flush();
    ASSERT_FALSE(reflushed.has_value());
    EXPECT_EQ(reflushed.error().code(), aa::ErrorCode::audio_sink_unavailable);

    EXPECT_EQ(rec->drain_calls, 1U);
    router.value().close();
    EXPECT_EQ(rec->drain_calls, 1U);
    EXPECT_EQ(router.value().report().sink.cause, recorded_cause);
}

namespace {

    void AudioPipelineRoute::open_incremental() {
        auto sink = std::make_unique<RecordingAudioSink>();
        rec = sink->state;
        auto config = pcm_router_config();
        config.render_quantum = aa::core::Nanoseconds{5'000'000};
        config.on_write_completed = [&](const auto& value) { completions.push_back(value); };
        auto opened = aa::audio::detail::open_router_with_sink_for_test(config, std::move(sink));
        ASSERT_TRUE(opened);
        router.emplace(std::move(opened).value());
    }

    void AudioPipelineRoute::enqueue(aa::protocol::ChannelRole role, std::uint64_t sequence,
                                    Tone tone, std::int64_t offset_ns) {
        const auto pcm = constant_pcm(tone);
        ASSERT_TRUE(router->submit(make_message(role, sequence,
            aa::core::Nanoseconds{base.count + offset_ns}, pcm), aa::core::Nanoseconds{123}));
    }

    aa::audio::RenderStep AudioPipelineRoute::step() {
        auto result = router->render_step();
        EXPECT_TRUE(result);
        return result ? std::move(result).value() : aa::audio::RenderStep{};
    }

    void AudioPipelineRoute::expect_headers(const std::vector<aa::protocol::MessageHeader>& actual,
                        const std::vector<std::pair<aa::protocol::ChannelRole, std::uint64_t>>& expected) {
        std::vector<std::pair<aa::protocol::ChannelRole, std::uint64_t>> identities;
        identities.reserve(actual.size());
        for (const auto& header : actual) { identities.emplace_back(header.channel, header.sequence); }
        EXPECT_EQ(identities, expected);
    }

} // namespace

TEST_F(AudioPipelineRoute, IncrementalSharedFrontierDoesNotDisplaceNewMediaForFutureGuidance) {
    open_incremental();
    enqueue(media, 1, {1000, 480});
    enqueue(guidance, 2, {2000, 240}, 100'000'000);
    const auto first = step();
    expect_headers(first.contributors, {{media, 1}});
    EXPECT_TRUE(first.completed.empty());
    EXPECT_TRUE(first.pending);
    enqueue(media, 3, {3000, 240}, 10'000'000);

    const auto second = step();

    expect_headers(second.contributors, {{media, 1}});
    expect_headers(second.completed, {{media, 1}});
    EXPECT_TRUE(second.pending);
    EXPECT_EQ(rec->write_ns, (std::vector<std::int64_t>{base.count, base.count + 5'000'000}));
    const auto third = step();
    expect_headers(third.contributors, {{media, 3}});
    expect_headers(third.completed, {{media, 3}});
    EXPECT_EQ(rec->write_ns.back(), base.count + 10'000'000);
    EXPECT_EQ(flatten_i16({rec->writes.back()}), std::vector<std::int16_t>(480, 3000));
    EXPECT_EQ(router->report().stats.frames_silence, 0U);
}

TEST_F(AudioPipelineRoute, IncrementalOverlappingRolesHaveExactCreditsAndShortTails) {
    open_incremental();
    enqueue(media, 7, {1000, 481});
    enqueue(guidance, 7, {2000, 240}, 5'000'000);

    const auto first = step();
    const auto overlap = step();
    const auto tail = step();
    const auto empty = step();

    expect_headers(first.contributors, {{media, 7}});
    EXPECT_TRUE(first.completed.empty());
    expect_headers(overlap.contributors, {{media, 7}, {guidance, 7}});
    expect_headers(overlap.completed, {{guidance, 7}});
    expect_headers(tail.contributors, {{media, 7}});
    expect_headers(tail.completed, {{media, 7}});
    EXPECT_FALSE(tail.pending);
    EXPECT_FALSE(empty.pending);
    EXPECT_TRUE(empty.completed.empty());
    EXPECT_TRUE(empty.contributors.empty());
    ASSERT_EQ(rec->writes.size(), 3U);
    EXPECT_EQ(flatten_i16({rec->writes[0]}), std::vector<std::int16_t>(480, 1000));
    EXPECT_EQ(flatten_i16({rec->writes[1]}), std::vector<std::int16_t>(480, 2250));
    EXPECT_EQ(flatten_i16({rec->writes[2]}), std::vector<std::int16_t>(2, 250));
    const auto& stats = router->report().stats;
    EXPECT_EQ(stats.frames_decoded, 721U);
    EXPECT_EQ(stats.frames_retained, 721U);
    EXPECT_EQ(stats.frames_output, 481U);
    ASSERT_EQ(completions.size(), 3U);
    EXPECT_EQ(completions.back().scheduled.count, base.count + 10'000'000);
    EXPECT_EQ(completions.back().scheduled_end.count, base.count + 481LL * 1'000'000'000 / 48000);
    EXPECT_EQ(stats.last_path_latency.count, completions.back().completed.count - 123);
}

TEST_F(AudioPipelineRoute, IncrementalStepsMatchBatchedPcmWithFifoOverlapsAndGaps) {
    open_incremental();
    auto sink = std::make_unique<RecordingAudioSink>();
    const auto batched_rec = sink->state;
    auto batched = aa::audio::detail::open_router_with_sink_for_test(pcm_router_config(), std::move(sink));
    ASSERT_TRUE(batched);
    const std::array<aa::protocol::ChannelRole, 4> roles{media, media, guidance, media};
    const std::array<std::int64_t, 4> times{0, 0, 5'000'000, 30'000'000};
    const std::array<Tone, 4> tones{{{1000, 481}, {3000, 239}, {2000, 240}, {5000, 1}}};
    for (std::size_t i = 0; i < roles.size(); ++i) {
        enqueue(roles[i], i, tones[i], times[i]);
        const auto pcm = constant_pcm(tones[i]);
        ASSERT_TRUE(batched.value().submit(make_message(roles[i], i,
            aa::core::Nanoseconds{base.count + times[i]}, pcm)));
    }
    ASSERT_TRUE(batched.value().pump());

    std::vector<std::pair<aa::protocol::ChannelRole, std::uint64_t>> completed;
    for (int i = 0; i < 20; ++i) {
        const auto result = step();
        for (const auto& header : result.completed) { completed.emplace_back(header.channel, header.sequence); }
        if (!result.pending) { break; }
        ASSERT_LT(i, 19);
    }

    EXPECT_EQ(flatten_i16(rec->writes), flatten_i16(batched_rec->writes));
    EXPECT_EQ(completed, (std::vector<std::pair<aa::protocol::ChannelRole, std::uint64_t>>{
        {guidance, 2}, {media, 0}, {media, 1}, {media, 3}}));
    std::uint64_t frame_offset = 0;
    for (std::size_t i = 0; i < rec->writes.size(); ++i) {
        EXPECT_LE(rec->writes[i].size(), 240U * 4U);
        EXPECT_EQ(rec->write_ns[i], base.count
            + static_cast<std::int64_t>(frame_offset * 1'000'000'000 / 48000));
        frame_offset += rec->writes[i].size() / 4;
    }
    EXPECT_EQ(router->report().stats.frames_silence, 720U);
}

TEST_F(AudioPipelineRoute, IncrementalCancellationBeforeStepDiscardsOnlySelectedRole) {
    open_incremental();
    enqueue(media, 1, {1000, 960});
    enqueue(guidance, 2, {2000, 240});

    router->cancel_pending(media);
    const auto result = step();

    expect_headers(result.contributors, {{guidance, 2}});
    expect_headers(result.completed, {{guidance, 2}});
    EXPECT_FALSE(result.pending);
    EXPECT_EQ(flatten_i16(rec->writes), std::vector<std::int16_t>(480, 2000));
}

TEST_F(AudioPipelineRoute, IncrementalCancellationDuringWriteSuppressesEvenFinalParentCredit) {
    open_incremental();
    enqueue(media, 1, {1000, 240});
    enqueue(media, 2, {3000, 960});
    enqueue(guidance, 1, {2000, 480});
    std::mutex mutex;
    std::condition_variable cv;
    bool entered = false;
    bool cancelled = false;
    rec->during_write = [&] {
        std::unique_lock lock(mutex);
        entered = true;
        cv.notify_all();
        EXPECT_TRUE(cv.wait_for(lock, std::chrono::seconds{2}, [&] { return cancelled; }));
    };
    std::jthread canceller([&] {
        std::unique_lock lock(mutex);
        if (!cv.wait_for(lock, std::chrono::seconds{2}, [&] { return entered; })) { return; }
        lock.unlock();
        router->cancel_pending(media);
        lock.lock();
        cancelled = true;
        cv.notify_all();
    });

    const auto result = step();

    canceller.join();
    rec->during_write = {};
    expect_headers(result.contributors, {{media, 1}, {guidance, 1}});
    EXPECT_TRUE(result.completed.empty());
    // Media 1 still reached the sink whole; guidance 1 only partly.
    expect_headers(result.delivered, {{media, 1}});
    EXPECT_TRUE(result.pending);
    EXPECT_EQ(flatten_i16(rec->writes), std::vector<std::int16_t>(480, 2250));
    const auto tail = step();
    expect_headers(tail.completed, {{guidance, 1}});
    EXPECT_FALSE(tail.pending);
    enqueue(media, 3, {4000, 1}, 10'000'000);
    expect_headers(step().completed, {{media, 3}});
}

TEST_F(AudioPipelineRoute, IncrementalFailedWriteAfterCancellationStillFailsAndFlushDoesNotRetry) {
    open_incremental();
    enqueue(media, 1, {1000, 960});
    rec->during_write = [&] { router->cancel_pending(media); };
    rec->fail_write = true;

    const auto result = router->render_step();

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code(), aa::ErrorCode::audio_sink_unavailable);
    EXPECT_EQ(router->state(), aa::audio::RouterState::degraded);
    EXPECT_EQ(router->report().stats.frames_output, 0U);
    EXPECT_EQ(router->report().stats.frames_retained, 0U);
    EXPECT_TRUE(completions.empty());
    EXPECT_FALSE(router->flush());
    EXPECT_FALSE(router->pump());
    EXPECT_FALSE(router->render_step());
    EXPECT_EQ(rec->writes.size(), 1U);
    EXPECT_EQ(rec->drain_calls, 0U);
    router->close();
    EXPECT_EQ(rec->writes.size(), 1U);
}

TEST_F(AudioPipelineRoute, IncrementalFlushDrainsRemainderOnceAndCloseDoesNotReplay) {
    open_incremental();
    enqueue(media, 1, {1000, 481});
    ASSERT_TRUE(step().pending);

    ASSERT_TRUE(router->flush());

    EXPECT_EQ(flatten_i16(rec->writes), std::vector<std::int16_t>(962, 1000));
    EXPECT_EQ(rec->drain_calls, 1U);
    EXPECT_FALSE(router->render_step());
    ASSERT_TRUE(router->flush());
    router->close();
    EXPECT_EQ(rec->drain_calls, 1U);
    EXPECT_EQ(rec->writes.size(), 3U);
}

TEST_F(AudioPipelineRoute, IncrementalCloseDiscardsUnrenderedRemainder) {
    open_incremental();
    enqueue(media, 1, {1000, 960});
    ASSERT_TRUE(step().pending);

    router->close();

    EXPECT_EQ(rec->writes.size(), 1U);
    EXPECT_EQ(rec->drain_calls, 1U);
    EXPECT_FALSE(router->render_step());
}

TEST_F(AudioPipelineRoute, IncrementalHiddenFutureParentRejectsWholeScheduleBeforeWrite) {
    open_incremental();
    enqueue(media, 1, {1000, 240});
    enqueue(media, 2, {1000, 240}, 300'000'000);

    const auto result = router->render_step();

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code(), aa::ErrorCode::invalid_argument);
    EXPECT_TRUE(rec->writes.empty());
    ASSERT_TRUE(router->report().render_failure);
    const auto failure = *router->report().render_failure;
    EXPECT_EQ(failure.reason, aa::audio::RenderFailureReason::single_gap);
    ASSERT_TRUE(failure.offending);
    EXPECT_EQ(failure.offending->sequence, 2U);
    EXPECT_EQ(failure.base_ns, 1'000'000'000);
    EXPECT_FALSE(failure.base_committed);
    EXPECT_EQ(failure.entry_cursor, 0U);
    EXPECT_EQ(failure.intended, 14400);
    EXPECT_EQ(failure.start, 14400U);
    EXPECT_EQ(failure.raw_gap, 14160U);
    EXPECT_EQ(failure.uncovered_gap, 14160U);
    EXPECT_EQ(failure.total_gap_before, 0U);
    EXPECT_EQ(failure.cap, 9600U);
    router->cancel_pending(media);
    router->retire_all_role_idle();
    ASSERT_TRUE(router->report().render_failure);
    EXPECT_EQ(router->report().render_failure->reason, failure.reason);
    EXPECT_FALSE(step().pending);
    EXPECT_FALSE(router->report().render_failure);
    enqueue(media, 3, {1000, 1});
    expect_headers(step().completed, {{media, 3}});
    EXPECT_EQ(rec->write_ns.front(), base.count);
    EXPECT_FALSE(router->report().render_failure);
}

TEST_F(AudioPipelineRoute, IncrementalTotalGapBudgetRejectsBeforeAnyStep) {
    open_incremental();
    enqueue(media, 1, {1000, 240});
    enqueue(guidance, 2, {2000, 240}, 155'000'000);
    enqueue(media, 3, {1000, 240}, 310'000'000);

    const auto result = router->render_step();

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code(), aa::ErrorCode::invalid_argument);
    EXPECT_TRUE(rec->writes.empty());
    ASSERT_TRUE(router->report().render_failure);
    const auto& failure = *router->report().render_failure;
    EXPECT_EQ(failure.reason, aa::audio::RenderFailureReason::aggregate_gap);
    ASSERT_TRUE(failure.offending);
    EXPECT_EQ(failure.offending->sequence, 3U);
    EXPECT_EQ(failure.raw_gap, 7200U);
    EXPECT_EQ(failure.uncovered_gap, 7200U);
    EXPECT_EQ(failure.total_gap_before, 7200U);
    EXPECT_EQ(failure.cap, 9600U);
}

TEST_F(AudioPipelineRoute, IncrementalGapAtCapIsBoundedSilenceWithoutContributorCredits) {
    open_incremental();
    enqueue(media, 1, {1000, 240});
    expect_headers(step().completed, {{media, 1}});
    enqueue(guidance, 2, {2000, 1}, 205'000'000);

    for (int i = 0; i < 40; ++i) {
        const auto silence = step();
        EXPECT_TRUE(silence.contributors.empty());
        EXPECT_TRUE(silence.completed.empty());
        EXPECT_TRUE(silence.pending);
        EXPECT_EQ(rec->writes.back(), std::vector<std::byte>(960));
    }
    const auto tail = step();

    expect_headers(tail.completed, {{guidance, 2}});
    EXPECT_FALSE(tail.pending);
    EXPECT_EQ(router->report().stats.frames_silence, 9600U);
    EXPECT_EQ(completions.size(), 2U);
    EXPECT_EQ(rec->write_ns.back(), base.count + 205'000'000);
}

TEST_F(AudioPipelineRoute, IncrementalLateArrivalAboveCapRejectsWithoutMovingCursor) {
    open_incremental();
    enqueue(media, 1, {1000, 240});
    step();
    const auto pcm = constant_pcm({3000, 1});
    ASSERT_TRUE(router->submit(make_message(media, 2, aa::core::Nanoseconds{0}, pcm)));

    const auto rejected = router->render_step();

    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().code(), aa::ErrorCode::invalid_argument);
    ASSERT_TRUE(router->report().render_failure);
    const auto& failure = *router->report().render_failure;
    EXPECT_EQ(failure.reason, aa::audio::RenderFailureReason::late);
    ASSERT_TRUE(failure.offending);
    EXPECT_EQ(failure.offending->timestamp.count, 0);
    EXPECT_EQ(failure.base_ns, 1'000'000'000);
    EXPECT_TRUE(failure.base_committed);
    EXPECT_EQ(failure.entry_cursor, 240U);
    EXPECT_EQ(failure.intended, -48000);
    EXPECT_EQ(failure.cap, 9600U);
    EXPECT_EQ(failure.preflighted, false);
    EXPECT_FALSE(failure.start);
    EXPECT_FALSE(failure.now_ns);
    EXPECT_FALSE(failure.raw_gap);
    enqueue(media, 3, {3000, 1}, 5'000'000);
    expect_headers(step().completed, {{media, 3}});
    EXPECT_EQ(rec->write_ns.back(), base.count + 5'000'000);
    EXPECT_FALSE(router->report().render_failure);
}

TEST_F(AudioPipelineRoute, IncrementalFailureSnapshotKeepsFirstReasonAcrossPartialPlanScanAndClose) {
    open_incremental();
    enqueue(media, 1, {1000, 240});
    step();
    enqueue(media, 2, {1000, 240}, 300'000'000);
    const auto pcm = constant_pcm({2000, 240});
    ASSERT_TRUE(router->submit(make_message(guidance, 3, aa::core::Nanoseconds{0}, pcm)));
    const auto rejected = router->render_step();
    ASSERT_FALSE(rejected);
    ASSERT_TRUE(router->report().render_failure);
    EXPECT_EQ(router->report().render_failure->reason, aa::audio::RenderFailureReason::late);
    ASSERT_TRUE(router->report().render_failure->offending);
    EXPECT_EQ(router->report().render_failure->offending->sequence, 3U);
    EXPECT_EQ(rec->writes.size(), 1U);
    router->close();
    ASSERT_TRUE(router->report().render_failure);
    EXPECT_EQ(router->report().render_failure->reason, aa::audio::RenderFailureReason::late);
}

TEST_F(AudioPipelineRoute, IncrementalParentEndConversionSnapshotHasExactOperandBeforeAnyWrite) {
    open_incremental();
    const auto pcm = constant_pcm({1000, 960});
    ASSERT_TRUE(router->submit(make_message(media, 1,
        aa::core::Nanoseconds{INT64_MAX - 5'000'000}, pcm)));
    const auto rejected = router->render_step();
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().code(), aa::ErrorCode::invalid_argument);
    ASSERT_TRUE(router->report().render_failure);
    const auto& failure = *router->report().render_failure;
    EXPECT_EQ(failure.reason, aa::audio::RenderFailureReason::parent_end_time_conversion);
    EXPECT_EQ(failure.time_conversion_frame, 960U);
    EXPECT_EQ(failure.start, 0U);
    EXPECT_EQ(failure.remaining, 960U);
    EXPECT_FALSE(failure.raw_gap);
    EXPECT_FALSE(failure.now_ns);
    EXPECT_TRUE(rec->writes.empty());
}

TEST_F(AudioPipelineRoute, IncrementalDriftTrimIsOncePerParentAndRetainsAbsolutePcmOffsets) {
    open_incremental();
    auto config = pcm_router_config();
    config.live_pcm_drift.enabled = true;
    config.render_quantum = aa::core::Nanoseconds{5'000'000};
    auto sink = std::make_unique<RecordingAudioSink>();
    const auto observed = sink->state;
    auto opened = aa::audio::detail::open_router_with_sink_for_test(config, std::move(sink));
    ASSERT_TRUE(opened);
    auto& route = opened.value();
    const auto lead = constant_pcm({1000, 1440});
    ASSERT_TRUE(route.submit(make_message(media, 1, base, lead)));
    ASSERT_TRUE(route.pump());
    std::vector<std::byte> ramp;
    for (std::int16_t i = 0; i < 961; ++i) {
        const auto frame = constant_pcm({i, 1});
        ramp.insert(ramp.end(), frame.begin(), frame.end());
    }
    ASSERT_TRUE(route.submit(make_message(media, 2, base, ramp)));

    for (int i = 0; i < 4; ++i) {
        const auto rendered = route.render_step();
        ASSERT_TRUE(rendered);
        EXPECT_EQ(rendered.value().pending, i != 3);
        expect_headers(rendered.value().completed, i == 3
            ? std::vector<std::pair<aa::protocol::ChannelRole, std::uint64_t>>{{media, 2}}
            : std::vector<std::pair<aa::protocol::ChannelRole, std::uint64_t>>{});
    }

    const auto& stats = route.report().stats;
    EXPECT_EQ(stats.frames_decoded, 2401U);
    EXPECT_EQ(stats.frames_trimmed, 240U);
    EXPECT_EQ(stats.frames_retained, 2161U);
    EXPECT_EQ(stats.frames_output, 2161U);
    const auto flat = flatten_i16(observed->writes);
    ASSERT_EQ(flat.size(), 2161U * 2U);
    for (std::size_t i = 0; i < 721; ++i) {
        EXPECT_EQ(flat[(1440 + i) * 2], static_cast<std::int16_t>(i + 240));
    }
    EXPECT_EQ(observed->write_ns.back(), base.count + 45'000'000);
}

// #154: the catch-up drops a run's whole excess above target once its phase
// passes the watermark; below it (and with the watermark unset) the bounded
// 5 ms trim is unchanged.
TEST_F(AudioPipelineRoute, DriftPolicyCatchUpDropsTheWholeExcessOnlyAboveItsWatermark) {
    aa::audio::LivePcmDriftPolicy policy;
    policy.enabled = true;
    policy.catch_up_watermark = aa::core::Nanoseconds{90'000'000LL};
    const aa::audio::LivePcmDriftCorrector corrector{policy, 48000};
    EXPECT_EQ(corrector.trim_frames(aa::core::Nanoseconds{30'000'000LL}, 4800), 240U);
    EXPECT_EQ(corrector.caught_up_frames(aa::core::Nanoseconds{30'000'000LL}, 4800), 0U);
    EXPECT_EQ(corrector.trim_frames(aa::core::Nanoseconds{90'000'000LL}, 4800), 240U);
    EXPECT_EQ(corrector.caught_up_frames(aa::core::Nanoseconds{90'000'000LL}, 4800), 0U);
    // 100 ms behind: everything above the 15 ms target (85 ms = 4080 frames).
    EXPECT_EQ(corrector.trim_frames(aa::core::Nanoseconds{100'000'000LL}, 4800), 4080U);
    EXPECT_EQ(corrector.caught_up_frames(aa::core::Nanoseconds{100'000'000LL}, 4800), 3840U);
    // Never more than the run holds.
    EXPECT_EQ(corrector.trim_frames(aa::core::Nanoseconds{300'000'000LL}, 2048), 2048U);
    EXPECT_EQ(corrector.caught_up_frames(aa::core::Nanoseconds{300'000'000LL}, 2048), 1808U);
    aa::audio::LivePcmDriftPolicy bounded;
    bounded.enabled = true;
    const aa::audio::LivePcmDriftCorrector unchanged{bounded, 48000};
    EXPECT_EQ(unchanged.trim_frames(aa::core::Nanoseconds{300'000'000LL}, 4800), 240U);
    EXPECT_EQ(unchanged.caught_up_frames(aa::core::Nanoseconds{300'000'000LL}, 4800), 0U);
    aa::audio::LivePcmDriftPolicy lossless;
    lossless.catch_up_watermark = aa::core::Nanoseconds{90'000'000LL};
    const aa::audio::LivePcmDriftCorrector disabled{lossless, 48000};
    EXPECT_EQ(disabled.trim_frames(aa::core::Nanoseconds{300'000'000LL}, 4800), 0U);
}

namespace {

// One drift-enabled incremental route on a recording sink, rendering in the
// production 50 ms quantum; catch_up_ns = 0 keeps the bounded trim only.
struct BurstRoute final {
    std::shared_ptr<RecordingSinkState> observed;
    std::optional<aa::audio::AudioRouter> route;

    explicit BurstRoute(std::int64_t catch_up_ns,
                        std::optional<aa::audio::AudioFormat> guidance_format = std::nullopt,
                        aa::audio::detail::TestDecodeFn decode = {}) {
        auto config = pcm_router_config();
        if (guidance_format) {
            config.roles[aa::audio::role_index(aa::audio::Role::guidance)].input_format = *guidance_format;
        }
        config.live_pcm_drift.enabled = true;
        config.live_pcm_drift.catch_up_watermark = aa::core::Nanoseconds{catch_up_ns};
        config.render_quantum = aa::core::Nanoseconds{50'000'000};
        auto sink = std::make_unique<RecordingAudioSink>();
        observed = sink->state;
        auto opened = decode
            ? aa::audio::detail::open_router_with_decoder_for_test(config, std::move(sink), std::move(decode))
            : aa::audio::detail::open_router_with_sink_for_test(config, std::move(sink));
        if (opened) { route.emplace(std::move(opened).value()); }
    }

    // End of the last write on the 48 kHz timeline (steady-clock ns).
    [[nodiscard]] std::int64_t written_end_ns() const {
        if (observed->write_ns.empty()) { return 0; }
        const auto frames = static_cast<std::int64_t>(observed->writes.back().size() / 4);
        return observed->write_ns.back() + frames * 1'000'000'000LL / 48000;
    }

    // Renders until nothing is pending; returns the first failure, if any.
    [[nodiscard]] aa::core::Result<void> drain() {
        for (int i = 0; i < 256; ++i) {
            auto stepped = route->render_step();
            if (!stepped) { return stepped.error(); }
            if (!stepped.value().pending) { return {}; }
        }
        return aa::Error{aa::ErrorCode::timeout};
    }
};

} // namespace

// #154: a sink stall withholds credit, then the phone bursts its backlog, all
// stamped at about one arrival. Sequentialized after the cursor, the burst
// used to be placed ever further behind its arrival (5 ms trimmed per chunk),
// so the next real-time arrival fell more than 200 ms behind the cursor: a
// "late" rejection, which the receiver latches as a session-ending
// audio_sink_failure (reproduced live after a 500 ms pipewire-pulse pause).
// With the catch-up, a run placed more than 90 ms behind its arrival is
// trimmed down to the 15 ms target at once (here each 42.7 ms chunk is
// dropped whole) and the resumed arrival plays 18 ms behind it.
TEST_F(AudioPipelineRoute, IncrementalStallBurstCatchesUpInsteadOfRejectingTheNextArrival) {
    BurstRoute burst{90'000'000};
    ASSERT_TRUE(burst.route);
    auto& route = *burst.route;
    const auto chunk = constant_pcm({1000, 2048}); // one 42.67 ms phone chunk
    for (std::uint64_t i = 0; i < 16; ++i) {
        ASSERT_TRUE(route.submit(make_message(media, i, base, chunk)));
    }
    ASSERT_TRUE(burst.drain());
    const auto resumed = aa::core::Nanoseconds{base.count + 100'000'000};
    ASSERT_TRUE(route.submit(make_message(media, 16, resumed, constant_pcm({2000, 2048}))));

    const auto drained = burst.drain();

    ASSERT_TRUE(drained) << aa::to_string(drained.error().code());
    EXPECT_EQ(route.state(), aa::audio::RouterState::active);
    const auto& stats = route.report().stats;
    // Chunks 2 and 3 keep the bounded 5 ms trim (phase 42.7 and 80.3 ms);
    // chunks 4-16 start 118 ms behind their arrival and are dropped whole.
    EXPECT_EQ(stats.frames_trimmed, 2U * 240U + 13U * 2048U);
    EXPECT_EQ(stats.frames_caught_up, 13U * (2048U - 240U));
    EXPECT_EQ(stats.frames_retained, 2048U + 2U * 1808U + 2048U);
    ASSERT_FALSE(burst.observed->write_ns.empty());
    EXPECT_EQ(burst.observed->write_ns.back(), base.count + 118'000'000);
    EXPECT_EQ(flatten_i16({burst.observed->writes.back()}), std::vector<std::int16_t>(4096, 2000));
}

// The same burst without the catch-up is the pre-#154 behaviour: the burst is
// carried ~600 ms behind its arrival and the next arrival is rejected late.
TEST_F(AudioPipelineRoute, IncrementalStallBurstWithoutCatchUpRejectsTheNextArrivalLate) {
    BurstRoute burst{0};
    ASSERT_TRUE(burst.route);
    auto& route = *burst.route;
    const auto chunk = constant_pcm({1000, 2048});
    for (std::uint64_t i = 0; i < 16; ++i) {
        ASSERT_TRUE(route.submit(make_message(media, i, base, chunk)));
    }
    ASSERT_TRUE(burst.drain());
    EXPECT_EQ(route.report().stats.frames_caught_up, 0U);
    ASSERT_TRUE(route.submit(make_message(media, 16, aa::core::Nanoseconds{base.count + 100'000'000},
                                          constant_pcm({2000, 2048}))));

    const auto drained = burst.drain();

    ASSERT_FALSE(drained);
    EXPECT_EQ(drained.error().code(), aa::ErrorCode::invalid_argument);
    ASSERT_TRUE(route.report().render_failure);
    EXPECT_EQ(route.report().render_failure->reason, aa::audio::RenderFailureReason::late);
}

// Voice uses the same policy: a guidance burst far behind its arrival is
// caught up too, so a stall during a prompt cannot end the session either.
TEST_F(AudioPipelineRoute, IncrementalGuidanceStallBurstIsCaughtUpLikeMedia) {
    BurstRoute burst{90'000'000};
    ASSERT_TRUE(burst.route);
    auto& route = *burst.route;
    const auto chunk = constant_pcm({1000, 2048});
    for (std::uint64_t i = 0; i < 16; ++i) {
        ASSERT_TRUE(route.submit(make_message(guidance, i, base, chunk)));
    }
    ASSERT_TRUE(burst.drain());
    ASSERT_TRUE(route.submit(make_message(guidance, 16, aa::core::Nanoseconds{base.count + 100'000'000},
                                          constant_pcm({2000, 2048}))));

    ASSERT_TRUE(burst.drain());
    EXPECT_EQ(route.report().stats.frames_caught_up, 13U * (2048U - 240U));
    EXPECT_EQ(burst.observed->write_ns.back(), base.count + 118'000'000);
}

// The catch-up removes a partly retained run's head, not its tail: the oldest,
// latest-placed samples go and the newest are kept, in order. Three chunks
// leave the cursor 118 ms behind their shared arrival; a 200 ms ramp arriving
// then is trimmed to the 15 ms target (103 ms = 4944 frames, 4704 of them
// beyond the bounded 240-frame trim) and plays from frame 4944 onward.
TEST_F(AudioPipelineRoute, IncrementalCatchUpTrimsTheRunHeadAndKeepsItsTail) {
    BurstRoute burst{90'000'000};
    ASSERT_TRUE(burst.route);
    auto& route = *burst.route;
    const auto chunk = constant_pcm({1000, 2048});
    for (std::uint64_t i = 0; i < 3; ++i) {
        ASSERT_TRUE(route.submit(make_message(media, i, base, chunk)));
    }
    std::vector<std::byte> ramp;
    for (std::int16_t i = 0; i < 9600; ++i) {
        const auto frame = constant_pcm({i, 1});
        ramp.insert(ramp.end(), frame.begin(), frame.end());
    }
    ASSERT_TRUE(route.submit(make_message(media, 3, base, ramp)));

    ASSERT_TRUE(burst.drain());

    const auto& stats = route.report().stats;
    EXPECT_EQ(stats.frames_trimmed, 2U * 240U + 4944U);
    EXPECT_EQ(stats.frames_caught_up, 4944U - 240U);
    EXPECT_EQ(stats.frames_retained, 2048U + 2U * 1808U + 4656U);
    const auto flat = flatten_i16(burst.observed->writes);
    constexpr std::size_t lead = 2048 + 2 * 1808;
    ASSERT_EQ(flat.size(), (lead + 4656U) * 2U);
    for (std::size_t i = 0; i < 4656; ++i) {
        ASSERT_EQ(flat[(lead + i) * 2], static_cast<std::int16_t>(4944 + i)) << i;
        ASSERT_EQ(flat[(lead + i) * 2 + 1], static_cast<std::int16_t>(4944 + i)) << i;
    }
}

// A stand-in for the 16 kHz mono -> 48 kHz stereo decoder whose output
// schedule a test controls (#155 Sol check): every payload's samples are
// buffered (each 16 kHz sample becomes three 48 kHz frames of the same
// value) and decode call i returns the oldest `script(i, buffered)` frames,
// at least one and at most what is buffered. Output can thus be split
// across runs (a 200 ms parent returning 96 frames) or coalesced into one
// (19098 frames, ~398 ms, as Sol's decoder replay returned for one 200 ms
// submission) deterministically, independent of GStreamer's threads. 48 kHz
// stereo payloads (media) pass through 1:1.
struct ScriptedResampler final {
    std::function<std::size_t(std::size_t call, std::size_t buffered)> script;
    std::deque<std::int16_t> buffered{};
    std::size_t calls{0};

    [[nodiscard]] static aa::audio::detail::TestDecodeFn decoder(
        std::function<std::size_t(std::size_t, std::size_t)> script) {
        auto state = std::make_shared<ScriptedResampler>();
        state->script = std::move(script);
        return [state](const aa::audio::EncodedChunk& chunk)
                   -> aa::core::Result<std::vector<std::byte>> {
            if (chunk.input_format.sample_rate_hz == 48000) {
                return std::vector<std::byte>(chunk.payload.begin(), chunk.payload.end());
            }
            for (std::size_t i = 0; i + 1 < chunk.payload.size(); i += 2) {
                std::int16_t sample = 0;
                std::memcpy(&sample, chunk.payload.data() + i, sizeof(sample));
                state->buffered.insert(state->buffered.end(), 3, sample);
            }
            const auto wanted = state->script(state->calls++, state->buffered.size());
            const auto count = std::clamp<std::size_t>(wanted, 1, state->buffered.size());
            std::vector<std::byte> out(count * 4);
            for (std::size_t f = 0; f < count; ++f) {
                const auto value = state->buffered.front();
                state->buffered.pop_front();
                std::memcpy(out.data() + f * 4, &value, sizeof(value));
                std::memcpy(out.data() + f * 4 + 2, &value, sizeof(value));
            }
            return out;
        };
    }
};

// The production worker admits 6400-byte guidance parents: 200 ms of 16 kHz
// mono. value marks the parent its samples came from.
[[nodiscard]] std::vector<std::byte> guidance_parent(std::int16_t value = 1000,
                                                     std::size_t bytes = 6400) {
    std::vector<std::byte> pcm(bytes);
    for (std::size_t i = 0; i < pcm.size(); i += 2) {
        std::memcpy(pcm.data() + i, &value, sizeof(value));
    }
    return pcm;
}

// Sol's burst: max-size parents stamped 0, 1, 10, 12 ... 20 ms apart, the
// route rendered to idle after each.
constexpr std::array<std::int64_t, 10> kGuidanceBurstMs{0, 1, 10, 12, 12, 12, 13, 14, 20, 20};

// A burst of 200 ms guidance parents (Sol re-review and check of #155): the
// first carries the cursor ~200 ms past its arrival, so every later parent
// starts more than 200 ms behind it although its own audio is fresh, and the
// resampler may split a parent's output or coalesce several into one run.
// The catch-up judges a run's phase from the source time of its oldest
// sample (output beyond the parent's own duration is older carryover) and
// the late check uses the parent's own PTS range, so the burst survives on
// every output schedule: named splits and coalescings, Sol's 19098-frame
// run, and 200 seeded schedules (minstd_rand: the same on every platform).
// Placement never carries the cursor more than the 90 ms watermark past the
// end of the newest parent's PCM.
TEST_F(AudioPipelineRoute, IncrementalMaxSizeGuidanceBurstSurvivesEveryResamplerSchedule) {
    using Script = std::function<std::size_t(std::size_t, std::size_t)>;
    std::vector<std::pair<std::string, Script>> schedules{
        {"one parent per run", [](std::size_t, std::size_t) { return std::size_t{9600}; }},
        {"split then coalesced", [](std::size_t call, std::size_t buffered) {
            return call % 2 == 0 ? std::size_t{96} : buffered; }},
        {"sol 19098-frame run", [](std::size_t call, std::size_t buffered) {
            return call == 0 ? std::size_t{96} : call == 1 ? std::size_t{19098} : buffered; }},
        {"three into one", [](std::size_t call, std::size_t buffered) {
            return call % 3 == 2 ? buffered : std::size_t{96}; }},
        {"everything held", [](std::size_t, std::size_t) { return std::size_t{1}; }},
        {"coalesced to the last", [](std::size_t call, std::size_t buffered) {
            return call == kGuidanceBurstMs.size() - 1 ? buffered : std::size_t{1}; }},
    };
    for (std::uint32_t seed = 1; seed <= 200; ++seed) {
        auto rng = std::make_shared<std::minstd_rand>(seed);
        schedules.emplace_back("seed " + std::to_string(seed),
            [rng](std::size_t, std::size_t buffered) -> std::size_t {
                switch ((*rng)() % 4) {
                case 0: return 96;
                case 1: return buffered;
                case 2: return 9600;
                default: return (*rng)() % buffered + 1;
                }
            });
    }
    for (auto& [name, script] : schedules) {
        SCOPED_TRACE(name);
        BurstRoute burst{90'000'000, aa::audio::AudioFormat{16000, 1, 16},
                         ScriptedResampler::decoder(script)};
        ASSERT_TRUE(burst.route);
        auto& route = *burst.route;
        std::vector<std::vector<std::byte>> parents;
        for (std::size_t i = 0; i < kGuidanceBurstMs.size(); ++i) {
            parents.push_back(guidance_parent(static_cast<std::int16_t>(1000 + i)));
            ASSERT_TRUE(route.submit(make_message(guidance, i,
                aa::core::Nanoseconds{base.count + kGuidanceBurstMs[i] * 1'000'000}, parents.back())))
                << i;
            const auto drained = burst.drain();
            const auto& failure = route.report().render_failure;
            ASSERT_TRUE(drained) << "parent " << i << " reason "
                << (failure ? static_cast<int>(failure->reason) : -1) << " cursor "
                << (failure ? failure->entry_cursor : 0) << " intended "
                << (failure && failure->intended ? *failure->intended : 0);
            // The cursor invariant: never more than the watermark past the
            // end of the newest parent's own PCM.
            EXPECT_LE(burst.written_end_ns(),
                      base.count + (kGuidanceBurstMs[i] + 200 + 90) * 1'000'000) << i;
        }
        EXPECT_EQ(route.state(), aa::audio::RouterState::active);
        EXPECT_FALSE(route.report().render_failure);
        // Any schedule that let a second parent's worth out built a backlog
        // to catch up ("everything held" returns one frame per call).
        if (route.report().stats.frames_decoded > 2U * 9600U) {
            EXPECT_GT(route.report().stats.frames_caught_up, 0U);
        }
    }
}

// The same burst without the catch-up still hits the late cap, whichever way
// the output is split: the test above would fail on the bounded policy.
TEST_F(AudioPipelineRoute, IncrementalMaxSizeGuidanceBurstWithoutCatchUpIsRejectedLate) {
    for (const bool split : {false, true}) {
        SCOPED_TRACE(split);
        BurstRoute burst{0, aa::audio::AudioFormat{16000, 1, 16},
            ScriptedResampler::decoder([split](std::size_t call, std::size_t buffered) {
                return split && call % 2 == 0 ? std::size_t{96} : std::min<std::size_t>(buffered, 9600);
            })};
        ASSERT_TRUE(burst.route);
        auto& route = *burst.route;
        const auto pcm = guidance_parent();
        std::optional<aa::Error> failure;
        for (std::size_t i = 0; i < kGuidanceBurstMs.size() && !failure; ++i) {
            ASSERT_TRUE(route.submit(make_message(guidance, i,
                aa::core::Nanoseconds{base.count + kGuidanceBurstMs[i] * 1'000'000}, pcm)));
            auto drained = burst.drain();
            if (!drained) { failure = drained.error(); }
        }
        ASSERT_TRUE(failure);
        ASSERT_TRUE(route.report().render_failure);
        EXPECT_EQ(route.report().render_failure->reason, aa::audio::RenderFailureReason::late);
    }
}

// The real GStreamer resampler on the same burst. Its split varies with
// scheduling; the outcome must not.
TEST_F(AudioPipelineRoute, IncrementalMaxSizeGuidanceBurstSurvivesTheRealResampler) {
    BurstRoute burst{90'000'000, aa::audio::AudioFormat{16000, 1, 16}};
    ASSERT_TRUE(burst.route);
    auto& route = *burst.route;
    const auto pcm = guidance_parent();
    for (std::size_t i = 0; i < kGuidanceBurstMs.size(); ++i) {
        ASSERT_TRUE(route.submit(make_message(guidance, i,
            aa::core::Nanoseconds{base.count + kGuidanceBurstMs[i] * 1'000'000}, pcm))) << i;
        const auto drained = burst.drain();
        const auto& failure = route.report().render_failure;
        ASSERT_TRUE(drained) << "parent " << i << " reason "
            << (failure ? static_cast<int>(failure->reason) : -1) << " cursor "
            << (failure ? failure->entry_cursor : 0) << " intended "
            << (failure && failure->intended ? *failure->intended : 0) << " decoded "
            << route.report().stats.frames_decoded;
        EXPECT_LE(burst.written_end_ns(), base.count + (kGuidanceBurstMs[i] + 200 + 90) * 1'000'000) << i;
    }
    EXPECT_FALSE(route.report().render_failure);
    EXPECT_GT(route.report().stats.frames_caught_up, 0U);
}

// Decoded length earns no lateness credit (Sol check of #155): a 20 ms parent
// whose run also carries 198 ms of carryover is judged by its own PTS range.
// Stamped 50 ms before the timeline base with the cursor 200 ms past it, its
// PCM ends 230 ms behind the cursor and is rejected late, though its 218 ms
// run would have covered the gap; stamped at the base (PCM ending 180 ms
// behind) the same run is caught up instead.
TEST_F(AudioPipelineRoute, IncrementalCatchUpGivesDecoderCarryoverNoLatenessCredit) {
    for (const std::int64_t stamp_ms : {-50, 0}) {
        SCOPED_TRACE(stamp_ms);
        BurstRoute burst{90'000'000, aa::audio::AudioFormat{16000, 1, 16},
            ScriptedResampler::decoder([](std::size_t call, std::size_t buffered) {
                return call == 0 ? std::size_t{96} : buffered; })};
        ASSERT_TRUE(burst.route);
        auto& route = *burst.route;
        const auto held = guidance_parent(1000);
        ASSERT_TRUE(route.submit(make_message(guidance, 0, base, held))); // 96 of 9600 frames out
        const auto media_pcm = constant_pcm({500, 9600});
        ASSERT_TRUE(route.submit(make_message(media, 0, base, media_pcm)));
        ASSERT_TRUE(burst.drain()); // cursor 200 ms past base
        const auto late_pcm = guidance_parent(2000, 640); // 20 ms; its run: 9504 + 960 frames
        ASSERT_TRUE(route.submit(make_message(guidance, 1,
            aa::core::Nanoseconds{base.count + stamp_ms * 1'000'000}, late_pcm)));

        const auto drained = burst.drain();

        if (stamp_ms < 0) {
            ASSERT_FALSE(drained);
            ASSERT_TRUE(route.report().render_failure);
            EXPECT_EQ(route.report().render_failure->reason, aa::audio::RenderFailureReason::late);
        } else {
            ASSERT_TRUE(drained) << aa::to_string(drained.error().code());
            EXPECT_EQ(route.report().stats.frames_caught_up, 9504U + 960U - 240U);
        }
    }
}

// Real lag is not hidden: a run whose every sample is more than 200 ms behind
// the cursor is still rejected late with the catch-up enabled, though the
// catch-up would have dropped it whole.
TEST_F(AudioPipelineRoute, IncrementalCatchUpStillRejectsARunWhollyBeyondTheLateCap) {
    BurstRoute burst{90'000'000};
    ASSERT_TRUE(burst.route);
    auto& route = *burst.route;
    ASSERT_TRUE(route.submit(make_message(media, 0, base, constant_pcm({1000, 9600}))));
    ASSERT_TRUE(burst.drain()); // cursor 200 ms past base
    // 20 ms stamped 50 ms before base: its newest sample is 230 ms behind.
    ASSERT_TRUE(route.submit(make_message(media, 1, aa::core::Nanoseconds{base.count - 50'000'000},
                                          constant_pcm({2000, 960}))));

    const auto drained = burst.drain();

    ASSERT_FALSE(drained);
    ASSERT_TRUE(route.report().render_failure);
    EXPECT_EQ(route.report().render_failure->reason, aa::audio::RenderFailureReason::late);
    EXPECT_EQ(route.report().stats.frames_caught_up, 0U);
}

// Ordinary jitter never reaches the watermark: two chunks delivered together
// (phase 42.7 ms) keep the bounded trim and lose nothing to the catch-up.
TEST_F(AudioPipelineRoute, IncrementalOrdinaryJitterNeverTriggersTheCatchUp) {
    BurstRoute burst{90'000'000};
    ASSERT_TRUE(burst.route);
    auto& route = *burst.route;
    const auto chunk = constant_pcm({1000, 2048});
    for (std::uint64_t i = 0; i < 40; i += 2) {
        const aa::core::Nanoseconds arrival{base.count + static_cast<std::int64_t>(i) * 42'666'667};
        ASSERT_TRUE(route.submit(make_message(media, i, arrival, chunk)));
        ASSERT_TRUE(route.submit(make_message(media, i + 1, arrival, chunk)));
        ASSERT_TRUE(burst.drain());
    }
    EXPECT_EQ(route.report().stats.frames_caught_up, 0U);
    EXPECT_GT(route.report().stats.frames_trimmed, 0U);
}

namespace {

// Window 8 (A26 over Wi-Fi, 2026-11-20): the phone's own per-second average
// media ack round trip in ms ("CAR.AUDIO.STATS: Acks from MEDIA received ...
// with avg latency"), 241 s. In every second count x latency is ~1000 ms: the
// phone always had a chunk queued and sent stop-and-wait (max_unacked 1), so
// it delivered one 2048-frame (42.67 ms) chunk per round trip. 5632 acks in
// 241 s match the 5632 chunks the receiver decoded.
constexpr std::array<std::int64_t, 241> kWindow8AckRoundTripMs{
    29, 24, 21, 26, 46, 46, 89, 88, 72, 44, 34, 34, 33, 32, 36, 46, 50, 58, 52, 50,
    33, 32, 32, 32, 29, 53, 44, 58, 57, 51, 35, 33, 33, 32, 23, 42, 48, 50, 63, 49,
    35, 33, 34, 31, 28, 29, 43, 52, 68, 70, 54, 34, 32, 32, 32, 24, 41, 42, 76, 63,
    56, 41, 33, 34, 34, 34, 38, 46, 49, 54, 59, 47, 33, 32, 33, 27, 26, 44, 48, 59,
    52, 57, 38, 34, 34, 34, 29, 37, 41, 52, 56, 55, 48, 33, 31, 32, 24, 25, 50, 46,
    54, 63, 55, 35, 32, 32, 32, 25, 33, 51, 48, 63, 68, 48, 33, 33, 34, 32, 27, 53,
    44, 55, 65, 55, 35, 32, 32, 31, 24, 38, 40, 58, 61, 66, 39, 33, 33, 32, 21, 27,
    47, 47, 63, 61, 56, 33, 32, 34, 32, 27, 41, 45, 65, 65, 54, 43, 37, 38, 33, 34,
    38, 57, 56, 55, 54, 51, 31, 32, 31, 34, 29, 46, 51, 57, 51, 54, 38, 32, 34, 33,
    33, 31, 45, 58, 51, 59, 43, 32, 31, 33, 25, 28, 43, 47, 61, 54, 56, 36, 34, 34,
    32, 24, 44, 45, 57, 67, 70, 35, 32, 32, 32, 24, 33, 51, 48, 52, 56, 41, 34, 33,
    32, 22, 29, 51, 55, 52, 55, 51, 36, 30, 32, 29, 26, 45, 43, 63, 63, 49, 39, 32,
    31,
};

struct CreditLoopResult final {
    std::uint64_t chunks{};
    aa::audio::RouterStats stats{};
    std::optional<aa::audio::RenderFailureReason> failure{};
    std::uint64_t stored_backpressure{};
};

// Closed credit loop of a stop-and-wait phone and the receiver, in virtual
// time, through the real incremental router. The phone captures a 2048-frame
// chunk every 42.67 ms and sends the oldest one when it holds the credit; it
// arrives half a round trip later and is stamped at arrival (as the session
// does). The receiver acks when the write completes: at once, unless the
// pulsesink ring buffer (200 ms buffer-time + 21.3 ms latency-time + slack;
// window 7's last write completed 232 ms before its end) is full. The ack
// reaches the phone half a round trip later. The round trip of a chunk sent
// in second s is round_trip_ms(s).
template <typename RoundTrip>
[[nodiscard]] CreditLoopResult run_credit_loop(std::int64_t catch_up_ns, std::int64_t seconds,
                                               RoundTrip round_trip_ms) {
    constexpr std::int64_t kSinkAheadNs = 232'000'000;
    constexpr std::uint64_t kChunkFrames = 2048;
    constexpr std::int64_t kStart = 1'000'000'000;
    BurstRoute burst{catch_up_ns, std::nullopt,
        [](const aa::audio::EncodedChunk& chunk) -> aa::core::Result<std::vector<std::byte>> {
            return std::vector<std::byte>(chunk.payload.begin(), chunk.payload.end());
        }};
    CreditLoopResult result;
    if (!burst.route) { result.failure = aa::audio::RenderFailureReason::missing_sink; return result; }
    const auto chunk = constant_pcm({kMediaLevel, kChunkFrames});
    std::int64_t credit_at = 0;
    for (std::uint64_t k = 0;; ++k) {
        const auto captured = static_cast<std::int64_t>(k * kChunkFrames * 1'000'000'000ULL / 48000ULL);
        const std::int64_t sent = std::max(captured, credit_at);
        if (sent >= seconds * 1'000'000'000LL) { break; }
        const std::int64_t rtt = round_trip_ms(sent / 1'000'000'000LL) * 1'000'000LL;
        const std::int64_t arrival = kStart + sent + rtt / 2;
        if (!burst.route->submit(make_message(aa::protocol::ChannelRole::media_audio, k,
                                              aa::core::Nanoseconds{arrival}, chunk))) {
            result.failure = aa::audio::RenderFailureReason::closed;
            break;
        }
        ++result.chunks;
        if (auto drained = burst.drain(); !drained) {
            const auto& failure = burst.route->report().render_failure;
            result.failure = failure ? failure->reason : aa::audio::RenderFailureReason::closed;
            if (failure) { result.stored_backpressure = failure->stored_backpressure.count; }
            break;
        }
        const std::int64_t completed = std::max(arrival, burst.written_end_ns() - kSinkAheadNs);
        credit_at = completed - kStart + (rtt - rtt / 2);
    }
    result.stats = burst.route->report().stats;
    return result;
}

} // namespace

// Root cause of the wireless audio failures (windows 7 and 8): the recorded
// credit round trips, not the router. Replaying window 8's own ack round trips
// (21-89 ms against a 42.67 ms chunk) makes the stop-and-wait phone alternate
// between bursts faster than real time (trimmed) and droughts (gap silence).
// With the 90 ms catch-up it survives but loses audio and plays silence, as
// window 8 did (13.2% trimmed, 13.4% silence); with the wireless default (off)
// a burst refills the sink and the next chunk is judged >200 ms late with no
// recorded backpressure, the session-ending failure of window 7.
TEST_F(AudioPipelineRoute, Window8CreditRoundTripsTrimAndSilenceAndEndInLateWithoutCatchUp) {
    const auto recorded = [](std::int64_t second) {
        return kWindow8AckRoundTripMs.at(static_cast<std::size_t>(
            std::min(second, static_cast<std::int64_t>(kWindow8AckRoundTripMs.size()) - 1)));
    };
    const auto caught_up = run_credit_loop(90'000'000, 240, recorded);
    ASSERT_FALSE(caught_up.failure);
    const auto decoded = caught_up.stats.frames_decoded;
    EXPECT_GT(caught_up.chunks, 5600U);
    EXPECT_GT(caught_up.stats.frames_trimmed * 20, decoded);   // > 5% dropped
    EXPECT_GT(caught_up.stats.frames_caught_up * 50, decoded); // > 2% whole chunks
    EXPECT_GT(caught_up.stats.frames_silence * 20, decoded);   // > 5% gap silence

    const auto off = run_credit_loop(0, 240, recorded);
    ASSERT_TRUE(off.failure);
    EXPECT_EQ(*off.failure, aa::audio::RenderFailureReason::late);
    EXPECT_EQ(off.stored_backpressure, 0U);
    EXPECT_LT(off.chunks, 1000U);
}

// The same router and policy are lossless whenever the credit round trip stays
// under the chunk period: the phone is then paced by capture, not by acks, and
// arrival stamps track real time. That is what removing the Nagle hold on the
// receiver's small ack frames (TCP_NODELAY) restores on Wi-Fi; no router
// policy change is needed for it.
TEST_F(AudioPipelineRoute, CreditRoundTripsUnderTheChunkPeriodAreLossless) {
    for (const std::int64_t rtt_ms : {5, 16, 30, 40}) {
        for (const std::int64_t catch_up_ns : {std::int64_t{0}, std::int64_t{90'000'000}}) {
            const auto run = run_credit_loop(catch_up_ns, 120, [rtt_ms](std::int64_t) { return rtt_ms; });
            ASSERT_FALSE(run.failure) << rtt_ms;
            EXPECT_EQ(run.stats.frames_trimmed, 0U) << rtt_ms;
            EXPECT_EQ(run.stats.frames_silence, 0U) << rtt_ms;
            EXPECT_EQ(run.stats.frames_retained, run.stats.frames_decoded) << rtt_ms;
        }
    }
}

namespace {

// Window 9 (A26 over Wi-Fi, 2026-11-20, #180 deployed: TCP_NODELAY), the
// phone's per-second media ack round trips in ms ("CAR.AUDIO.STATS: Acks from
// MEDIA received ... with avg latency", acks:latency): 27 s before the
// session ended `late` (~218 ms behind the cursor) right after the 12:83 ms
// second. Mostly 9-42 ms at 22-25 acks/s, against 21-89 ms in window 8.
constexpr std::array<std::int64_t, 27> kWindow9AckRoundTripMs{
    16, 13, 37, 29, 38, 42, 39, 28, 9, 12,
    9, 9, 13, 38, 36, 39, 32, 14, 9, 11,
    11, 12, 26, 33, 57, 83, 35,
};

template <std::size_t N>
[[nodiscard]] auto recorded_round_trips(const std::array<std::int64_t, N>& seconds) {
    return [&seconds](std::int64_t second) {
        return seconds.at(static_cast<std::size_t>(
            std::clamp<std::int64_t>(second, 0, static_cast<std::int64_t>(N) - 1)));
    };
}

struct PlayoutLoopResult final {
    std::uint64_t chunks{};
    aa::audio::RouterStats stats{};
    std::optional<aa::audio::RenderFailureReason> failure{};
    std::vector<aa::audio::WriteCompletion> completions{};
    // Most chunks the phone had sent and not yet seen acknowledged.
    std::uint64_t max_in_flight{};
    // After each chunk was rendered: its arrival, the router's playout offset,
    // and the frames trimmed and underruns so far.
    struct Sample final {
        std::int64_t arrival;
        std::int64_t offset;
        std::uint64_t trimmed;
        std::uint64_t underruns;
        std::uint64_t shed{};
    };
    std::vector<Sample> offsets{};
};

// The credit loop of run_credit_loop() with a credit window: the phone may
// keep `window` chunks unacknowledged, so it sends each 2048-frame chunk as
// soon as it is captured unless `window` earlier chunks still await their
// ack. TCP delivers in order, half a round trip after sending. The render
// worker acks a chunk when it is queued, not when it is written, until its
// credit-scaled reservation is full: about 500 ms of A26 media queued behind
// the 232 ms pulsesink ring. The ack reaches the phone half a round trip
// later. The receiver runs the real incremental
// router with the wireless playout policy (PlayoutPolicy, production depth).
template <typename RoundTrip>
[[nodiscard]] PlayoutLoopResult run_playout_loop(std::int64_t seconds, RoundTrip round_trip_ms,
                                                 std::uint32_t window,
                                                 aa::core::Nanoseconds depth = aa::audio::kWirelessPlayoutDepth,
                                                 std::uint64_t burst = 1, std::uint64_t later_burst = 0,
                                                 std::int64_t burst_change_s = 0,
                                                 std::int64_t phone_buffer_ns = 0,
                                                 std::int64_t burst_end_s = 0) {
    constexpr std::int64_t kSinkAheadNs = 232'000'000 + 500'000'000;
    constexpr std::uint64_t kChunkFrames = 2048;
    constexpr std::int64_t kStart = 1'000'000'000;
    PlayoutLoopResult result;
    auto config = pcm_router_config();
    config.live_pcm_drift.enabled = true;
    config.playout.enabled = true;
    config.playout.depth = depth;
    auto completions = std::make_shared<std::vector<aa::audio::WriteCompletion>>();
    config.on_write_completed = [completions](const aa::audio::WriteCompletion& completion) {
        completions->push_back(completion);
    };
    auto sink = std::make_unique<RecordingAudioSink>();
    const auto observed = sink->state;
    auto opened = aa::audio::detail::open_router_with_sink_for_test(config, std::move(sink));
    if (!opened) { result.failure = aa::audio::RenderFailureReason::missing_sink; return result; }
    auto route = std::move(opened).value();
    const auto written_end_ns = [&]() -> std::int64_t {
        if (observed->write_ns.empty()) { return 0; }
        const auto frames = static_cast<std::int64_t>(observed->writes.back().size() / 4);
        return observed->write_ns.back() + frames * 1'000'000'000LL / 48000;
    };
    const auto chunk = constant_pcm({kMediaLevel, kChunkFrames});
    std::deque<std::int64_t> acked_at; // phone time each in-flight chunk's ack arrives
    std::int64_t last_arrival = 0;
    for (std::uint64_t k = 0;; ++k) {
        // The phone releases chunks in groups of `burst`, when the group's last
        // chunk is captured (window 10: depths up to 274 ms = 3.6 chunks ahead).
        const auto change_chunk = static_cast<std::uint64_t>(burst_change_s) * 48000ULL / kChunkFrames;
        const auto end_chunk = static_cast<std::uint64_t>(burst_end_s) * 48000ULL / kChunkFrames;
        auto released = (k / burst) * burst + burst - 1;
        if (later_burst != 0 && burst_end_s > 0 && k >= end_chunk) {
            released = k; // back to single chunks
        } else if (later_burst != 0 && k >= change_chunk) {
            const auto j = k - change_chunk;
            released = change_chunk + (j / later_burst) * later_burst + later_burst - 1;
        } else if (later_burst != 0) {
            released = std::min<std::uint64_t>(released, change_chunk - 1);
        }
        const auto captured = static_cast<std::int64_t>(released * kChunkFrames * 1'000'000'000ULL / 48000ULL);
        std::int64_t sent = captured;
        if (acked_at.size() >= window) {
            sent = std::max(sent, acked_at.front());
            acked_at.pop_front();
        }
        if (sent >= seconds * 1'000'000'000LL) { break; }
        // The A26 records into a 65536-byte AudioRecord buffer (341 ms): a
        // chunk still unsent that long after its capture was overwritten.
        if (phone_buffer_ns > 0 && sent - captured > phone_buffer_ns) {
            acked_at.push_front(sent);
            continue;
        }
        std::uint64_t in_flight = 1;
        for (const auto at : acked_at) { if (at > sent) { ++in_flight; } }
        result.max_in_flight = std::max(result.max_in_flight, in_flight);
        const std::int64_t rtt = round_trip_ms(sent / 1'000'000'000LL) * 1'000'000LL;
        // TCP delivers in order; co-arriving chunks still get distinct
        // receiver stamps, ~100 us apart as on the live link (Neon P2-3).
        const std::int64_t arrival = std::max(last_arrival + (last_arrival != 0 ? 100'000 : 0), kStart + sent + rtt / 2);
        last_arrival = arrival;
        if (!route.submit(make_message(aa::protocol::ChannelRole::media_audio, k,
                                       aa::core::Nanoseconds{arrival}, chunk))) {
            result.failure = aa::audio::RenderFailureReason::closed;
            break;
        }
        ++result.chunks;
        bool drained = false;
        for (int i = 0; i < 256; ++i) {
            auto stepped = route.render_step();
            if (!stepped) { break; }
            if (!stepped.value().pending) { drained = true; break; }
        }
        if (!drained) {
            const auto& failure = route.report().render_failure;
            result.failure = failure ? failure->reason : aa::audio::RenderFailureReason::closed;
            break;
        }
        const std::int64_t acked = std::max(arrival, written_end_ns() - kSinkAheadNs);
        acked_at.push_back(acked - kStart + (rtt - rtt / 2));
        result.offsets.push_back({arrival, route.report().stats.playout_offset.count,
                                  route.report().stats.frames_trimmed, route.report().stats.playout_underruns,
                                  route.report().stats.frames_shed});
    }
    result.stats = route.report().stats;
    result.completions = *completions;
    return result;
}

// audio_write_completed's content schedule (scheduled - playout offset) must
// strictly increase: the collector drops a write whose scheduled_ns does not.
void expect_monotonic_content_schedule(const PlayoutLoopResult& run, const char* label) {
    std::int64_t last_end = INT64_MIN;
    std::size_t backwards = 0;
    for (const auto& completion : run.completions) {
        const auto start = completion.scheduled.count - completion.playout_offset.count;
        const auto end = completion.scheduled_end.count - completion.playout_end_offset.count;
        if (start < last_end) { ++backwards; }
        EXPECT_GT(end, start) << label;
        last_end = end;
        EXPECT_GE(completion.playout_offset.count, 0) << label;
        EXPECT_GE(completion.playout_end_offset.count, 0) << label;
    }
    EXPECT_EQ(backwards, 0U) << label;
}

void expect_lossless_playout(const PlayoutLoopResult& run, const char* label) {
    ASSERT_FALSE(run.failure) << label;
    EXPECT_GT(run.chunks, 0U) << label;
    EXPECT_EQ(run.stats.frames_trimmed, 0U) << label;
    EXPECT_EQ(run.stats.frames_silence, 0U) << label;
    EXPECT_EQ(run.stats.frames_skipped, 0U) << label;
    EXPECT_EQ(run.stats.playout_underruns, 0U) << label;
    EXPECT_EQ(run.stats.frames_retained, run.stats.frames_decoded) << label;
    EXPECT_EQ(run.stats.playout_anchors, 1U) << label;
}

} // namespace

// Window 9 ran #180 (TCP_NODELAY) with the arrival model and stop-and-wait:
// the phone still lost credit round trips to every Wi-Fi spike, trimming 5.8%
// and filling 8.4% with silence before ending `late`. Replaying its own round
// trips through the arrival-stamped router with max_unacked 1 loses audio the
// same way (this model has no Wi-Fi burst beyond the round trips, so it trims
// less and does not reach the 200 ms cap); playout with the credit window
// below loses none.
TEST_F(AudioPipelineRoute, Window9RoundTripsCostTheStopAndWaitArrivalModelTrimsAndSilence) {
    const auto run = run_credit_loop(0, 27, recorded_round_trips(kWindow9AckRoundTripMs));
    ASSERT_FALSE(run.failure);
    EXPECT_GT(run.stats.frames_trimmed, 0U);
    EXPECT_GT(run.stats.frames_silence * 50, run.stats.frames_decoded); // > 2% gap silence
}

// Credit window + playout (owner decision 2026-11-20). With the wireless
// credit window the phone never waits for a round trip under 4 x 42.67 ms, so
// a spike delays arrivals by its one-way time instead of stalling the phone,
// and the 120 ms playout buffer absorbs that. Windows 8 and 9's recorded round
// trips then play every decoded frame: nothing trimmed, no gap silence, no
// underrun, one anchor.
TEST_F(AudioPipelineRoute, PlayoutWithCreditWindowPlaysWindow8And9Losslessly) {
    const auto window8 = run_playout_loop(240, recorded_round_trips(kWindow8AckRoundTripMs),
                                          aa::audio::kWirelessCreditWindow);
    expect_lossless_playout(window8, "window 8");
    EXPECT_GT(window8.chunks, 5600U);
    const auto window9 = run_playout_loop(27, recorded_round_trips(kWindow9AckRoundTripMs),
                                          aa::audio::kWirelessCreditWindow);
    expect_lossless_playout(window9, "window 9");
    EXPECT_GT(window9.chunks, 600U);
    // The phone really used more than one credit at once.
    EXPECT_GT(window8.max_in_flight, 1U);
    EXPECT_LE(window8.max_in_flight, aa::audio::kWirelessCreditWindow);
    // The buffer stayed inside its target band the whole time.
    ASSERT_TRUE(window8.stats.playout_depth_min && window8.stats.playout_depth_max);
    EXPECT_GT(window8.stats.playout_depth_min->count, 0);
    EXPECT_LE(window8.stats.playout_depth_max->count,
              aa::audio::kWirelessPlayoutDepth.count + 20'000'000);
}

// Synthetic Wi-Fi spikes inside the buffer: round trips of 83-150 ms for 1-2 s
// on a 20 ms link (one-way excursion <= 65 ms < 120 ms) cost nothing.
TEST_F(AudioPipelineRoute, PlayoutAbsorbsRoundTripSpikesInsideTheBuffer) {
    for (const auto& [spike_ms, spike_s] : std::vector<std::pair<std::int64_t, std::int64_t>>{
             {83, 1}, {83, 2}, {120, 1}, {150, 1}, {150, 2}}) {
        const auto rtt = [spike_ms, spike_s](std::int64_t second) {
            return second >= 10 && second < 10 + spike_s ? spike_ms : std::int64_t{20};
        };
        const auto run = run_playout_loop(30, rtt, aa::audio::kWirelessCreditWindow);
        const auto label = std::to_string(spike_ms) + " ms x " + std::to_string(spike_s) + " s";
        expect_lossless_playout(run, label.c_str());
    }
}

// The rejected alternative: a jitter buffer with one chunk in flight. The
// stop-and-wait phone cannot send faster than one chunk per round trip, so a
// 150 ms spike starves the buffer however deep it is.
TEST_F(AudioPipelineRoute, PlayoutWithOneChunkInFlightStillUnderrunsOnASpike) {
    const auto rtt = [](std::int64_t second) {
        return second >= 10 && second < 12 ? std::int64_t{150} : std::int64_t{20};
    };
    const auto run = run_playout_loop(30, rtt, 1);
    ASSERT_FALSE(run.failure);
    EXPECT_GT(run.stats.playout_underruns, 0U);
}

// A drought longer than the buffer (a 1 s spike to an 800 ms round trip: the
// one-way delay jumps by 390 ms) underruns gracefully: the role re-anchors,
// the elapsed hole is skipped without a write, the refill is bounded silence,
// the phone's backlog that follows is bled back to the target with the bounded
// drift trim, and the session never fails `late`.
TEST_F(AudioPipelineRoute, PlayoutDroughtBeyondTheBufferUnderrunsBoundedNotLate) {
    const auto rtt = [](std::int64_t second) {
        return second == 10 ? std::int64_t{800} : std::int64_t{20};
    };
    const auto run = run_playout_loop(60, rtt, aa::audio::kWirelessCreditWindow);
    ASSERT_FALSE(run.failure);
    // An 800 ms round trip still lets the four credits through in bursts, so
    // the drought is a few separate holes, each one bounded re-anchor.
    EXPECT_GE(run.stats.playout_underruns, 1U);
    EXPECT_LE(run.stats.playout_underruns, 4U);
    // The hole was skipped, not synthesized.
    EXPECT_GT(run.stats.frames_skipped, 0U);
    // Refill silence: at most one buffer per underrun, plus bounded pads.
    const auto depth_frames = static_cast<std::uint64_t>(aa::audio::kWirelessPlayoutDepth.count) * 48 / 1'000'000;
    EXPECT_LE(run.stats.frames_silence - run.stats.frames_padded,
              run.stats.playout_underruns * (depth_frames + 1));
    // The phone's backlog then arrives on top of a refilled buffer. The drift
    // trim (at most 5 ms per chunk, never a catch-up) removes exactly the
    // latency the drought added - its skipped holes plus the refills - and no
    // more: lossless playback of a longer drought would need unbounded latency.
    EXPECT_EQ(run.stats.frames_caught_up, 0U);
    EXPECT_GT(run.stats.frames_trimmed, 0U);
    EXPECT_LE(run.stats.frames_trimmed, run.stats.frames_skipped + run.stats.frames_silence + 2048U);
    EXPECT_LE(run.stats.frames_trimmed * 100, run.stats.frames_decoded * 3); // < 3% of the minute
    // The buffer is back at its target by the end.
    EXPECT_LE(std::abs(run.stats.playout_offset.count - aa::audio::kWirelessPlayoutDepth.count),
              20'000'000);
}

// DeepSeek F1 / window 10: with a credit window the phone releases several
// chunks at once (window 10's depths reached 274 ms). Each early chunk raises
// a single depth by up to a chunk period per position in the burst, but the
// burst drains by itself: the drift correction must not trim it (window 10
// trimmed 0.81%, thinned the buffer to 1 ms and then underran 13 times on
// ordinary 3-28 ms jitter).
TEST_F(AudioPipelineRoute, PlayoutCoalescedBurstsAreNotTrimmed) {
    for (const std::uint64_t burst : {2U, 3U, 4U}) {
        const auto steady = run_playout_loop(30, [](std::int64_t) { return std::int64_t{20}; },
                                             aa::audio::kWirelessCreditWindow,
                                             aa::audio::kWirelessPlayoutDepth, burst);
        const auto label = "burst " + std::to_string(burst);
        expect_lossless_playout(steady, label.c_str());
        expect_monotonic_content_schedule(steady, label.c_str());
        // Bursts plus a spike inside the buffer.
        const auto spiked = run_playout_loop(30, [](std::int64_t second) {
            return second >= 10 && second < 12 ? std::int64_t{120} : std::int64_t{20};
        }, aa::audio::kWirelessCreditWindow, aa::audio::kWirelessPlayoutDepth, burst);
        expect_lossless_playout(spiked, (label + " + spike").c_str());
    }
}

// Neon P1-1: a sustained slower path (the round trip steps from 20 to 230 ms
// for good, so every arrival is 105 ms later) thins the buffer below its
// floor for longer than the drift window, so the router pads it back. The
// padded run must then play: a cached pad re-applied to the advancing cursor
// wrote pad silence forever.
TEST_F(AudioPipelineRoute, PlayoutPadForASustainedSlowerPathPlaysTheRun) {
    const auto run = run_playout_loop(30, [](std::int64_t second) {
        return second >= 5 ? std::int64_t{230} : std::int64_t{20};
    }, 8U /* AudioCreditPolicy::kMaxWindow */);
    ASSERT_FALSE(run.failure);
    EXPECT_GT(run.stats.frames_padded, 0U);
    EXPECT_EQ(run.stats.playout_underruns, 0U);
    EXPECT_EQ(run.stats.frames_trimmed, 0U);
    EXPECT_EQ(run.stats.frames_retained, run.stats.frames_decoded);
    // Padding is bounded: it restores the buffer, then stops.
    EXPECT_LE(run.stats.frames_padded, 48U * 50U);
    expect_monotonic_content_schedule(run, "pad");
}

// Neon P2-4: every schedule the drought scenario produces (underruns, refills,
// the backlog's drift trims) stays monotonic on the content clock.
TEST_F(AudioPipelineRoute, PlayoutContentScheduleStaysMonotonicThroughADrought) {
    const auto run = run_playout_loop(40, [](std::int64_t second) {
        return second == 10 ? std::int64_t{800} : std::int64_t{20};
    }, aa::audio::kWirelessCreditWindow);
    ASSERT_FALSE(run.failure);
    EXPECT_GE(run.stats.playout_underruns, 1U);
    expect_monotonic_content_schedule(run, "drought");
}

// Neon P2-3: two re-anchors in one render step (media underruns while a
// guidance stream starts 300 ms later, both queued behind a busy write) each
// refill up to the playout depth. Together that is over the 200 ms aggregate
// silence cap, which used to end the session; a re-anchor refill is bounded by
// the depth on its own and is not synthesized gap silence.
TEST_F(AudioPipelineRoute, PlayoutTwoReanchorsInOneStepSurviveTheAggregateCap) {
    auto config = pcm_router_config();
    config.live_pcm_drift.enabled = true;
    config.playout.enabled = true;
    auto opened = aa::audio::detail::open_router_with_sink_for_test(
        config, std::make_unique<RecordingAudioSink>());
    ASSERT_TRUE(opened);
    auto route = std::move(opened).value();
    const auto chunk = constant_pcm({kMediaLevel, 2048});
    const auto drain = [&] {
        for (int i = 0; i < 512; ++i) {
            auto stepped = route.render_step();
            if (!stepped) { return false; }
            if (!stepped.value().pending) { return true; }
        }
        return false;
    };
    constexpr std::int64_t kChunk = 2048LL * 1'000'000'000LL / 48000LL;
    for (std::uint64_t k = 0; k < 10; ++k) {
        ASSERT_TRUE(route.submit(make_message(media, k,
            aa::core::Nanoseconds{base.count + static_cast<std::int64_t>(k) * kChunk}, chunk)));
        ASSERT_TRUE(drain());
    }
    // Media's next chunk is 200 ms late; guidance starts 300 ms after it.
    const aa::core::Nanoseconds late{base.count + 10 * kChunk + 200'000'000};
    ASSERT_TRUE(route.submit(make_message(media, 10, late, chunk)));
    ASSERT_TRUE(route.submit(make_message(guidance, 1,
        aa::core::Nanoseconds{late.count + 300'000'000}, chunk)));
    ASSERT_TRUE(drain()) << (route.report().render_failure
        ? static_cast<int>(route.report().render_failure->reason) : -1);
    const auto& stats = route.report().stats;
    EXPECT_EQ(stats.playout_underruns, 1U);
    EXPECT_EQ(stats.playout_anchors, 2U);
    EXPECT_EQ(stats.frames_retained, 12U * 2048U);
}

// Neon P3: an idle authorization the worker recorded owns the frames it covers
// (frames_idle_skipped), even when a playout re-anchor's hole overlaps it.
TEST_F(AudioPipelineRoute, PlayoutResumeHoleInsideRecordedIdleCountsAsIdle) {
    auto config = pcm_router_config();
    config.live_pcm_drift.enabled = true;
    config.playout.enabled = true;
    auto opened = aa::audio::detail::open_router_with_sink_for_test(
        config, std::make_unique<RecordingAudioSink>());
    ASSERT_TRUE(opened);
    auto route = std::move(opened).value();
    const auto chunk = constant_pcm({kMediaLevel, 2048});
    constexpr std::int64_t kChunk = 2048LL * 1'000'000'000LL / 48000LL;
    for (std::uint64_t k = 0; k < 4; ++k) {
        ASSERT_TRUE(route.submit(make_message(media, k,
            aa::core::Nanoseconds{base.count + static_cast<std::int64_t>(k) * kChunk}, chunk)));
        for (int i = 0; i < 64; ++i) {
            auto stepped = route.render_step();
            ASSERT_TRUE(stepped);
            if (!stepped.value().pending) { break; }
        }
    }
    // The role's stream ended at base + depth + 4 chunks; idle from then to the resume.
    const auto ended = base.count + aa::audio::kWirelessPlayoutDepth.count + 4 * kChunk;
    const aa::core::Nanoseconds resumed{ended + 2'000'000'000LL};
    route.note_all_role_idle(aa::core::Nanoseconds{ended}, resumed);
    ASSERT_TRUE(route.submit(make_message(media, 4, resumed, chunk)));
    for (int i = 0; i < 64; ++i) {
        auto stepped = route.render_step();
        ASSERT_TRUE(stepped);
        if (!stepped.value().pending) { break; }
    }
    const auto& stats = route.report().stats;
    EXPECT_GT(stats.frames_idle_skipped, 48000U * 19 / 10);
    EXPECT_EQ(stats.frames_skipped, 0U);
}

// N1 (both rechecks): which delay does audio really play with when the phone
// delivers in groups? The A26 captures with a real-time AudioRecord (window
// 11: "Start capturing system audio ... expected frame duration: 42.666667
// ms") and acks stay at real-time rate, so it never produces ahead: a group's
// chunks were captured on time and delivered late, its last one on time. The
// least-delayed arrival therefore marks on-time content, and the playout
// offset (what audio_write_completed reports and the video hold matches) is
// the depth of that arrival: depth + (burst - 1) chunk periods here, since the
// anchor was a group's first, late, chunk. Every write reports that delay
// from the chunk's on-time capture.
TEST_F(AudioPipelineRoute, PlayoutOffsetIsTheDelayFromOnTimeContentUnderBursts) {
    constexpr std::int64_t kChunk = 2048LL * 1'000'000'000LL / 48000LL;
    for (const std::uint64_t burst : {1U, 2U, 3U, 4U, 5U}) {
        const auto run = run_playout_loop(20, [](std::int64_t) { return std::int64_t{20}; },
                                          aa::audio::kWirelessCreditWindow + 1,
                                          aa::audio::kWirelessPlayoutDepth, burst);
        const auto label = "burst " + std::to_string(burst);
        expect_lossless_playout(run, label.c_str());
        const auto expected = aa::audio::kWirelessPlayoutDepth.count
            + static_cast<std::int64_t>(burst - 1) * kChunk;
        EXPECT_NEAR(static_cast<double>(run.stats.playout_offset.count),
                    static_cast<double>(expected), 1e6) << label;
        // Every write after the first group reports the same delay.
        ASSERT_GT(run.completions.size(), 20U) << label;
        for (std::size_t i = 10; i < run.completions.size(); ++i) {
            EXPECT_NEAR(static_cast<double>(run.completions[i].playout_offset.count),
                        static_cast<double>(expected), 1e6) << label << " write " << i;
        }
        // The reported content start is the on-time arrival of the chunk that
        // plays: chunk k is captured at k periods and on time 10 ms later.
        const auto& last = run.completions.back();
        const auto content = last.scheduled.count - last.playout_offset.count;
        const auto chunk_index = (content - 1'000'000'000LL - 10'000'000LL + kChunk / 2) / kChunk;
        EXPECT_NEAR(static_cast<double>(content),
                    static_cast<double>(1'000'000'000LL + 10'000'000LL + chunk_index * kChunk), 1e6) << label;
    }
}

// N2: the pad must be reachable while the phone bursts. A group's last chunk
// always sits a whole span above its first, so judging the pad on the
// best-buffered arrival never fires for burst >= 3; the same sustained slower
// path then thins the buffer to an underrun.
TEST_F(AudioPipelineRoute, PlayoutPadForASustainedSlowerPathWorksWhileThePhoneBursts) {
    for (const std::uint64_t burst : {3U, 4U}) {
        // One-way +105 ms for good leaves the latest chunk 15 ms of buffer.
        const auto run = run_playout_loop(40, [](std::int64_t second) {
            return second >= 5 ? std::int64_t{230} : std::int64_t{20};
        }, 8U /* AudioCreditPolicy::kMaxWindow */, aa::audio::kWirelessPlayoutDepth, burst);
        const auto label = "burst " + std::to_string(burst);
        ASSERT_FALSE(run.failure) << label;
        EXPECT_GT(run.stats.frames_padded, 0U) << label;
        EXPECT_EQ(run.stats.playout_underruns, 0U) << label;
        EXPECT_EQ(run.stats.frames_trimmed, 0U) << label;
        EXPECT_EQ(run.stats.frames_retained, run.stats.frames_decoded) << label;
        expect_monotonic_content_schedule(run, label.c_str());
    }
}

// Neon P2-3: after a drought the phone's backlog sits on top of a refilled
// buffer. A single late arrival (the re-anchor chunk itself) used to hold the
// window's minimum for the whole 4 s window, so the drain began 4 s late. It
// must begin within 1.5 s of the last underrun. The drain itself is bounded
// by max_trim (5 ms per 42.67 ms chunk), so a ~700 ms backlog takes ~6 s; the
// video hold follows the offset meanwhile (N1), so A/V stays aligned.
TEST_F(AudioPipelineRoute, PlayoutBacklogAfterADroughtStartsDrainingPromptly) {
    const auto run = run_playout_loop(40, [](std::int64_t second) {
        return second == 10 ? std::int64_t{800} : std::int64_t{20};
    }, aa::audio::kWirelessCreditWindow);
    ASSERT_FALSE(run.failure);
    ASSERT_GE(run.stats.playout_underruns, 1U);
    std::optional<std::int64_t> last_underrun;
    std::optional<std::int64_t> first_trim;
    for (std::size_t i = 1; i < run.offsets.size(); ++i) {
        const auto& sample = run.offsets[i];
        if (sample.underruns > run.offsets[i - 1].underruns) { last_underrun = sample.arrival; first_trim.reset(); }
        if (last_underrun && !first_trim && sample.trimmed > run.offsets[i - 1].trimmed) { first_trim = sample.arrival; }
    }
    ASSERT_TRUE(last_underrun);
    ASSERT_TRUE(first_trim);
    EXPECT_LE(*first_trim - *last_underrun, 1'500'000'000LL);
    // Back inside its band once the backlog is gone. The only pads are the
    // floor's last-resort refills while the drought thinned the buffer.
    const auto& last = run.offsets.back();
    EXPECT_LE(last.offset, aa::audio::kWirelessPlayoutDepth.count + 25'000'000);
    // Neon P2-3: the buffer never thinned below its floor here, so any pad
    // is a trimmed entry coming back when the recovery window widens.
    EXPECT_EQ(run.stats.frames_padded, 0U);
}

// Neon P1-1 / R3: the reported offset (which the video hold follows) moves by
// at most half the output time that passes, in both directions. Like window
// 11's 10 s pattern of ack latency (~10 ms, then 40-95 ms), the one-way delay
// rises by 50 ms for 6 s: once the low-delay arrivals leave the 4 s window the
// offset (the least-delayed arrival's depth) falls 50 ms with no trim and no
// pad. Reported at once, every video frame after the drop was pinned behind
// the last held one's due: a freeze, then an excursion of (drop - frame
// period). When the delay falls again the offset rises, half-rate.
TEST_F(AudioPipelineRoute, PlayoutReportedOffsetSlewsDownAsWellAsUp) {
    const auto run = run_playout_loop(30, [](std::int64_t second) {
        return second >= 10 && second < 16 ? std::int64_t{120} : std::int64_t{20};
    }, aa::audio::kWirelessCreditWindow);
    ASSERT_FALSE(run.failure);
    expect_monotonic_content_schedule(run, "delay step");
    EXPECT_EQ(run.stats.frames_trimmed, 0U);
    EXPECT_EQ(run.stats.frames_padded, 0U);
    std::int64_t lowest = INT64_MAX;
    std::int64_t highest = 0;
    std::int64_t worst_drop = 0;
    for (std::size_t i = 1; i < run.completions.size(); ++i) {
        const auto& previous = run.completions[i - 1];
        const auto& current = run.completions[i];
        const auto elapsed = current.scheduled_end.count - previous.scheduled_end.count;
        const auto moved = current.playout_end_offset.count - previous.playout_end_offset.count;
        EXPECT_LE(std::abs(moved), elapsed / 2 + 1000) << "write " << i;
        worst_drop = std::max(worst_drop, -moved);
        lowest = std::min(lowest, current.playout_end_offset.count);
        highest = std::max(highest, current.playout_end_offset.count);
    }
    // It really fell by the delay step, never more than half a chunk a write.
    EXPECT_GT(highest - lowest, 45'000'000);
    EXPECT_LE(worst_drop, 22'000'000);
}

// Neon P1-2 / DeepSeek R1: after a drought the phone's backlog sits on top of
// the refilled buffer and the offset climbs above the 400 ms video hold cap,
// so audio and video diverge for as long as it stays there. The A26 keeps at
// most its 341 ms AudioRecord buffer of backlog (older audio is overwritten),
// which bounds the peak near 460 ms; a phone with a larger buffer can leave a
// deeper one. A backlog more than 350 ms deep is shed at 30% of each chunk
// instead of 5 ms, so the time above the cap is bounded, and every write
// still reports the delay it plays with (the shed stays under the reported
// offset's slew).
TEST_F(AudioPipelineRoute, PlayoutDeepBacklogIsShedUnderTheVideoHoldCap) {
    struct Case final {
        std::int64_t phone_buffer_ns;
        std::int64_t max_above_cap_ns;
    };
    for (const auto& c : {Case{341'000'000, 900'000'000}, Case{682'000'000, 2'200'000'000}}) {
        const auto run = run_playout_loop(30, [](std::int64_t second) {
            return second == 10 ? std::int64_t{800} : std::int64_t{20};
        }, aa::audio::kWirelessCreditWindow, aa::audio::kWirelessPlayoutDepth, 1, 0, 0, c.phone_buffer_ns);
        const auto label = "phone buffer " + std::to_string(c.phone_buffer_ns / 1'000'000) + " ms";
        ASSERT_FALSE(run.failure) << label;
        ASSERT_GE(run.stats.playout_underruns, 1U) << label;
        EXPECT_GT(run.stats.frames_shed, 0U) << label;
        EXPECT_GT(run.stats.playout_shed_residual_chunks, 0U) << label;
        EXPECT_GT(run.stats.playout_shed_residual_peak.count, 0) << label;
        EXPECT_EQ(run.stats.frames_caught_up, 0U) << label;
        // Output time spent reporting more than the 400 ms cap.
        std::int64_t above = 0;
        for (const auto& completion : run.completions) {
            if (completion.playout_end_offset.count > 400'000'000) {
                above += completion.scheduled_end.count - completion.scheduled.count;
            }
        }
        EXPECT_LE(above, c.max_above_cap_ns) << label;
        // The router's own estimate agrees with what was reported at the end.
        EXPECT_LE(std::abs(run.stats.playout_offset.count - run.completions.back().playout_end_offset.count),
                  5'000'000) << label;
        expect_monotonic_content_schedule(run, label.c_str());
    }
}

// DeepSeek N1 (round 4): the shed protects the 400 ms video hold cap, which
// bounds the offset - the window's largest depth. With grouped delivery that
// is a group span above the floor, so a shed gated on the floor stopped while
// the offset was still up to a span above its threshold (~520 ms at groups of
// 5 against the 400 ms hold). Gated on the offset, the excess over the cap
// stays small whatever the grouping, with the A26's 341 ms AudioRecord buffer.
TEST_F(AudioPipelineRoute, PlayoutShedKeepsTheOffsetUnderTheCapWhileThePhoneBursts) {
    for (const std::uint64_t burst : {4U, 5U}) {
        const auto run = run_playout_loop(30, [](std::int64_t second) {
            return second == 10 ? std::int64_t{800} : std::int64_t{20};
        }, aa::audio::kWirelessCreditWindow + 1, aa::audio::kWirelessPlayoutDepth, burst, 0, 0, 341'000'000);
        const auto label = "burst " + std::to_string(burst);
        ASSERT_FALSE(run.failure) << label;
        ASSERT_GE(run.stats.playout_underruns, 1U) << label;
        std::int64_t excess = 0;
        std::int64_t above = 0;
        for (const auto& completion : run.completions) {
            excess = std::max(excess, completion.playout_end_offset.count - 400'000'000);
            if (completion.playout_end_offset.count > 400'000'000) {
                above += completion.scheduled_end.count - completion.scheduled.count;
            }
        }
        EXPECT_LE(excess, 60'000'000) << label;
        EXPECT_LE(above, 900'000'000) << label;
        expect_monotonic_content_schedule(run, label.c_str());
    }
}

// Neon P2-3 (round 4): the re-anchor's co-arriving chunks carry live stamps
// microseconds apart; skipping only the exact re-anchor stamp let the next
// chunk (depth + one period) hold the recovery floor down for the whole 1 s
// window. The phone groups chunks in 4s here (DeepSeek P3-2: with single
// chunks no two share a re-anchor group, and the 10 ms group skip is never
// exercised), with the A26's 341 ms AudioRecord buffer behind the drought:
// after the underrun the shed itself starts within 0.6 s.
TEST_F(AudioPipelineRoute, PlayoutShedStartsPromptlyWithLiveArrivalStamps) {
    const auto run = run_playout_loop(30, [](std::int64_t second) {
        return second == 10 ? std::int64_t{800} : std::int64_t{20};
    }, aa::audio::kWirelessCreditWindow, aa::audio::kWirelessPlayoutDepth, 4, 0, 0, 341'000'000);
    ASSERT_FALSE(run.failure);
    std::optional<std::int64_t> last_underrun;
    std::optional<std::int64_t> first_shed;
    for (std::size_t i = 1; i < run.offsets.size(); ++i) {
        const auto& sample = run.offsets[i];
        if (sample.underruns > run.offsets[i - 1].underruns) { last_underrun = sample.arrival; first_shed.reset(); }
        if (last_underrun && !first_shed && sample.shed > run.offsets[i - 1].shed) { first_shed = sample.arrival; }
    }
    ASSERT_TRUE(last_underrun);
    ASSERT_TRUE(first_shed) << "the post-drought backlog was never shed";
    EXPECT_LE(*first_shed - *last_underrun, 600'000'000LL);
}

// Window 13: with playout, media PCM already in the output format skips the
// GStreamer round trip (an identity conversion) so the render worker's lock
// stays short under a post-stall burst; the bytes played are the bytes sent.
TEST_F(AudioPipelineRoute, PlayoutIdentityPcmIsPlayedAsSent) {
    auto config = pcm_router_config();
    config.live_pcm_drift.enabled = true;
    config.playout.enabled = true;
    auto sink = std::make_unique<RecordingAudioSink>();
    const auto observed = sink->state;
    auto opened = aa::audio::detail::open_router_with_sink_for_test(config, std::move(sink));
    ASSERT_TRUE(opened);
    auto route = std::move(opened).value();
    std::vector<std::byte> pcm(2048 * 4);
    for (std::size_t i = 0; i < pcm.size(); ++i) { pcm[i] = static_cast<std::byte>((i * 37U + 11U) & 0xffU); }
    ASSERT_TRUE(route.submit(make_message(media, 1, base, pcm)));
    for (int i = 0; i < 16; ++i) {
        auto stepped = route.render_step();
        ASSERT_TRUE(stepped);
        if (!stepped.value().pending) { break; }
    }
    std::vector<std::byte> played;
    for (const auto& write : observed->writes) { played.insert(played.end(), write.begin(), write.end()); }
    ASSERT_GE(played.size(), pcm.size());
    EXPECT_TRUE(std::equal(pcm.begin(), pcm.end(), played.end() - static_cast<std::ptrdiff_t>(pcm.size())));
}

// Neon P2-3: a backlog that builds over a second after an underrun (the round
// trip comes down 800 -> 100 -> 20 ms, never thinning the re-anchored buffer)
// straddles the end of the 4 s recovery; entries the 1 s recovery window
// skipped were trimmed meanwhile and came back below the floor when the
// window widened, padding silence into a buffer that was never thin.
TEST_F(AudioPipelineRoute, PlayoutRecoveryEndNeverPadsAHealthyBuffer) {
    const auto run = run_playout_loop(30, [](std::int64_t second) {
        if (second == 10) { return std::int64_t{800}; }
        if (second == 11) { return std::int64_t{100}; }
        return std::int64_t{20};
    }, aa::audio::kWirelessCreditWindow);
    ASSERT_FALSE(run.failure);
    ASSERT_GE(run.stats.playout_underruns, 1U);
    EXPECT_EQ(run.stats.frames_padded, 0U);
    EXPECT_LE(run.offsets.back().offset, aa::audio::kWirelessPlayoutDepth.count + 25'000'000);
}

// R8: playout without the drift policy would follow the phone's clock without
// bound, so it is refused.
TEST_F(AudioPipelineRoute, PlayoutRequiresTheDriftPolicy) {
    auto config = pcm_router_config();
    config.playout.enabled = true;
    config.live_pcm_drift.enabled = false;
    EXPECT_FALSE(aa::audio::detail::open_router_with_sink_for_test(
        config, std::make_unique<RecordingAudioSink>()));
}

TEST_F(AudioPipelineRoute, PlayoutRejectsDepthAboveSupportedMaximum) {
    auto config = pcm_router_config();
    config.playout.enabled = true;
    config.live_pcm_drift.enabled = true;
    config.playout.depth = aa::audio::kMaxPlayoutDepth;
    EXPECT_TRUE(aa::audio::detail::open_router_with_sink_for_test(
        config, std::make_unique<RecordingAudioSink>()));
    ++config.playout.depth.count;
    const auto rejected = aa::audio::detail::open_router_with_sink_for_test(
        config, std::make_unique<RecordingAudioSink>());
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().code(), aa::ErrorCode::invalid_argument);
}

// N6 / F7: playout skips the arrival model's 200 ms `late` check, so a
// backward jump of the arrival clock read as a sustained depth excess and
// trimmed real content. It fails closed again, as the arrival model does.
TEST_F(AudioPipelineRoute, PlayoutBackwardArrivalJumpFailsClosed) {
    auto config = pcm_router_config();
    config.live_pcm_drift.enabled = true;
    config.playout.enabled = true;
    auto opened = aa::audio::detail::open_router_with_sink_for_test(
        config, std::make_unique<RecordingAudioSink>());
    ASSERT_TRUE(opened);
    auto route = std::move(opened).value();
    const auto chunk = constant_pcm({kMediaLevel, 2048});
    constexpr std::int64_t kChunk = 2048LL * 1'000'000'000LL / 48000LL;
    const auto drain = [&] {
        for (int i = 0; i < 64; ++i) {
            auto stepped = route.render_step();
            if (!stepped) { return false; }
            if (!stepped.value().pending) { return true; }
        }
        return false;
    };
    for (std::uint64_t k = 0; k < 10; ++k) {
        ASSERT_TRUE(route.submit(make_message(media, k,
            aa::core::Nanoseconds{base.count + 1'000'000'000LL + static_cast<std::int64_t>(k) * kChunk}, chunk)));
        ASSERT_TRUE(drain());
    }
    ASSERT_TRUE(route.submit(make_message(media, 10, base, chunk)));
    EXPECT_FALSE(drain());
    ASSERT_TRUE(route.report().render_failure);
    EXPECT_EQ(route.report().render_failure->reason, aa::audio::RenderFailureReason::late);
    EXPECT_EQ(route.report().stats.frames_trimmed, 0U);
}

// N9: max_arrival_burst counts chunks that arrive together, not a timestamp
// that jumps backwards.
TEST_F(AudioPipelineRoute, ArrivalBurstIgnoresBackwardTimestamps) {
    open_incremental();
    enqueue(media, 1, {kMediaLevel, 960}, 50'000'000);
    enqueue(media, 2, {kMediaLevel, 960}, 0);
    enqueue(media, 3, {kMediaLevel, 960}, -1'000'000);
    EXPECT_EQ(router->report().stats.max_arrival_burst, 1U);
    enqueue(media, 4, {kMediaLevel, 960}, 20'000'000);
    enqueue(media, 5, {kMediaLevel, 960}, 20'000'000);
    EXPECT_EQ(router->report().stats.max_arrival_burst, 2U);
}

// P3: a new session or a cancelled stream starts its burst count afresh.
TEST_F(AudioPipelineRoute, ArrivalBurstResetsWithTheStream) {
    open_incremental();
    enqueue(media, 1, {kMediaLevel, 960}, 0);
    router->cancel_pending(media);
    enqueue(media, 2, {kMediaLevel, 960}, 1'000'000);
    router->reset_session_timeline();
    enqueue(media, 3, {kMediaLevel, 960}, 2'000'000);
    EXPECT_EQ(router->report().stats.max_arrival_burst, 1U);
}

// N3: every anchor of a render step is reported, with its skip, not only the
// last one (media and guidance both resume in one step).
TEST_F(AudioPipelineRoute, PlayoutReportsEveryAnchorOfAStepWithItsSkip) {
    auto config = pcm_router_config();
    config.live_pcm_drift.enabled = true;
    config.playout.enabled = true;
    auto opened = aa::audio::detail::open_router_with_sink_for_test(
        config, std::make_unique<RecordingAudioSink>());
    ASSERT_TRUE(opened);
    auto route = std::move(opened).value();
    const auto chunk = constant_pcm({kMediaLevel, 2048});
    constexpr std::int64_t kChunk = 2048LL * 1'000'000'000LL / 48000LL;
    const auto drain = [&] {
        for (int i = 0; i < 512; ++i) {
            auto stepped = route.render_step();
            if (!stepped) { return false; }
            if (!stepped.value().pending) { return true; }
        }
        return false;
    };
    ASSERT_TRUE(route.submit(make_message(media, 0, base, chunk)));
    ASSERT_TRUE(route.submit(make_message(guidance, 0, base, chunk)));
    ASSERT_TRUE(drain());
    auto events = route.take_playout_events();
    ASSERT_EQ(events.size(), 2U);
    EXPECT_EQ(events[0].kind, aa::audio::PlayoutEvent::Kind::start);
    EXPECT_EQ(events[1].kind, aa::audio::PlayoutEvent::Kind::start);
    // Both resume 3 s later in one step.
    const aa::core::Nanoseconds resumed{base.count + kChunk + 3'000'000'000LL};
    ASSERT_TRUE(route.submit(make_message(media, 1, resumed, chunk)));
    ASSERT_TRUE(route.submit(make_message(guidance, 1, resumed, chunk)));
    ASSERT_TRUE(drain());
    events = route.take_playout_events();
    ASSERT_EQ(events.size(), 2U);
    for (const auto& event : events) {
        EXPECT_EQ(event.kind, aa::audio::PlayoutEvent::Kind::resume);
        EXPECT_GT(event.late.count, 2'800'000'000LL);
        EXPECT_NEAR(static_cast<double>(event.depth.count),
                    static_cast<double>(aa::audio::kWirelessPlayoutDepth.count), 1e6);
    }
    EXPECT_TRUE(route.take_playout_events().empty());
}

// A/V alignment: each write reports the playout offset it was scheduled with,
// and that offset equals the buffer depth, so scheduled - offset is the
// content's arrival clock (the clock video frames are stamped on).
TEST_F(AudioPipelineRoute, PlayoutWritesReportTheirOffsetFromTheArrivalClock) {
    const auto run = run_playout_loop(5, [](std::int64_t) { return std::int64_t{20}; },
                                      aa::audio::kWirelessCreditWindow);
    expect_lossless_playout(run, "steady");
    ASSERT_FALSE(run.completions.empty());
    const auto& first = run.completions.front();
    // The first chunk arrives half a 20 ms round trip after kStart.
    EXPECT_EQ(first.scheduled.count - first.playout_offset.count, 1'000'000'000 + 10'000'000);
    for (const auto& completion : run.completions) {
        EXPECT_EQ(completion.playout_offset, aa::audio::kWirelessPlayoutDepth);
    }
}

// The arrival model (wired) reports no offset: its schedule is the arrival.
TEST_F(AudioPipelineRoute, ArrivalModelWritesReportNoPlayoutOffset) {
    open_incremental();
    enqueue(media, 1, {kMediaLevel, 960});
    (void)step();
    ASSERT_FALSE(completions.empty());
    for (const auto& completion : completions) {
        EXPECT_EQ(completion.playout_offset.count, 0);
    }
}

// A stream that pauses without a stop message and resumes seconds later is a
// fresh anchor, not an underrun: the elapsed pause is skipped (no 3 s of
// synthesized silence, no gap-cap failure) and only the buffer refill plays.
TEST_F(AudioPipelineRoute, PlayoutResumeAfterAPauseReanchorsWithoutUnderrun) {
    auto config = pcm_router_config();
    config.live_pcm_drift.enabled = true;
    config.playout.enabled = true;
    auto opened = aa::audio::detail::open_router_with_sink_for_test(
        config, std::make_unique<RecordingAudioSink>());
    ASSERT_TRUE(opened);
    auto route = std::move(opened).value();
    const auto chunk = constant_pcm({kMediaLevel, 2048});
    const auto drain = [&] {
        for (int i = 0; i < 512; ++i) {
            auto stepped = route.render_step();
            if (!stepped) { return false; }
            if (!stepped.value().pending) { return true; }
        }
        return false;
    };
    constexpr std::int64_t kChunk = 2048LL * 1'000'000'000LL / 48000LL;
    for (std::uint64_t k = 0; k < 10; ++k) {
        ASSERT_TRUE(route.submit(make_message(media, k,
            aa::core::Nanoseconds{base.count + static_cast<std::int64_t>(k) * kChunk}, chunk)));
        ASSERT_TRUE(drain());
    }
    const aa::core::Nanoseconds resumed{base.count + 10 * kChunk + 3'000'000'000LL};
    ASSERT_TRUE(route.submit(make_message(media, 10, resumed, chunk)));
    ASSERT_TRUE(drain());
    const auto& stats = route.report().stats;
    EXPECT_EQ(stats.playout_underruns, 0U);
    EXPECT_EQ(stats.playout_anchors, 2U);
    // The hole is the pause less the buffer that was still queued: 3 s - 120 ms.
    EXPECT_GT(stats.frames_skipped, 48000U * 28 / 10);
    const auto depth_frames = static_cast<std::uint64_t>(aa::audio::kWirelessPlayoutDepth.count) * 48 / 1'000'000;
    EXPECT_LE(stats.frames_silence, depth_frames + 1);
    EXPECT_EQ(stats.frames_retained, 11U * 2048U);
}

// A playout depth must leave the refill under the 200 ms silence cap.
TEST_F(AudioPipelineRoute, PlayoutRejectsADepthBeyondTheSilenceCap) {
    auto config = pcm_router_config();
    config.live_pcm_drift.enabled = true;
    config.playout.enabled = true;
    config.playout.depth = aa::core::Nanoseconds{aa::audio::kMaxPlayoutDepth.count + 1};
    EXPECT_FALSE(aa::audio::detail::open_router_with_sink_for_test(
        config, std::make_unique<RecordingAudioSink>()));
    config.playout.depth = aa::audio::kMaxPlayoutDepth;
    EXPECT_TRUE(aa::audio::detail::open_router_with_sink_for_test(
        config, std::make_unique<RecordingAudioSink>()));
}

// #155 F1: a non-caught-up 200 ms guidance packet placed near the 90 ms
// watermark advances cursor to timestamp + ~284 ms. A subsequent forward-
// timestamped short chunk (< 83 ms, e.g. 42.67 ms media or 64 ms guidance
// tail) must be caught up rather than failing late_beyond_cap.
TEST_F(AudioPipelineRoute, IncrementalForwardShortChunkAfterNonCaughtUpGuidanceSurvivesLateCap) {
    // (a) Multi-role: 3 x 42.67 ms media at t=0 (cursor=118 ms), 200 ms guidance
    // at t=30 ms (phase=88 ms, cursor=313 ms), then 42.67 ms media at t=40 ms.
    {
        BurstRoute burst{90'000'000, aa::audio::AudioFormat{16000, 1, 16},
            ScriptedResampler::decoder([](std::size_t, std::size_t) { return std::size_t{9600}; })};
        ASSERT_TRUE(burst.route);
        auto& route = *burst.route;
        const auto media_chunk = constant_pcm({1000, 2048});
        for (std::uint64_t i = 0; i < 3; ++i) {
            ASSERT_TRUE(route.submit(make_message(media, i, base, media_chunk)));
        }
        ASSERT_TRUE(burst.drain());
        ASSERT_TRUE(route.submit(make_message(guidance, 0,
            aa::core::Nanoseconds{base.count + 30'000'000}, guidance_parent(1500, 6400))));
        ASSERT_TRUE(burst.drain());
        ASSERT_TRUE(route.submit(make_message(media, 3,
            aa::core::Nanoseconds{base.count + 40'000'000}, media_chunk)));
        const auto drained = burst.drain();
        ASSERT_TRUE(drained) << aa::to_string(drained.error().code());
        EXPECT_FALSE(route.report().render_failure);
        EXPECT_GT(route.report().stats.frames_caught_up, 0U);
    }
    // (b) Single-role guidance: 200 ms at t=0, 200 ms at t=111 ms (phase=89 ms,
    // cursor=395 ms), then 64 ms tail (2048 B of 16 kHz mono) at t=112 ms.
    {
        BurstRoute burst{90'000'000, aa::audio::AudioFormat{16000, 1, 16},
            ScriptedResampler::decoder([](std::size_t, std::size_t buffered) { return buffered; })};
        ASSERT_TRUE(burst.route);
        auto& route = *burst.route;
        ASSERT_TRUE(route.submit(make_message(guidance, 0, base, guidance_parent(1000, 6400))));
        ASSERT_TRUE(burst.drain());
        ASSERT_TRUE(route.submit(make_message(guidance, 1,
            aa::core::Nanoseconds{base.count + 111'000'000}, guidance_parent(1100, 6400))));
        ASSERT_TRUE(burst.drain());
        ASSERT_TRUE(route.submit(make_message(guidance, 2,
            aa::core::Nanoseconds{base.count + 112'000'000}, guidance_parent(1200, 2048))));
        const auto drained = burst.drain();
        ASSERT_TRUE(drained) << aa::to_string(drained.error().code());
        EXPECT_FALSE(route.report().render_failure);
        EXPECT_GT(route.report().stats.frames_caught_up, 0U);
    }
}

// #155 F2: an on-time guidance prompt (phase == 0, no source overlap) whose
// resampler splits Packet 0 (96 + 19098 frames) must not trigger catch-up via
// carried_ns and drop ~183 ms of speech with no stall.
TEST_F(AudioPipelineRoute, IncrementalOnTimeGuidanceSplitDoesNotTriggerCarryoverCatchUp) {
    BurstRoute burst{90'000'000, aa::audio::AudioFormat{16000, 1, 16},
        ScriptedResampler::decoder([](std::size_t call, std::size_t buffered) {
            return call == 0 ? std::size_t{96} : call == 1 ? std::size_t{19098} : buffered; })};
    ASSERT_TRUE(burst.route);
    auto& route = *burst.route;
    ASSERT_TRUE(route.submit(make_message(guidance, 0, base, guidance_parent(1000, 6400))));
    ASSERT_TRUE(burst.drain());
    ASSERT_TRUE(route.submit(make_message(guidance, 1,
        aa::core::Nanoseconds{base.count + 200'000'000}, guidance_parent(2000, 6400))));
    ASSERT_TRUE(burst.drain());
    EXPECT_EQ(route.report().stats.frames_caught_up, 0U);
    EXPECT_EQ(route.report().stats.frames_trimmed, 0U);
}

TEST_F(AudioPipelineRoute, IncrementalQueuedParentsDoNotBecomeLateArrivalsBetweenSteps) {
    open_incremental();
    for (std::uint64_t i = 0; i < 12; ++i) { enqueue(media, i, {1000, 960}); }

    std::vector<std::uint64_t> completed;
    for (int i = 0; i < 48; ++i) {
        const auto result = router->render_step();
        ASSERT_TRUE(result) << i;
        EXPECT_EQ(result.value().pending, i != 47);
        for (const auto& header : result.value().completed) { completed.push_back(header.sequence); }
    }

    EXPECT_EQ(completed, (std::vector<std::uint64_t>{0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11}));
    EXPECT_EQ(flatten_i16(rec->writes), std::vector<std::int16_t>(std::size_t{12} * 960 * 2, 1000));
}

TEST_F(AudioPipelineRoute, IncrementalCancellationDuringSilencePreservesOnlyDetachedGap) {
    open_incremental();
    enqueue(media, 1, {1000, 240});
    step();
    enqueue(guidance, 2, {2000, 240}, 100'000'000);
    rec->during_write = [&] { router->cancel_pending(guidance); };

    const auto result = step();

    rec->during_write = {};
    EXPECT_FALSE(result.pending);
    EXPECT_TRUE(result.contributors.empty());
    EXPECT_TRUE(result.completed.empty());
    enqueue(media, 3, {3000, 1}, 10'000'000);
    expect_headers(step().completed, {{media, 3}});
    EXPECT_EQ(router->report().stats.frames_silence, 240U);
    EXPECT_EQ(rec->write_ns.back(), base.count + 10'000'000);
}

TEST_F(AudioPipelineRoute, IncrementalFullyTrimmedParentCompletesOnceWithoutWriting) {
    open_incremental();
    auto config = pcm_router_config();
    config.live_pcm_drift.enabled = true;
    auto sink = std::make_unique<RecordingAudioSink>();
    const auto observed = sink->state;
    auto opened = aa::audio::detail::open_router_with_sink_for_test(config, std::move(sink));
    ASSERT_TRUE(opened);
    const auto lead = constant_pcm({1000, 1440});
    ASSERT_TRUE(opened.value().submit(make_message(media, 1, base, lead)));
    ASSERT_TRUE(opened.value().pump());
    const auto tiny = constant_pcm({3000, 1});
    ASSERT_TRUE(opened.value().submit(make_message(media, 2, base, tiny)));

    const auto result = opened.value().render_step();

    ASSERT_TRUE(result);
    expect_headers(result.value().completed, {{media, 2}});
    EXPECT_FALSE(result.value().pending);
    EXPECT_TRUE(result.value().contributors.empty());
    EXPECT_EQ(observed->writes.size(), 1U);
    EXPECT_EQ(opened.value().report().stats.frames_trimmed, 1U);
    const auto empty = opened.value().render_step();
    ASSERT_TRUE(empty);
    EXPECT_TRUE(empty.value().completed.empty());
}

TEST_F(AudioPipelineRoute, IncrementalCancellationCountsOnlySuccessfullyWrittenDetachedInput) {
    open_incremental();
    enqueue(media, 1, {1000, 960});
    rec->during_write = [&] { router->cancel_pending(media); };

    const auto result = step();

    rec->during_write = {};
    expect_headers(result.contributors, {{media, 1}});
    EXPECT_TRUE(result.completed.empty());
    EXPECT_FALSE(result.pending);
    const auto& stats = router->report().stats;
    EXPECT_EQ(stats.frames_decoded, 960U);
    EXPECT_EQ(stats.frames_retained, 240U);
    EXPECT_EQ(stats.frames_trimmed, 0U);
    EXPECT_EQ(stats.frames_output, 240U);
    ASSERT_TRUE(router->flush());
    EXPECT_EQ(rec->writes.size(), 1U);
}

TEST_F(AudioPipelineRoute, IncrementalNonintegralQuantumBoundsAudioAndSilenceByFiveMilliseconds) {
    open_incremental();
    auto config = pcm_router_config();
    config.output_format.sample_rate_hz = 44100;
    config.render_quantum = aa::core::Nanoseconds{5'000'000};
    for (auto& role : config.roles) { role.input_format = config.output_format; }
    auto sink = std::make_unique<RecordingAudioSink>();
    const auto observed = sink->state;
    auto opened = aa::audio::detail::open_router_with_sink_for_test(config, std::move(sink));
    ASSERT_TRUE(opened);
    const auto first = constant_pcm({1000, 443});
    const auto second = constant_pcm({2000, 441});
    ASSERT_TRUE(opened.value().submit(make_message(media, 1, base, first)));
    ASSERT_TRUE(opened.value().submit(make_message(guidance, 2,
        aa::core::Nanoseconds{base.count + 20'000'000}, second)));

    for (int i = 0; i < 20; ++i) {
        const auto result = opened.value().render_step();
        ASSERT_TRUE(result);
        if (!result.value().pending) { break; }
        ASSERT_LT(i, 19);
    }

    std::uint64_t offset = 0;
    for (std::size_t i = 0; i < observed->writes.size(); ++i) {
        const auto frames = observed->writes[i].size() / 4;
        EXPECT_GT(frames, 0U);
        EXPECT_LE(frames, 220U);
        const auto start = static_cast<std::int64_t>(offset * 1'000'000'000 / 44100);
        const auto end = static_cast<std::int64_t>((offset + frames) * 1'000'000'000 / 44100);
        EXPECT_LE(end - start, 5'000'000);
        EXPECT_EQ(observed->write_ns[i], base.count + start);
        offset += frames;
    }
    EXPECT_EQ(offset, 1323U);
    EXPECT_EQ(opened.value().report().stats.frames_silence, 439U);
    EXPECT_EQ(opened.value().report().stats.frames_retained, 884U);
}

TEST_F(AudioPipelineRoute, IncrementalElapsedBackpressureGapAdvancesWithoutSilenceWrites) {
    open_incremental();
    enqueue(media, 1, {1000, 240});
    expect_headers(step().completed, {{media, 1}});
    enqueue(media, 2, {2000, 240}, 300'000'000);
    router->note_backpressure(base, aa::core::Nanoseconds{base.count + 300'000'000});

    const auto skipped = step();

    EXPECT_TRUE(skipped.contributors.empty());
    EXPECT_TRUE(skipped.completed.empty());
    EXPECT_TRUE(skipped.pending);
    EXPECT_EQ(rec->writes.size(), 1U);
    const auto resumed = step();
    expect_headers(resumed.completed, {{media, 2}});
    EXPECT_FALSE(resumed.pending);
    EXPECT_EQ(rec->write_ns, (std::vector<std::int64_t>{base.count, base.count + 300'000'000}));
    EXPECT_EQ(flatten_i16({rec->writes.back()}), std::vector<std::int16_t>(480, 2000));
    EXPECT_EQ(router->report().stats.frames_silence, 0U);
    EXPECT_EQ(router->report().stats.frames_skipped, 14160U);
    ASSERT_EQ(completions.size(), 2U);
    EXPECT_EQ(completions.back().scheduled.count, base.count + 300'000'000);
}

TEST_F(AudioPipelineRoute, IncrementalPartialBackpressureLeavesOversizedRemainderRejected) {
    open_incremental();
    enqueue(media, 1, {1000, 240});
    expect_headers(step().completed, {{media, 1}});
    enqueue(media, 2, {2000, 240}, 300'000'000);
    router->note_backpressure(aa::core::Nanoseconds{base.count + 250'000'000},
                              aa::core::Nanoseconds{base.count + 300'000'000});

    const auto result = router->render_step();

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code(), aa::ErrorCode::invalid_argument);
    EXPECT_EQ(rec->writes.size(), 1U);
    EXPECT_EQ(router->report().stats.frames_skipped, 0U);
    ASSERT_TRUE(router->report().render_failure);
    const auto& failure = *router->report().render_failure;
    EXPECT_EQ(failure.reason, aa::audio::RenderFailureReason::single_gap);
    EXPECT_EQ(failure.raw_gap, 14160U);
    EXPECT_EQ(failure.uncovered_gap, 11760U);
    EXPECT_EQ(failure.stored_backpressure.count, 1U);
    EXPECT_EQ(failure.stored_backpressure.start_ns, base.count + 250'000'000);
    EXPECT_EQ(failure.stored_backpressure.end_ns, base.count + 300'000'000);
    ASSERT_TRUE(failure.effective_backpressure);
    EXPECT_EQ(failure.effective_backpressure->count, 1U);
    EXPECT_EQ(failure.effective_backpressure->first_frame, 12000U);
    EXPECT_EQ(failure.effective_backpressure->last_frame, 14400U);
    EXPECT_EQ(failure.effective_backpressure->excluded_frames, 2400U);
    EXPECT_EQ(failure.stored_idle.count, 0U);
    EXPECT_FALSE(failure.stored_idle.start_ns);
    EXPECT_FALSE(failure.stored_idle_prefix);
    ASSERT_TRUE(failure.effective_idle);
    EXPECT_EQ(failure.effective_idle->excluded_frames, 0U);
}

TEST_F(AudioPipelineRoute, IncrementalFutureBackpressureGapIsNotSkippable) {
    open_incremental();
    const auto future = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count() + 3'600'000'000'000LL - base.count;
    enqueue(media, 1, {1000, 240}, future);
    expect_headers(step().completed, {{media, 1}});
    enqueue(media, 2, {2000, 240}, future + 300'000'000);
    router->note_backpressure(aa::core::Nanoseconds{base.count + future},
                              aa::core::Nanoseconds{base.count + future + 300'000'000});

    const auto result = router->render_step();

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code(), aa::ErrorCode::invalid_argument);
    EXPECT_EQ(router->report().stats.frames_skipped, 0U);
}

TEST_F(AudioPipelineRoute, IncrementalBackpressureNeverSkipsAnotherRolesPcm) {
    open_incremental();
    enqueue(media, 1, {1000, 240});
    expect_headers(step().completed, {{media, 1}});
    enqueue(guidance, 2, {2000, 80}, 100'000'000);
    enqueue(media, 3, {3000, 240}, 300'000'000);
    router->note_backpressure(base, aa::core::Nanoseconds{base.count + 300'000'000});

    for (int i = 0; i < 8 && step().pending; ++i) {}

    ASSERT_EQ(rec->write_ns, (std::vector<std::int64_t>{
        base.count, base.count + 100'000'000, base.count + 300'000'000}));
    const auto guidance_pcm = flatten_i16({rec->writes[1]});
    ASSERT_FALSE(guidance_pcm.empty());
    EXPECT_EQ(guidance_pcm, std::vector<std::int16_t>(guidance_pcm.size(), 2000));
    EXPECT_EQ(router->report().stats.frames_silence, 0U);
    EXPECT_EQ(router->report().stats.frames_skipped, 14400U - 240U - guidance_pcm.size() / 2);
}

TEST_F(AudioPipelineRoute, IncrementalBackpressureStartingMidFrameRendersThatFrame) {
    open_incremental();
    enqueue(media, 1, {1000, 240});
    expect_headers(step().completed, {{media, 1}});
    enqueue(media, 2, {2000, 240}, 300'000'000);
    router->note_backpressure(aa::core::Nanoseconds{base.count + 5'000'010},
                              aa::core::Nanoseconds{base.count + 300'000'000});

    for (int i = 0; i < 4 && step().pending; ++i) {}

    EXPECT_EQ(router->report().stats.frames_silence, 1U);
    EXPECT_EQ(router->report().stats.frames_skipped, 14159U);
    EXPECT_EQ(rec->write_ns.back(), base.count + 300'000'000);
}

TEST_F(AudioPipelineRoute, IncrementalExplicitIdleResumesAtOriginalTimestamp) {
    open_incremental();
    enqueue(media, 1, {1000, 240});
    expect_headers(step().completed, {{media, 1}});
    // Settlement is B+5 ms; receipt of the next parent is B+2405 ms.
    enqueue(media, 2, {2000, 240}, 2'405'000'000);
    router->note_all_role_idle(aa::core::Nanoseconds{base.count + 5'000'000},
                               aa::core::Nanoseconds{base.count + 2'405'000'000});

    const auto skipped = router->render_step();

    ASSERT_TRUE(skipped);
    EXPECT_TRUE(skipped.value().contributors.empty());
    EXPECT_TRUE(skipped.value().completed.empty());
    EXPECT_TRUE(skipped.value().pending);
    EXPECT_EQ(rec->writes.size(), 1U);
    EXPECT_EQ(completions.size(), 1U);
    const auto resumed = step();
    expect_headers(resumed.contributors, {{media, 2}});
    expect_headers(resumed.completed, {{media, 2}});
    EXPECT_FALSE(resumed.pending);
    EXPECT_TRUE(step().completed.empty());
    EXPECT_EQ(rec->write_ns, (std::vector<std::int64_t>{base.count, base.count + 2'405'000'000}));
    EXPECT_EQ(flatten_i16({rec->writes.back()}), std::vector<std::int16_t>(480, 2000));
    ASSERT_EQ(completions.size(), 2U);
    EXPECT_EQ(completions.back().scheduled.count, base.count + 2'405'000'000);
    const auto& stats = router->report().stats;
    EXPECT_EQ(stats.frames_idle_skipped, 115200U);
    EXPECT_EQ(stats.frames_skipped, 0U);
    EXPECT_EQ(stats.frames_silence, 0U);
    EXPECT_EQ(stats.frames_output, 480U);
    EXPECT_EQ(stats.frames_retained, 480U);
    EXPECT_EQ(stats.frames_trimmed, 0U);
}

TEST_F(AudioPipelineRoute, IncrementalAbandonedIdleAuthorizationAllowsSecondResumeAfterRetirement) {
    open_incremental();
    enqueue(media, 1, {1000, 240});
    expect_headers(step().completed, {{media, 1}});
    router->note_all_role_idle(aa::core::Nanoseconds{base.count + 5'000'000},
                               aa::core::Nanoseconds{base.count + 2'405'000'000});
    enqueue(media, 2, {2000, 240}, 2'405'000'000);
    const auto first_skip = step();
    EXPECT_TRUE(first_skip.pending);
    EXPECT_TRUE(first_skip.contributors.empty());
    EXPECT_TRUE(first_skip.completed.empty());
    ASSERT_EQ(rec->writes.size(), 1U);
    router->cancel_pending(media);
    router->retire_all_role_idle();

    router->note_all_role_idle(aa::core::Nanoseconds{base.count + 2'405'000'000},
                               aa::core::Nanoseconds{base.count + 4'805'000'000});
    enqueue(media, 3, {3000, 240}, 4'805'000'000);
    const auto second_skip = router->render_step();
    ASSERT_TRUE(second_skip);
    EXPECT_TRUE(second_skip.value().pending);
    EXPECT_TRUE(second_skip.value().contributors.empty());
    EXPECT_TRUE(second_skip.value().completed.empty());
    EXPECT_EQ(rec->writes.size(), 1U);
    EXPECT_EQ(completions.size(), 1U);
    const auto resumed = step();
    expect_headers(resumed.contributors, {{media, 3}});
    expect_headers(resumed.completed, {{media, 3}});
    EXPECT_FALSE(resumed.pending);
    EXPECT_EQ(rec->write_ns, (std::vector<std::int64_t>{base.count, base.count + 4'805'000'000}));
    EXPECT_EQ(flatten_i16({rec->writes.back()}), std::vector<std::int16_t>(480, 3000));
    ASSERT_EQ(completions.size(), 2U);
    EXPECT_EQ(completions.back().scheduled.count, base.count + 4'805'000'000);
    const auto& stats = router->report().stats;
    EXPECT_EQ(stats.frames_idle_skipped, 230400U);
    EXPECT_EQ(stats.frames_skipped, 0U);
    EXPECT_EQ(stats.frames_silence, 0U);
    EXPECT_EQ(stats.frames_output, 480U);
    EXPECT_EQ(stats.frames_retained, 480U);
    EXPECT_EQ(stats.frames_trimmed, 0U);
}

TEST_F(AudioPipelineRoute, IncrementalIdleRetirementPreservesCountersTimelinePendingAndBackpressure) {
    open_incremental();
    enqueue(media, 1, {1000, 240});
    step();
    router->note_all_role_idle(aa::core::Nanoseconds{base.count + 5'000'000},
                               aa::core::Nanoseconds{base.count + 2'405'000'000});
    enqueue(media, 2, {2000, 240}, 2'405'000'000);
    EXPECT_TRUE(step().contributors.empty());
    router->note_backpressure(aa::core::Nanoseconds{base.count + 2'410'000'000},
                              aa::core::Nanoseconds{base.count + 2'710'000'000});
    const auto before = router->report().stats;

    router->retire_all_role_idle();
    router->retire_all_role_idle();

    const auto& after = router->report().stats;
    EXPECT_EQ(after.chunks_submitted, before.chunks_submitted);
    EXPECT_EQ(after.frames_mixed, before.frames_mixed);
    EXPECT_EQ(after.frames_written, before.frames_written);
    EXPECT_EQ(after.samples_written, before.samples_written);
    EXPECT_EQ(after.last_path_latency.count, before.last_path_latency.count);
    EXPECT_EQ(after.max_path_latency.count, before.max_path_latency.count);
    EXPECT_EQ(after.last_completion_skew.count, before.last_completion_skew.count);
    EXPECT_EQ(after.max_completion_skew.count, before.max_completion_skew.count);
    EXPECT_EQ(after.frames_decoded, before.frames_decoded);
    EXPECT_EQ(after.frames_retained, before.frames_retained);
    EXPECT_EQ(after.frames_trimmed, before.frames_trimmed);
    EXPECT_EQ(after.frames_silence, before.frames_silence);
    EXPECT_EQ(after.frames_output, before.frames_output);
    EXPECT_EQ(after.frames_skipped, before.frames_skipped);
    EXPECT_EQ(after.frames_idle_skipped, before.frames_idle_skipped);
    EXPECT_EQ(rec->writes.size(), 1U);
    EXPECT_EQ(completions.size(), 1U);
    expect_headers(step().completed, {{media, 2}});
    enqueue(media, 3, {3000, 240}, 2'710'000'000);
    const auto skipped = step();
    EXPECT_TRUE(skipped.pending);
    EXPECT_TRUE(skipped.contributors.empty());
    EXPECT_TRUE(skipped.completed.empty());
    expect_headers(step().completed, {{media, 3}});
    EXPECT_EQ(rec->write_ns, (std::vector<std::int64_t>{
        base.count, base.count + 2'405'000'000, base.count + 2'710'000'000}));
    EXPECT_EQ(after.frames_idle_skipped, 115200U);
    EXPECT_EQ(after.frames_skipped, 14400U);
    EXPECT_EQ(after.frames_silence, 0U);
    EXPECT_EQ(after.frames_retained, 720U);
}

TEST_F(AudioPipelineRoute, IncrementalIdleRetirementCannotHideStaleFutureOrHiddenGapParents) {
    for (int scenario = 0; scenario < 5; ++scenario) {
        SCOPED_TRACE(scenario);
        open_incremental();
        enqueue(media, 1, {1000, 240});
        step();
        router->note_all_role_idle(aa::core::Nanoseconds{base.count + 5'000'000},
                                   aa::core::Nanoseconds{base.count + 2'405'000'000});
        router->retire_all_role_idle();
        router->note_all_role_idle(aa::core::Nanoseconds{base.count + 5'000'000},
                                   aa::core::Nanoseconds{base.count + 4'805'000'000});
        if (scenario == 0) {
            ASSERT_TRUE(router->submit(make_message(media, 2, aa::core::Nanoseconds{0},
                                                    constant_pcm({2000, 240}))));
        } else {
            const auto future = now_ns().count + 3'600'000'000'000LL - base.count;
            enqueue(media, 2, {2000, 240}, scenario == 1 ? future : 4'805'000'000);
            if (scenario == 2 || scenario == 3) {
                enqueue(scenario == 2 ? media : guidance, 3, {3000, 240}, 5'110'000'000);
            } else if (scenario == 4) {
                enqueue(guidance, 3, {3000, 240}, 100'000'000);
            }
        }
        router->retire_all_role_idle();
        router->note_all_role_idle(aa::core::Nanoseconds{base.count + 5'000'000},
                                   aa::core::Nanoseconds{base.count + 4'805'000'000});

        const auto rejected = router->render_step();

        ASSERT_FALSE(rejected);
        EXPECT_EQ(rejected.error().code(), aa::ErrorCode::invalid_argument);
        EXPECT_EQ(rec->writes.size(), 1U);
        EXPECT_EQ(router->report().stats.frames_idle_skipped, 0U);
        EXPECT_EQ(router->report().stats.frames_skipped, 0U);
        EXPECT_EQ(router->report().stats.frames_silence, 0U);
        EXPECT_EQ(router->report().stats.frames_output, 240U);
        enqueue(media, 4, {4000, 1}, 5'000'000);
        expect_headers(step().completed, {{media, 4}});
        EXPECT_EQ(rec->write_ns.back(), base.count + 5'000'000);
    }
}

TEST_F(AudioPipelineRoute, IncrementalLongIdleWithoutObservationKeepsBaselineRejection) {
    open_incremental();
    enqueue(media, 1, {1000, 240});
    expect_headers(step().completed, {{media, 1}});
    enqueue(media, 2, {2000, 240}, 2'405'000'000);

    const auto rejected = router->render_step();

    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().code(), aa::ErrorCode::invalid_argument);
    EXPECT_EQ(rec->writes.size(), 1U);
    EXPECT_EQ(router->report().stats.frames_idle_skipped, 0U);
}

TEST_F(AudioPipelineRoute, IncrementalShortIdleDoesNotAuthorizeAnyFrames) {
    for (const auto duration : {0LL, 100'000'000LL, 200'000'000LL}) {
        SCOPED_TRACE(duration);
        open_incremental();
        enqueue(media, 1, {1000, 240});
        step();
        router->note_all_role_idle(aa::core::Nanoseconds{base.count + 5'000'000},
            aa::core::Nanoseconds{base.count + 5'000'000 + duration});
        enqueue(media, 2, {2000, 240}, 205'000'000);
        for (int i = 0; i < 41 && step().pending; ++i) {}
        EXPECT_EQ(router->report().stats.frames_idle_skipped, 0U);
        EXPECT_EQ(router->report().stats.frames_silence, 9600U);
        EXPECT_EQ(rec->write_ns.back(), base.count + 205'000'000);
    }
}

TEST_F(AudioPipelineRoute, IncrementalIdleRejectsStaleFutureHiddenAndOverflowAtomically) {
    for (int scenario = 0; scenario < 6; ++scenario) {
        SCOPED_TRACE(scenario);
        open_incremental();
        enqueue(media, 1, {1000, 240});
        step();
        router->note_all_role_idle(aa::core::Nanoseconds{base.count + 5'000'000},
                                   aa::core::Nanoseconds{base.count + 2'405'000'000});
        if (scenario == 0) {
            ASSERT_TRUE(router->submit(make_message(media, 2, aa::core::Nanoseconds{0},
                                                    constant_pcm({2000, 240}))));
        } else {
            enqueue(media, 2, {2000, 240}, scenario == 1 ? 2'710'000'000 : 2'405'000'000);
            if (scenario == 2 || scenario == 3) {
                enqueue(scenario == 2 ? media : guidance, 3, {3000, 240}, 2'710'000'000);
            } else if (scenario == 4) {
                enqueue(guidance, 3, {3000, 240}, 100'000'000);
            } else if (scenario == 5) {
                ASSERT_TRUE(router->submit(make_message(guidance, 3,
                    aa::core::Nanoseconds{INT64_MAX}, constant_pcm({3000, 240}))));
            }
        }

        const auto rejected = router->render_step();

        ASSERT_FALSE(rejected);
        EXPECT_EQ(rejected.error().code(), aa::ErrorCode::invalid_argument);
        EXPECT_EQ(rec->writes.size(), 1U);
        EXPECT_EQ(router->report().stats.frames_idle_skipped, 0U);
        EXPECT_EQ(router->report().stats.frames_skipped, 0U);
        EXPECT_EQ(router->report().stats.frames_silence, 0U);
        EXPECT_EQ(router->report().stats.frames_output, 240U);
        EXPECT_FALSE(step().pending);
        enqueue(media, 4, {4000, 240}, 2'405'000'000);
        EXPECT_FALSE(router->render_step());
        enqueue(media, 5, {4000, 1}, 5'000'000);
        expect_headers(step().completed, {{media, 5}});
        EXPECT_EQ(rec->write_ns.back(), base.count + 5'000'000);
    }
}

TEST_F(AudioPipelineRoute, IncrementalIdleDoesNotAuthorizeFutureTime) {
    open_incremental();
    const auto future = now_ns().count + 3'600'000'000'000LL - base.count;
    enqueue(media, 1, {1000, 240}, future);
    step();
    router->note_all_role_idle(aa::core::Nanoseconds{base.count + future + 5'000'000},
                               aa::core::Nanoseconds{base.count + future + 2'405'000'000});
    enqueue(media, 2, {2000, 240}, future + 2'405'000'000);

    const auto rejected = router->render_step();

    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().code(), aa::ErrorCode::invalid_argument);
    EXPECT_EQ(rec->writes.size(), 1U);
    EXPECT_EQ(router->report().stats.frames_idle_skipped, 0U);
}

TEST_F(AudioPipelineRoute, IncrementalIdleOverlappingBackpressureCountsUnionOnce) {
    open_incremental();
    enqueue(media, 1, {1000, 240});
    step();
    router->note_all_role_idle(aa::core::Nanoseconds{base.count + 5'000'000},
                               aa::core::Nanoseconds{base.count + 2'405'000'000});
    router->note_backpressure(aa::core::Nanoseconds{base.count + 105'000'000},
                              aa::core::Nanoseconds{base.count + 1'105'000'000});
    router->note_backpressure(aa::core::Nanoseconds{base.count + 605'000'000},
                              aa::core::Nanoseconds{base.count + 1'605'000'000});
    enqueue(media, 2, {2000, 240}, 2'405'000'000);
    for (int i = 0; i < 4; ++i) {
        const auto result = step();
        if (i < 3) {
            EXPECT_TRUE(result.completed.empty());
            EXPECT_TRUE(result.contributors.empty());
            EXPECT_EQ(rec->writes.size(), 1U);
        } else {
            expect_headers(result.completed, {{media, 2}});
            EXPECT_FALSE(result.pending);
        }
    }
    EXPECT_EQ(router->report().stats.frames_idle_skipped, 43200U);
    EXPECT_EQ(router->report().stats.frames_skipped, 72000U);
    EXPECT_EQ(router->report().stats.frames_silence, 0U);
    EXPECT_EQ(rec->write_ns.back(), base.count + 2'405'000'000);
}

TEST_F(AudioPipelineRoute, IncrementalIdleStartingAndEndingMidFrameIsConservative) {
    open_incremental();
    enqueue(media, 1, {1000, 240});
    step();
    router->note_all_role_idle(aa::core::Nanoseconds{base.count + 5'000'010},
                               aa::core::Nanoseconds{base.count + 2'404'999'990});
    enqueue(media, 2, {2000, 240}, 2'405'000'000);
    for (int i = 0; i < 4 && step().pending; ++i) {}
    EXPECT_EQ(router->report().stats.frames_idle_skipped, 115198U);
    EXPECT_EQ(router->report().stats.frames_silence, 2U);
    EXPECT_EQ(rec->write_ns.back(), base.count + 2'405'000'000);
}

TEST_F(AudioPipelineRoute, IncrementalIdleRetiresUnusedTailOnFirstPcmConsumption) {
    open_incremental();
    enqueue(media, 1, {1000, 240});
    step();
    router->note_all_role_idle(aa::core::Nanoseconds{base.count + 5'000'000},
                               aa::core::Nanoseconds{base.count + 5'000'000'000});
    enqueue(media, 2, {2000, 480}, 2'405'000'000);
    EXPECT_TRUE(step().contributors.empty());
    const auto first_pcm = step();
    expect_headers(first_pcm.contributors, {{media, 2}});
    EXPECT_TRUE(first_pcm.completed.empty());
    enqueue(guidance, 3, {3000, 240}, 3'000'000'000);

    const auto rejected = router->render_step();

    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().code(), aa::ErrorCode::invalid_argument);
    EXPECT_EQ(rec->writes.size(), 2U);
    EXPECT_EQ(router->report().stats.frames_idle_skipped, 115200U);
}

TEST_F(AudioPipelineRoute, IncrementalIdleCannotExtendLatchedPrefixWithLaterSubmission) {
    open_incremental();
    enqueue(media, 1, {1000, 240});
    step();
    router->note_all_role_idle(aa::core::Nanoseconds{base.count + 5'000'000},
                               aa::core::Nanoseconds{base.count + 5'000'000'000});
    enqueue(media, 2, {2000, 240}, 2'405'000'000);
    EXPECT_TRUE(step().contributors.empty());
    enqueue(guidance, 3, {3000, 240}, 3'000'000'000);

    const auto rejected = router->render_step();

    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().code(), aa::ErrorCode::invalid_argument);
    EXPECT_EQ(rec->writes.size(), 1U);
    EXPECT_EQ(router->report().stats.frames_idle_skipped, 115200U);
}

TEST_F(AudioPipelineRoute, IncrementalIdleRepeatedObservationCannotExtendAuthorization) {
    open_incremental();
    enqueue(media, 1, {1000, 240});
    step();
    router->note_all_role_idle(aa::core::Nanoseconds{base.count + 5'000'000},
                               aa::core::Nanoseconds{base.count + 305'000'000});
    router->note_all_role_idle(aa::core::Nanoseconds{base.count + 5'000'000},
                               aa::core::Nanoseconds{base.count + 2'405'000'000});
    enqueue(media, 2, {2000, 240}, 2'405'000'000);

    EXPECT_FALSE(router->render_step());
    EXPECT_EQ(rec->writes.size(), 1U);
    EXPECT_EQ(router->report().stats.frames_idle_skipped, 0U);
}

TEST_F(AudioPipelineRoute, IncrementalIdleAt44100PreservesOriginalSampleTimeline) {
    auto config = pcm_router_config();
    config.output_format.sample_rate_hz = 44100;
    for (auto& role : config.roles) { role.input_format = config.output_format; }
    auto sink = std::make_unique<RecordingAudioSink>();
    const auto observed = sink->state;
    auto opened = aa::audio::detail::open_router_with_sink_for_test(config, std::move(sink));
    ASSERT_TRUE(opened);
    auto& route = opened.value();
    ASSERT_TRUE(route.submit(make_message(media, 1, base, constant_pcm({1000, 441}))));
    ASSERT_TRUE(route.render_step());
    route.note_all_role_idle(aa::core::Nanoseconds{base.count + 10'000'010},
                             aa::core::Nanoseconds{base.count + 2'410'000'000});
    ASSERT_TRUE(route.submit(make_message(media, 2,
        aa::core::Nanoseconds{base.count + 2'410'000'000}, constant_pcm({2000, 441}))));
    for (int i = 0; i < 4; ++i) {
        const auto rendered = route.render_step();
        ASSERT_TRUE(rendered);
        if (!rendered.value().pending) {
            expect_headers(rendered.value().completed, {{media, 2}});
            break;
        }
        ASSERT_LT(i, 3);
    }
    EXPECT_EQ(route.report().stats.frames_idle_skipped, 105839U);
    EXPECT_EQ(route.report().stats.frames_silence, 1U);
    EXPECT_EQ(route.report().stats.frames_retained, 882U);
    EXPECT_EQ(observed->write_ns.back(), base.count + 2'410'000'000);
}

TEST_F(AudioPipelineRoute, IncrementalDefaultQuantumWritesOneObservedMediaParentPerStep) {
    auto sink = std::make_unique<RecordingAudioSink>();
    const auto observed = sink->state;
    auto opened = aa::audio::detail::open_router_with_sink_for_test(pcm_router_config(), std::move(sink));
    ASSERT_TRUE(opened);
    // One observed media parent: 8192 B at 48 kHz stereo = 2048 frames (42.67 ms).
    ASSERT_TRUE(opened.value().submit(make_message(media, 1, base, constant_pcm({1000, 2048}))));
    for (int i = 0; i < 8 && opened.value().render_step().value().pending; ++i) {}
    std::vector<std::size_t> frames;
    for (const auto& write : observed->writes) { frames.push_back(write.size() / 4); }
    EXPECT_EQ(frames, (std::vector<std::size_t>{2048}));
}

// #173 P3: cancel_pending retires the cancelled role's unplayed source frontier.
// The frontier matters once the cursor has moved past it: the catch-up's late
// check compares a run's parent with min(cursor, frontier), so a frontier left
// 200 ms ahead fails a run that is really within the cap.
TEST_F(AudioPipelineRoute, CancellationRetiresTheUnplayedSourceFrontier) {
    auto config = pcm_router_config();
    config.live_pcm_drift.enabled = true;
    config.live_pcm_drift.catch_up_watermark = aa::core::Nanoseconds{90'000'000};
    auto opened = aa::audio::detail::open_router_with_sink_for_test(
        config, std::make_unique<RecordingAudioSink>());
    ASSERT_TRUE(opened);
    auto& route = opened.value();
    const auto drained = [&] {
        for (int i = 0; i < 16; ++i) {
            auto stepped = route.render_step();
            if (!stepped) { return false; }
            if (!stepped.value().pending) { return true; }
        }
        return false;
    };
    const auto at = [&](std::int64_t ms) { return aa::core::Nanoseconds{base.count + ms * 1'000'000}; };
    // 50 ms of a 200 ms guidance parent play (cursor 50 ms, frontier 200 ms),
    // then the prompt is cancelled.
    ASSERT_TRUE(route.submit(make_message(guidance, 1, base, constant_pcm({1000, 9600}))));
    ASSERT_TRUE(route.render_step());
    route.cancel_pending(guidance);
    // 100 ms of media stamped 30 ms before the base plays at 50..145 ms: the
    // cursor is now 75 ms past the real frontier (70 ms).
    ASSERT_TRUE(route.submit(make_message(media, 2, at(-30), constant_pcm({2000, 4800}))));
    ASSERT_TRUE(drained());
    // A 10 ms chunk ending at -90 ms is 160 ms behind the real frontier (caught
    // up), but 235 ms behind the cursor and the retired one.
    ASSERT_TRUE(route.submit(make_message(media, 3, at(-100), constant_pcm({2000, 480}))));
    EXPECT_TRUE(drained());
    EXPECT_FALSE(route.report().render_failure);
    EXPECT_EQ(route.report().stats.frames_caught_up, 480U - 240U);
}

// #173 P3: reset_session_timeline clears the global and the per-role source
// frontier. Two runs whose treatment depends on each.
TEST_F(AudioPipelineRoute, SessionResetClearsTheSourceFrontiers) {
    auto config = pcm_router_config();
    config.live_pcm_drift.enabled = true;
    config.live_pcm_drift.catch_up_watermark = aa::core::Nanoseconds{90'000'000};
    for (const bool per_role : {true, false}) {
        SCOPED_TRACE(per_role);
        std::size_t extra_frames = 0;
        auto opened = aa::audio::detail::open_router_with_decoder_for_test(
            config, std::make_unique<RecordingAudioSink>(),
            [&](const aa::audio::EncodedChunk& chunk) -> aa::core::Result<std::vector<std::byte>> {
                std::vector<std::byte> out(chunk.payload.begin(), chunk.payload.end());
                out.resize(out.size() + extra_frames * 4, std::byte{1});
                return out;
            });
        ASSERT_TRUE(opened);
        auto& route = opened.value();
        const auto drained = [&] {
            for (int i = 0; i < 16; ++i) {
                auto stepped = route.render_step();
                if (!stepped) { return false; }
                if (!stepped.value().pending) { return true; }
            }
            return false;
        };
        const auto at = [&](std::int64_t ms) { return aa::core::Nanoseconds{base.count + ms * 1'000'000}; };
        ASSERT_TRUE(route.submit(make_message(guidance, 1, base, constant_pcm({1000, 9600}))));
        ASSERT_TRUE(route.render_step());
        route.reset_session_timeline();
        if (per_role) {
            // An on-time first run carrying 100 ms of earlier output overlaps
            // nothing in the new session, so its carryover is not a backlog.
            extra_frames = 4800;
            ASSERT_TRUE(route.submit(make_message(guidance, 2, at(0), constant_pcm({2000, 480}))));
            EXPECT_TRUE(drained());
            EXPECT_EQ(route.report().stats.frames_trimmed, 0U);
        } else {
            // As above: the cursor passes the real frontier (110 ms) to 175 ms,
            // and a run 190 ms behind that frontier is caught up, not late.
            ASSERT_TRUE(route.submit(make_message(media, 2, at(0), constant_pcm({2000, 4800}))));
            ASSERT_TRUE(drained());
            ASSERT_TRUE(route.submit(make_message(media, 3, at(30), constant_pcm({2000, 3840}))));
            ASSERT_TRUE(drained());
            ASSERT_TRUE(route.submit(make_message(media, 4, at(-90), constant_pcm({2000, 480}))));
            EXPECT_TRUE(drained());
            EXPECT_EQ(route.report().stats.frames_caught_up, 480U - 240U);
        }
        EXPECT_FALSE(route.report().render_failure);
    }
}

// ---------------------------------------------------------------------------
// #228: wired audio never drops sound. Every test below opens the router with
// the receivers' production defaults, aa::audio::pcm_router_config(), and
// reads only counters that existed before the lossless policy where it can, so
// it can be run against earlier revisions too.
namespace {

constexpr std::uint64_t kA26Frames = 2048;
constexpr std::int64_t kA26ChunkNs = 2048LL * 1'000'000'000LL / 48000LL; // 42.67 ms
constexpr std::int64_t kMs = 1'000'000;

// Continuous 440 Hz tone: a dropped or repeated sample shows as a jump.
constexpr double kToneAmplitude = 8000.0;
constexpr double kToneHz = 440.0;

[[nodiscard]] std::vector<std::byte> tone_pcm(std::uint64_t first_frame, std::uint64_t frames) {
    std::vector<std::byte> bytes(static_cast<std::size_t>(frames) * 4);
    for (std::uint64_t i = 0; i < frames; ++i) {
        const double phase = 2.0 * 3.14159265358979323846 * kToneHz
            * static_cast<double>(first_frame + i) / 48000.0;
        const auto value = static_cast<std::int16_t>(std::lround(kToneAmplitude * std::sin(phase)));
        std::memcpy(bytes.data() + i * 4, &value, sizeof(value));
        std::memcpy(bytes.data() + i * 4 + 2, &value, sizeof(value));
    }
    return bytes;
}

// Resampler as observed (#173): the first 200 ms guidance parent returns 96
// frames, the second the other 19,098, later ones whatever is buffered.
[[nodiscard]] aa::audio::detail::TestDecodeFn splitting_decoder() {
    struct State {
        std::deque<std::int16_t> buffered;
        std::size_t calls{};
    };
    auto state = std::make_shared<State>();
    return [state](const aa::audio::EncodedChunk& chunk) -> aa::core::Result<std::vector<std::byte>> {
        if (chunk.input_format.sample_rate_hz == 48000) {
            return std::vector<std::byte>(chunk.payload.begin(), chunk.payload.end());
        }
        for (std::size_t i = 0; i + 1 < chunk.payload.size(); i += 2) {
            std::int16_t sample = 0;
            std::memcpy(&sample, chunk.payload.data() + i, sizeof(sample));
            state->buffered.insert(state->buffered.end(), 3, sample);
        }
        const auto take = state->calls == 0 ? std::size_t{96}
            : state->calls == 1 ? std::min<std::size_t>(19098, state->buffered.size())
                                : state->buffered.size();
        ++state->calls;
        std::vector<std::byte> out(take * 4);
        for (std::size_t frame = 0; frame < take; ++frame) {
            const auto value = state->buffered.front();
            state->buffered.pop_front();
            std::memcpy(out.data() + frame * 4, &value, sizeof(value));
            std::memcpy(out.data() + frame * 4 + 2, &value, sizeof(value));
        }
        return out;
    };
}

// A production wired route on a recording sink. Arrival stamps start an hour
// ahead of the steady clock, so the newest arrival is the route's only notion
// of now: the interleaving does not depend on how fast the test runs.
struct WiredRoute final {
    std::shared_ptr<RecordingSinkState> sink;
    std::vector<aa::audio::WriteCompletion> writes;
    std::optional<aa::audio::AudioRouter> router;
    std::int64_t start;
    std::uint64_t media_chunks{};
    std::uint64_t guidance_chunks{};

    explicit WiredRoute(aa::audio::detail::TestDecodeFn decode = {},
                        std::function<void(aa::audio::RouterConfig&)> adjust = {})
        : start(now_ns().count + 3'600'000'000'000LL) {
        auto config = aa::audio::pcm_router_config();
        config.on_write_completed = [this](const auto& value) { writes.push_back(value); };
        if (adjust) { adjust(config); }
        auto recording = std::make_unique<RecordingAudioSink>();
        sink = recording->state;
        auto opened = decode
            ? aa::audio::detail::open_router_with_decoder_for_test(config, std::move(recording),
                                                                   std::move(decode))
            : aa::audio::detail::open_router_with_sink_for_test(config, std::move(recording));
        if (opened) { router.emplace(std::move(opened).value()); }
    }
    WiredRoute(const WiredRoute&) = delete;
    WiredRoute& operator=(const WiredRoute&) = delete;
    WiredRoute(WiredRoute&&) = delete;
    WiredRoute& operator=(WiredRoute&&) = delete;
    ~WiredRoute() = default;

    [[nodiscard]] bool drain() {
        for (int i = 0; i < 1024; ++i) {
            auto stepped = router->render_step();
            if (!stepped) { return false; }
            if (!stepped.value().pending) { return true; }
        }
        return false;
    }
    [[nodiscard]] bool submit_media(std::int64_t offset_ns) {
        const auto pcm = tone_pcm(media_chunks * kA26Frames, kA26Frames);
        return router->submit(make_message(aa::protocol::ChannelRole::media_audio, ++media_chunks,
                                           {start + offset_ns}, pcm)).has_value();
    }
    // 200 ms of 16 kHz mono, the largest admitted guidance parent.
    [[nodiscard]] bool submit_guidance(std::int64_t offset_ns, std::int16_t level = 1200) {
        std::vector<std::byte> pcm(6400);
        for (std::size_t i = 0; i < pcm.size(); i += 2) { std::memcpy(pcm.data() + i, &level, 2); }
        return router->submit(make_message(aa::protocol::ChannelRole::guidance_audio, ++guidance_chunks,
                                           {start + offset_ns}, pcm)).has_value();
    }
    [[nodiscard]] bool media(std::int64_t offset_ns) { return submit_media(offset_ns) && drain(); }
    [[nodiscard]] const aa::audio::RouterStats& stats() const { return router->report().stats; }
    [[nodiscard]] std::vector<std::int16_t> left() const {
        std::vector<std::int16_t> out;
        const auto all = flatten_i16(sink->writes);
        for (std::size_t i = 0; i < all.size(); i += 2) { out.push_back(all[i]); }
        return out;
    }
    // Largest offset any write reported (what the video hold follows).
    [[nodiscard]] std::int64_t largest_offset() const {
        std::int64_t largest = 0;
        for (const auto& write : writes) {
            largest = std::max({largest, write.playout_offset.count, write.playout_end_offset.count});
        }
        return largest;
    }
};

void expect_nothing_lost(const WiredRoute& route) {
    const auto& stats = route.stats();
    EXPECT_EQ(stats.frames_trimmed, 0U);
    EXPECT_EQ(stats.frames_shed, 0U);
    EXPECT_EQ(stats.frames_caught_up, 0U);
    EXPECT_EQ(stats.frames_padded, 0U);
    EXPECT_FALSE(route.router->report().render_failure);
}

// Every cycle of the media tone was played, whatever rate it played at: one
// silence gap may cut or add a crossing at either end. A 1,228-frame shed
// (Sol's probe) removes 22.
void expect_whole_tone(const WiredRoute& route, std::uint64_t gaps) {
    std::int64_t crossings = 0;
    std::int16_t previous = 0;
    for (const auto sample : route.left()) {
        if (sample == 0) { continue; }
        if (previous != 0 && (sample > 0) != (previous > 0)) { ++crossings; }
        previous = sample;
    }
    const auto expected = static_cast<double>(route.media_chunks * kA26Frames) / 48000.0 * kToneHz * 2.0;
    EXPECT_NEAR(static_cast<double>(crossings), expected, 2.0 + 2.0 * static_cast<double>(gaps));
}

// Largest step between neighbouring samples away from silence: the tone's own
// slope (461), so a splice or a repeat stands out.
[[nodiscard]] int largest_step(const std::vector<std::int16_t>& samples) {
    int largest = 0;
    for (std::size_t i = 1; i < samples.size(); ++i) {
        if (samples[i] == 0 || samples[i - 1] == 0) { continue; }
        largest = std::max(largest, std::abs(samples[i] - samples[i - 1]));
    }
    return largest;
}

} // namespace

// Review of #229, P1: 6 A26 chunks, the 7th withheld until 6T + 400 ms, then 8
// chunks 1 ms apart. 25d4010 shed 1,228 frames here.
TEST_F(AudioPipelineRoute, WiredDefaultsDropNothingInABurstAfterAnUnderrun) {
    WiredRoute route;
    ASSERT_TRUE(route.router);
    for (int k = 0; k < 6; ++k) { ASSERT_TRUE(route.media(k * kA26ChunkNs)); }
    const auto resumed = 6 * kA26ChunkNs + 400 * kMs;
    for (int k = 0; k < 8; ++k) { ASSERT_TRUE(route.media(resumed + k * kMs)); }
    EXPECT_EQ(route.stats().playout_underruns, 1U);
    expect_nothing_lost(route);
    expect_whole_tone(route, 1);
    // The backlog became latency (60 ms + 7 chunks) that the writes report, so
    // the video hold follows it, under its 400 ms cap ...
    EXPECT_GT(route.stats().playout_depth, aa::core::Nanoseconds{340 * kMs});
    EXPECT_LT(route.stats().playout_depth, aa::core::Nanoseconds{400 * kMs});
    // ... and drains without loss while the phone plays on.
    for (int k = 1; k <= 60'000 / 42; ++k) {
        ASSERT_TRUE(route.media(resumed + 7 * kMs + k * kA26ChunkNs));
    }
    expect_nothing_lost(route);
    expect_whole_tone(route, 1);
    EXPECT_EQ(route.stats().playout_underruns, 1U);
    EXPECT_LT(route.stats().playout_depth, aa::core::Nanoseconds{75 * kMs});
    EXPECT_GE(route.stats().playout_depth, aa::core::Nanoseconds{60 * kMs});
    EXPECT_LT(route.largest_offset(), 400 * kMs);
    EXPECT_LE(largest_step(route.left()), 470);
}

// Review of #229, P1: four burst chunks, then on-cadence arrivals. 25d4010
// trimmed 5,280 frames through the bounded drift correction.
TEST_F(AudioPipelineRoute, WiredDefaultsDropNothingFromASustainedBacklog) {
    WiredRoute route;
    ASSERT_TRUE(route.router);
    for (int k = 0; k < 6; ++k) { ASSERT_TRUE(route.media(k * kA26ChunkNs)); }
    const auto resumed = 6 * kA26ChunkNs + 400 * kMs;
    for (int k = 0; k < 4; ++k) { ASSERT_TRUE(route.media(resumed + k * kMs)); }
    for (int k = 1; k <= 29; ++k) { ASSERT_TRUE(route.media(resumed + 3 * kMs + k * kA26ChunkNs)); }
    EXPECT_EQ(route.stats().playout_underruns, 1U);
    expect_nothing_lost(route);
    expect_whole_tone(route, 1);
    EXPECT_LE(largest_step(route.left()), 470);
}

// A phone clock 500 ppm fast used to be trimmed as soon as the depth floor was
// 20 ms high; now the route plays 500 ppm fast.
TEST_F(AudioPipelineRoute, WiredDefaultsFollowAFastPhoneClockWithoutTrimming) {
    WiredRoute route;
    ASSERT_TRUE(route.router);
    const auto cadence = kA26ChunkNs - kA26ChunkNs / 2000;
    for (int k = 0; k < 3000; ++k) { ASSERT_TRUE(route.media(k * cadence)); }
    expect_nothing_lost(route);
    expect_whole_tone(route, 0);
    EXPECT_EQ(route.stats().playout_underruns, 0U);
    EXPECT_EQ(route.stats().frames_silence, 0U);
    EXPECT_LT(*route.stats().playout_depth_max, aa::core::Nanoseconds{80 * kMs});
    EXPECT_LE(largest_step(route.left()), 470);
}

// Review of #229, P2: receipt 50 ms later for good. Every chunk is still in
// time, so there is nothing to pad; 25d4010 inserted 240 zero frames.
TEST_F(AudioPipelineRoute, WiredDefaultsInsertNoSilenceBeforeAnUnderrun) {
    WiredRoute route;
    ASSERT_TRUE(route.router);
    for (int k = 0; k < 6; ++k) { ASSERT_TRUE(route.media(k * kA26ChunkNs)); }
    for (int k = 6; k < 400; ++k) { ASSERT_TRUE(route.media(k * kA26ChunkNs + 50 * kMs)); }
    expect_nothing_lost(route);
    expect_whole_tone(route, 0);
    EXPECT_EQ(route.stats().playout_underruns, 0U);
    EXPECT_EQ(route.stats().frames_silence, 0U);
    EXPECT_EQ(route.stats().frames_skipped, 0U);
    // The thin buffer refilled by playing slow, to half the depth, no further.
    EXPECT_GT(route.stats().playout_depth, aa::core::Nanoseconds{25 * kMs});
    EXPECT_LT(*route.stats().playout_depth_max, aa::core::Nanoseconds{61 * kMs});
    EXPECT_LT(route.stats().playout_depth, aa::core::Nanoseconds{32 * kMs});
    EXPECT_LE(largest_step(route.left()), 470);
}

// Review of #229, P1: an on-time prompt whose first two 200 ms parents decode
// to 96 + 19,098 frames, over 180 on-time media chunks (7.7 s). 25d4010 ran the
// shared cursor ahead of media and later trimmed 16,220 (two parents) and
// 40,546 frames (six).
TEST_F(AudioPipelineRoute, WiredDefaultsPromptOverMediaLeavesMediaUntouched) {
    for (const std::uint64_t parents : {std::uint64_t{2}, std::uint64_t{6}}) {
        SCOPED_TRACE(parents);
        WiredRoute route{splitting_decoder()};
        ASSERT_TRUE(route.router);
        for (int k = 0; k < 180; ++k) {
            ASSERT_TRUE(route.submit_media(k * kA26ChunkNs));
            while (route.guidance_chunks < parents
                   && 200 * kMs + static_cast<std::int64_t>(route.guidance_chunks) * 200 * kMs
                       < (k + 1) * kA26ChunkNs) {
                ASSERT_TRUE(route.submit_guidance(
                    200 * kMs + static_cast<std::int64_t>(route.guidance_chunks) * 200 * kMs));
            }
            ASSERT_TRUE(route.drain());
        }
        ASSERT_EQ(route.guidance_chunks, parents);
        expect_nothing_lost(route);
        EXPECT_EQ(route.stats().playout_underruns, 0U);
        EXPECT_EQ(route.stats().frames_silence, 0U);
        // Media never moved: it plays 60 ms after its arrival throughout, so
        // the video hold stays there, and every output frame carries it.
        EXPECT_LE(route.largest_offset(), 61 * kMs);
        EXPECT_EQ(route.stats().frames_output, 180U * kA26Frames);
    }
}

// A resampler that returns almost nothing for several parents and then all of
// them at once is neither an underrun nor a gap that ends the session, alone or
// over media.
TEST_F(AudioPipelineRoute, WiredDefaultsSurviveADecoderHoldingSeveralParents) {
    for (const bool over_media : {false, true}) {
        SCOPED_TRACE(over_media);
        auto calls = std::make_shared<std::size_t>(0);
        auto held = std::make_shared<std::size_t>(0);
        WiredRoute route{[calls, held](const aa::audio::EncodedChunk& chunk)
                             -> aa::core::Result<std::vector<std::byte>> {
            if (chunk.input_format.sample_rate_hz == 48000) {
                return std::vector<std::byte>(chunk.payload.begin(), chunk.payload.end());
            }
            *held += chunk.payload.size() / 2 * 3;
            const auto frames = ++*calls < 4 ? std::size_t{96} : *held;
            *held -= frames;
            std::vector<std::byte> out(frames * 4);
            for (std::size_t i = 0; i < out.size(); i += 2) {
                const std::int16_t level = 1200;
                std::memcpy(out.data() + i, &level, sizeof(level));
            }
            return out;
        }};
        ASSERT_TRUE(route.router);
        std::uint64_t decoded = 0;
        for (int k = 0; k < 60; ++k) {
            if (over_media) { ASSERT_TRUE(route.submit_media(k * kA26ChunkNs)); }
            while (route.guidance_chunks < 8
                   && static_cast<std::int64_t>(route.guidance_chunks) * 200 * kMs < (k + 1) * kA26ChunkNs) {
                ASSERT_TRUE(route.submit_guidance(static_cast<std::int64_t>(route.guidance_chunks) * 200 * kMs));
            }
            ASSERT_TRUE(route.drain());
            decoded = route.stats().frames_decoded;
        }
        ASSERT_TRUE(route.router->flush());
        expect_nothing_lost(route);
        EXPECT_EQ(route.stats().playout_underruns, 0U);
        EXPECT_EQ(decoded, 8U * 9600U + (over_media ? 60U * kA26Frames : 0U));
        EXPECT_EQ(decoded + route.stats().frames_stretched,
                  route.stats().frames_retained + route.stats().frames_compressed);
        if (over_media) { EXPECT_LE(route.largest_offset(), 61 * kMs); }
    }
}

// Stall shapes of the wired A26 logs (2026-11-17, 1800 s drive): 283.6 ms sink
// stall, the next chunk 204.7 ms late, its backlog at once; and the largest
// lateness without a stall, 43.3 ms, which must cost nothing at all.
TEST_F(AudioPipelineRoute, WiredDefaultsReplayTheMeasuredStallWithoutLoss) {
    WiredRoute route;
    ASSERT_TRUE(route.router);
    int k = 0;
    for (; k < 100; ++k) { ASSERT_TRUE(route.media(k * kA26ChunkNs)); }
    ASSERT_TRUE(route.media(k * kA26ChunkNs + 43'300'000));
    ++k;
    for (; k < 200; ++k) { ASSERT_TRUE(route.media(k * kA26ChunkNs)); }
    EXPECT_EQ(route.stats().playout_underruns, 0U);
    EXPECT_EQ(route.stats().frames_silence, 0U);
    const auto resumed = k * kA26ChunkNs + 204'700'000;
    for (int burst = 0; burst < 5; ++burst, ++k) { ASSERT_TRUE(route.media(resumed + burst * kMs)); }
    for (; k < 400; ++k) { ASSERT_TRUE(route.media(k * kA26ChunkNs)); }
    EXPECT_EQ(route.stats().playout_underruns, 1U);
    expect_nothing_lost(route);
    expect_whole_tone(route, 1);
    EXPECT_LT(route.largest_offset(), 400 * kMs);
}

// The wired acceptance list (#228) on the production defaults: steady state, a
// burst on a fresh stream, a burst of guidance, and silence only for a real
// underrun.
TEST_F(AudioPipelineRoute, WiredPlayoutIsLosslessAcrossSteadyBurstsAndGuidanceOverMedia) {
    {
        WiredRoute route;
        ASSERT_TRUE(route.router);
        for (int k = 0; k < 8; ++k) { ASSERT_TRUE(route.media(k * kMs)); }
        expect_nothing_lost(route);
        expect_whole_tone(route, 0);
        EXPECT_EQ(route.stats().frames_silence, 0U);
        EXPECT_EQ(route.stats().playout_underruns, 0U);
        // 250 ms after the last slot: the phone sent nothing to play.
        ASSERT_TRUE(route.media(8 * kA26ChunkNs + 310 * kMs));
        EXPECT_EQ(route.stats().playout_underruns, 1U);
        EXPECT_GT(route.stats().frames_silence, 0U);
        expect_nothing_lost(route);
    }
    {
        WiredRoute route{splitting_decoder()};
        ASSERT_TRUE(route.router);
        for (const std::int64_t offset_ms : {0, 1, 10, 12, 12, 13, 20, 20}) {
            ASSERT_TRUE(route.submit_guidance(offset_ms * kMs));
            ASSERT_TRUE(route.drain());
        }
        // 8 parents x 200 ms at once, 1.6 s deep.
        expect_nothing_lost(route);
        EXPECT_EQ(route.stats().frames_silence, 0U);
        EXPECT_EQ(route.stats().playout_underruns, 0U);
    }
}

// A prompt keeps playing when media starves under it, and media's own hole is
// an underrun even though the prompt kept the shared cursor moving.
TEST_F(AudioPipelineRoute, WiredDefaultsCountAMediaUnderrunUnderAPrompt) {
    std::int64_t clock = 0;
    WiredRoute route{{}, [&clock](aa::audio::RouterConfig& config) {
        config.clock = [&clock] { return aa::core::Nanoseconds{clock}; };
    }};
    ASSERT_TRUE(route.router);
    const auto at = [&](std::int64_t offset) { clock = route.start + offset; };
    const auto run_to = [&](std::int64_t offset) {
        // What the worker does: step until held, sleep, step again.
        for (int i = 0; i < 4096; ++i) {
            auto stepped = route.router->render_step();
            ASSERT_TRUE(stepped);
            if (stepped.value().pending) { continue; }
            if (!stepped.value().wait_until || stepped.value().wait_until->count >= route.start + offset) { break; }
            clock = stepped.value().wait_until->count;
        }
        at(offset);
    };
    for (int k = 0; k < 6; ++k) {
        run_to(k * kA26ChunkNs);
        ASSERT_TRUE(route.submit_media(k * kA26ChunkNs));
    }
    for (int parent = 0; parent < 4; ++parent) {
        run_to(6 * kA26ChunkNs + parent * 200 * kMs);
        ASSERT_TRUE(route.submit_guidance(6 * kA26ChunkNs + parent * 200 * kMs));
    }
    // Media comes back 650 ms late with its backlog.
    const auto resumed = 6 * kA26ChunkNs + 650 * kMs;
    for (int k = 0; k < 8; ++k) {
        run_to(resumed + k * kMs);
        ASSERT_TRUE(route.submit_media(resumed + k * kMs));
    }
    run_to(resumed + 2'000 * kMs);
    EXPECT_EQ(route.stats().playout_underruns, 1U);
    expect_nothing_lost(route);
    const auto& stats = route.stats();
    EXPECT_EQ(stats.frames_decoded + stats.frames_stretched,
              stats.frames_retained + stats.frames_compressed);
}

// The hold (RenderStep::wait_until): a frame the prompt could play alone waits
// for media until 20 ms before its slot, then goes out 10 ms at a time.
TEST_F(AudioPipelineRoute, WiredDefaultsHoldAPromptForMediaUntilShortlyBeforeItsSlot) {
    std::int64_t clock = 0;
    WiredRoute route{{}, [&clock](aa::audio::RouterConfig& config) {
        config.clock = [&clock] { return aa::core::Nanoseconds{clock}; };
    }};
    ASSERT_TRUE(route.router);
    clock = route.start;
    ASSERT_TRUE(route.submit_media(0));
    ASSERT_TRUE(route.submit_guidance(0));
    auto stepped = route.router->render_step();
    ASSERT_TRUE(stepped);
    ASSERT_TRUE(stepped.value().pending);
    EXPECT_EQ(stepped.value().contributors.size(), 2U);
    EXPECT_EQ(route.stats().frames_output, kA26Frames);

    stepped = route.router->render_step();
    ASSERT_TRUE(stepped);
    EXPECT_FALSE(stepped.value().pending);
    EXPECT_TRUE(stepped.value().contributors.empty());
    ASSERT_TRUE(stepped.value().wait_until);
    const auto slot = route.start + 60 * kMs + kA26ChunkNs;
    EXPECT_NEAR(static_cast<double>(stepped.value().wait_until->count),
                static_cast<double>(slot - 20 * kMs), 25'000.0);
    EXPECT_EQ(route.stats().frames_output, kA26Frames);

    // Media is late: the prompt goes on, 10 ms at a time.
    clock = stepped.value().wait_until->count;
    stepped = route.router->render_step();
    ASSERT_TRUE(stepped);
    EXPECT_EQ(stepped.value().contributors.size(), 1U);
    EXPECT_NEAR(static_cast<double>(route.stats().frames_output - kA26Frames), 480.0, 2.0);
    // The late chunk joins right there, nothing lost.
    ASSERT_TRUE(route.submit_media(clock - route.start));
    stepped = route.router->render_step();
    ASSERT_TRUE(stepped);
    EXPECT_EQ(stepped.value().contributors.size(), 2U);
    // Once media was stopped there is nothing to wait for.
    route.router->cancel_pending(media);
    EXPECT_TRUE(route.drain());
    EXPECT_FALSE(route.router->render_step().value().wait_until);
    EXPECT_EQ(route.stats().frames_trimmed, 0U);
}

// pump()/flush() write a held remainder instead of waiting for it.
TEST_F(AudioPipelineRoute, WiredDefaultsFlushWritesAHeldPrompt) {
    WiredRoute route;
    ASSERT_TRUE(route.router);
    ASSERT_TRUE(route.submit_media(0));
    ASSERT_TRUE(route.submit_guidance(0));
    ASSERT_TRUE(route.drain());
    EXPECT_EQ(route.stats().frames_output, kA26Frames);
    ASSERT_TRUE(route.router->flush());
    // The whole parent up to its last input sample (frame 9,597 at 48 kHz), the
    // resampler's last few included.
    EXPECT_GE(route.stats().frames_output, 9'598U);
    EXPECT_EQ(route.stats().frames_decoded, route.stats().frames_retained);
}

// Pulse backpressure: the sink accepts a piece only once its end is within the
// 200 ms ring, and arrivals are submitted only between writes (the worker).
// Split prompts over media, then a post-drought burst under a prompt.
TEST_F(AudioPipelineRoute, WiredDefaultsStayLosslessBehindAPacedSink) {
    std::int64_t clock = 0;
    WiredRoute route{splitting_decoder(), [&clock](aa::audio::RouterConfig& config) {
        config.clock = [&clock] { return aa::core::Nanoseconds{clock}; };
        const auto record = config.on_write_completed;
        config.on_write_completed = [&clock, record](const auto& value) {
            clock = std::max(clock, value.scheduled_end.count - 200 * kMs);
            record(value);
        };
    }};
    ASSERT_TRUE(route.router);
    struct Arrival {
        std::int64_t at;
        bool media;
    };
    std::vector<Arrival> arrivals;
    for (int k = 0; k < 400; ++k) {
        // Chunks 201..207 wait out a drought and arrive with the 208th, the
        // most the phone's 341 ms capture buffer holds.
        const bool late = k > 200 && k < 208;
        arrivals.push_back({late ? 208 * kA26ChunkNs + (k - 208) * kMs : k * kA26ChunkNs, true});
    }
    for (int parent = 0; parent < 6; ++parent) { arrivals.push_back({300 * kMs + parent * 200 * kMs, false}); }
    for (int parent = 0; parent < 6; ++parent) { arrivals.push_back({8'400 * kMs + parent * 200 * kMs, false}); }
    std::stable_sort(arrivals.begin(), arrivals.end(),
                     [](const auto& a, const auto& b) { return a.at < b.at; });
    clock = route.start;
    std::size_t next = 0;
    for (int i = 0; i < 100'000; ++i) {
        while (next < arrivals.size() && route.start + arrivals[next].at <= clock) {
            ASSERT_TRUE(arrivals[next].media ? route.submit_media(arrivals[next].at)
                                             : route.submit_guidance(arrivals[next].at));
            ++next;
        }
        auto stepped = route.router->render_step();
        ASSERT_TRUE(stepped);
        if (stepped.value().pending) { continue; }
        const auto wake = stepped.value().wait_until;
        if (next < arrivals.size()) {
            const auto due = route.start + arrivals[next].at;
            clock = std::max(clock, wake ? std::min(wake->count, due) : due);
        } else if (wake) {
            clock = std::max(clock, wake->count);
        } else {
            break;
        }
    }
    ASSERT_EQ(next, arrivals.size());
    expect_nothing_lost(route);
    EXPECT_EQ(route.stats().playout_underruns, 1U);
    EXPECT_LT(route.largest_offset(), 400 * kMs);
    EXPECT_EQ(route.stats().frames_overflowed, 0U);
    EXPECT_EQ(route.stats().frames_decoded + route.stats().frames_stretched,
              route.stats().frames_retained + route.stats().frames_compressed);
}

// The emergency bound: nothing is cut up to a 2 s backlog; beyond it only the
// excess goes, and it is counted apart from every other trim.
TEST_F(AudioPipelineRoute, WiredDefaultsCutOnlyBeyondTheEmergencyBacklog) {
    EXPECT_EQ(aa::audio::pcm_router_config().playout.emergency_backlog,
              aa::core::Nanoseconds{2'000 * kMs});
    WiredRoute route;
    ASSERT_TRUE(route.router);
    // Chunk n is 60 ms + n x 42.67 ms deep: n = 45 stays under 2 s.
    for (int k = 0; k < 46; ++k) { ASSERT_TRUE(route.media(0)); }
    expect_nothing_lost(route);
    EXPECT_EQ(route.stats().playout_overflows, 0U);
    for (int k = 0; k < 8; ++k) { ASSERT_TRUE(route.media(0)); }
    const auto& stats = route.stats();
    EXPECT_GT(stats.playout_overflows, 0U);
    EXPECT_EQ(stats.frames_trimmed, stats.frames_overflowed);
    EXPECT_EQ(stats.frames_shed, 0U);
    EXPECT_EQ(stats.frames_caught_up, 0U);
    // The route still holds two seconds of audio.
    EXPECT_GT(stats.frames_output, 93'000U);
    EXPECT_EQ(stats.frames_decoded, stats.frames_retained + stats.frames_trimmed);
    // A shorter bound than a second is refused.
    auto config = aa::audio::pcm_router_config();
    config.playout.emergency_backlog = aa::core::Nanoseconds{999 * kMs};
    EXPECT_FALSE(aa::audio::detail::open_router_with_sink_for_test(
        config, std::make_unique<RecordingAudioSink>()));
}

// Rate correction stays inaudible: at most 0.5% (1% while more than 100 ms
// over), moving by at most 0.1% per A26 chunk.
TEST_F(AudioPipelineRoute, WiredDefaultsRateCorrectionIsBoundedAndGradual) {
    WiredRoute route;
    ASSERT_TRUE(route.router);
    for (int k = 0; k < 6; ++k) { ASSERT_TRUE(route.media(k * kA26ChunkNs)); }
    const auto resumed = 6 * kA26ChunkNs + 400 * kMs;
    for (int k = 0; k < 8; ++k) { ASSERT_TRUE(route.media(resumed + k * kMs)); }
    auto before = route.stats().frames_compressed;
    std::int64_t previous = 0;
    std::int64_t largest = 0;
    for (int k = 1; k < 1'200; ++k) {
        const auto depth = route.stats().playout_depth.count;
        ASSERT_TRUE(route.media(resumed + 7 * kMs + k * kA26ChunkNs));
        const auto removed = static_cast<std::int64_t>(route.stats().frames_compressed - before);
        before = route.stats().frames_compressed;
        largest = std::max(largest, removed);
        EXPECT_LE(removed, depth > 160 * kMs ? 21 : 13) << k; // 2048 x 1% / 0.5%, ramp and carry
        EXPECT_LE(std::abs(removed - previous), 4) << k;      // 0.1% and the carry
        previous = removed;
    }
    EXPECT_GE(largest, 19);
    EXPECT_EQ(route.stats().frames_stretched, 0U);
    EXPECT_EQ(previous, 0);
}

// Re-review of #229, P2: a phone clock 500 ppm off on packets far shorter than
// A26's. 4a3f49f never corrected a run under 256 frames: 240-frame packets were
// 90 ms deep after a minute and overflowed after 65.
TEST_F(AudioPipelineRoute, WiredDefaultsFollowThePhoneClockOnShortPackets) {
    struct Case {
        std::uint64_t frames;
        int seconds;
        int ppm; // of the phone's clock: positive is fast
    };
    for (const Case c : {Case{240, 300, 500}, Case{240, 120, -500}, Case{480, 120, 500},
                         Case{960, 120, 500}, Case{96, 60, 500}, Case{48, 30, 800}}) {
        SCOPED_TRACE(std::to_string(c.frames) + " frames, " + std::to_string(c.ppm) + " ppm");
        WiredRoute route;
        ASSERT_TRUE(route.router);
        const auto nominal = static_cast<std::int64_t>(c.frames) * 1'000'000'000 / 48000;
        const auto cadence = nominal - nominal * c.ppm / 1'000'000;
        const auto packets = static_cast<std::uint64_t>(c.seconds) * 48000 / c.frames;
        for (std::uint64_t k = 0; k < packets; ++k) {
            const auto pcm = tone_pcm(k * c.frames, c.frames);
            ASSERT_TRUE(route.router->submit(make_message(
                aa::protocol::ChannelRole::media_audio, k + 1,
                {route.start + static_cast<std::int64_t>(k) * cadence}, pcm)));
            ASSERT_TRUE(route.drain());
        }
        expect_nothing_lost(route);
        EXPECT_EQ(route.stats().playout_underruns, 0U);
        EXPECT_EQ(route.stats().frames_silence, 0U);
        // Fast: within the dead band's equilibrium of 60 ms, where 4a3f49f is
        // 60 ms + ppm x time. Slow: not below the half it refills to.
        EXPECT_LT(*route.stats().playout_depth_max, aa::core::Nanoseconds{80 * kMs});
        EXPECT_GT(*route.stats().playout_depth_min, aa::core::Nanoseconds{25 * kMs});
        const auto corrected = route.stats().frames_compressed + route.stats().frames_stretched;
        const auto drift = packets * c.frames * static_cast<std::uint64_t>(std::abs(c.ppm)) / 1'000'000;
        // Whole 10 ms + 2 ms of drift stay inside the dead band.
        if (c.ppm > 0 && drift > 1'000) { EXPECT_NEAR(static_cast<double>(corrected), static_cast<double>(drift), 800.0); }
        EXPECT_EQ(route.stats().frames_decoded + route.stats().frames_stretched,
                  route.stats().frames_retained + route.stats().frames_compressed);
        // A frame taken out of 40 is 2.5% over those: the tone's slope by as much.
        EXPECT_LE(largest_step(route.left()), c.frames < 64 ? 480 : 470);
    }
}

// The correction a short run cannot take is never released in one burst:
// 4a3f49f spent 32 frames (1.56%) on the first long run after 1,600 short ones.
TEST_F(AudioPipelineRoute, WiredDefaultsNeverReleaseCarriedCorrectionInABurst) {
    WiredRoute route;
    ASSERT_TRUE(route.router);
    constexpr std::uint64_t kShort = 240;
    std::uint64_t frame = 0;
    std::uint64_t sequence = 0;
    const auto send = [&](std::int64_t at, std::uint64_t frames) {
        const auto pcm = tone_pcm(frame, frames);
        frame += frames;
        return route.router->submit(make_message(aa::protocol::ChannelRole::media_audio, ++sequence,
                                                 {route.start + at}, pcm)).has_value() && route.drain();
    };
    // Seven A26 chunks' worth at once, then 8 s on cadence.
    for (int k = 0; k < 60; ++k) { ASSERT_TRUE(send(0, kShort)); }
    for (int k = 1; k <= 1'600; ++k) { ASSERT_TRUE(send(k * 5 * kMs, kShort)); }
    EXPECT_LT(route.stats().playout_depth, aa::core::Nanoseconds{330 * kMs}); // it drained meanwhile
    const auto before = route.stats().frames_compressed;
    ASSERT_TRUE(send(1'601 * 5 * kMs, kA26Frames));
    EXPECT_LE(route.stats().frames_compressed - before, 21U); // 1% of 2048
    expect_nothing_lost(route);
    EXPECT_LE(largest_step(route.left()), 470);
}

// Re-review of #229, P2: the resampler holds about 100 frames of every guidance
// parent. After a cancel or a session reset they belong to a stream that is
// gone; 4a3f49f played 97 of them at the head of the next prompt.
TEST_F(AudioPipelineRoute, WiredDefaultsCancelAndResetDropWhatTheDecoderHeld) {
    for (const bool session_reset : {false, true}) {
        SCOPED_TRACE(session_reset);
        WiredRoute route;
        ASSERT_TRUE(route.router);
        ASSERT_TRUE(route.submit_guidance(0, 12'000));
        ASSERT_TRUE(route.drain());
        if (session_reset) { route.router->reset_session_timeline(); }
        else { route.router->cancel_pending(aa::protocol::ChannelRole::guidance_audio); }
        route.sink->writes.clear();
        ASSERT_TRUE(route.submit_guidance(1'000 * kMs, -12'000));
        ASSERT_TRUE(route.drain());
        const auto samples = route.left();
        EXPECT_EQ(std::count_if(samples.begin(), samples.end(), [](auto v) { return v > 1'000; }), 0);
        EXPECT_GT(std::count_if(samples.begin(), samples.end(), [](auto v) { return v < -1'000; }), 9'000);
    }
}

// The same tail at a regular end is sound: flush() plays it (4a3f49f: 9,502 of
// 9,600 frames).
TEST_F(AudioPipelineRoute, WiredDefaultsFlushPlaysWhatTheDecoderHolds) {
    WiredRoute route;
    ASSERT_TRUE(route.router);
    ASSERT_TRUE(route.submit_guidance(0, 12'000));
    ASSERT_TRUE(route.router->flush());
    // Output frame 9,597 is the last input sample's own time.
    EXPECT_GE(route.stats().frames_retained, 9'598U);
    EXPECT_LE(route.stats().frames_retained, 9'600U + 64U);
    EXPECT_EQ(route.stats().frames_decoded, route.stats().frames_retained);
    const auto samples = route.left();
    EXPECT_GE(std::count_if(samples.begin(), samples.end(), [](auto v) { return v > 1'000; }), 9'580);
}

// Re-review of #229, P2: a write that blocks past its slot leaves the sink's
// read position ahead of every slot up to then. 4a3f49f wrote the backlog there
// (a ring buffer throws it away) and reported 60 ms throughout; now what follows
// plays later, says so, and drains.
TEST_F(AudioPipelineRoute, WiredDefaultsRetimeBehindABlockedWrite) {
    for (const std::int64_t stall_ms : {25, 100, 300, 600, 1'600}) {
        SCOPED_TRACE(stall_ms);
        std::int64_t clock = 0;
        WiredRoute route{{}, [&clock](aa::audio::RouterConfig& config) {
            config.clock = [&clock] { return aa::core::Nanoseconds{clock}; };
        }};
        ASSERT_TRUE(route.router);
        std::size_t stalled_write = SIZE_MAX;
        std::vector<std::int64_t> handed;
        route.sink->during_write = [&] {
            handed.push_back(clock); // the moment this piece reaches the sink
            if (handed.size() == stalled_write) { clock += 60 * kMs + stall_ms * kMs; }
        };
        std::int64_t next = 0;
        const auto run_to = [&](int chunk) {
            while (next < chunk) {
                clock = std::max(clock, route.start + next * kA26ChunkNs);
                // What arrived while the last write blocked is submitted together.
                do { ASSERT_TRUE(route.submit_media(next * kA26ChunkNs)); }
                while (++next < chunk && route.start + next * kA26ChunkNs <= clock);
                ASSERT_TRUE(route.drain());
            }
        };
        run_to(100);
        EXPECT_LE(route.largest_offset(), 60 * kMs + 1'000);
        stalled_write = handed.size() + 1;
        run_to(101); // blocks until stall_ms after its slot
        const auto settled = handed.size();
        run_to(160);
        expect_nothing_lost(route);
        EXPECT_EQ(route.stats().playout_underruns, 0U);
        EXPECT_EQ(route.stats().frames_silence, 0U);
        ASSERT_EQ(route.sink->write_ns.size(), handed.size());
        // Every later piece reaches the sink before its slot ...
        for (std::size_t i = settled; i < handed.size(); ++i) {
            EXPECT_GT(route.sink->write_ns[i], handed[i]) << i;
        }
        // ... the reported offset carries the delay it plays with, and the
        // content clock is continuous across the stall.
        std::int64_t content = 0;
        for (const auto& write : route.writes) {
            const auto start = write.scheduled.count - write.playout_offset.count;
            if (content != 0) { EXPECT_NEAR(static_cast<double>(start - content), 0.0, 50'000.0); }
            content = write.scheduled_end.count - write.playout_end_offset.count;
        }
        // A stall within the piece's own 42.67 ms leaves the next one in time.
        const auto expected = stall_ms * kMs < kA26ChunkNs
            ? 60 * kMs : (60 + stall_ms + 20) * kMs - kA26ChunkNs;
        EXPECT_NEAR(static_cast<double>(route.largest_offset()), static_cast<double>(expected), 2.0e6);
        // It drains by the rate correction, as any backlog.
        if (stall_ms == 300) {
            run_to(160 + 40'000 / 42);
            EXPECT_LT(route.stats().playout_depth, aa::core::Nanoseconds{75 * kMs});
            expect_nothing_lost(route);
            expect_whole_tone(route, 0);
            EXPECT_LE(largest_step(route.left()), 470);
        }
        EXPECT_EQ(route.stats().frames_decoded + route.stats().frames_stretched,
                  route.stats().frames_retained + route.stats().frames_compressed);
    }
}

// Live A26 check of d0c5b41 (#228): a 1 s pipewire-pulse pause. pulsesink's
// clock stops with its server, so it plays on from where it was; stamps moved
// on by a retime were a gap it waited out in silence. Each later write blocked
// 0.6-0.85 s, was taken for a new stall and retimed on top of the last one:
// 28 retimes, depth 2.75 s, 6.3 s cut by the 2 s bound. This fake is that
// sink (as measured with a private pipewire-pulse): its clock stands still
// through the pause, it waits out 90% of a forward jump in its stamps (live,
// its clock slaving took the rest back, so each block decayed), and drops
// what is stamped behind its read position.
struct PausingSink final {
    std::int64_t* clock;
    bool holds{true};                  // honours hold_stream()
    std::int64_t lag{};                // how long its clock stood still
    std::int64_t held{};               // hold_stream() total
    std::optional<std::int64_t> end{}; // its stream time after the last sample
    std::int64_t pause_next{};         // stands still this long in the next write
    std::int64_t waited{};             // silence waited out for jumps
    std::int64_t dropped{};            // stream time written behind its read position
    std::int64_t longest_block{};      // after the pause: longest write past its start

    void attach(RecordingSinkState& state) {
        state.on_hold = [this](aa::core::Nanoseconds by) {
            if (!holds) { return false; }
            held += by.count;
            return true;
        };
        state.on_write = [this](const aa::audio::MixedChunk& chunk) {
            const auto length = static_cast<std::int64_t>(chunk.pcm.size() / 4) * 1'000'000'000 / 48'000;
            const auto pts = chunk.timestamp.count - held;
            if (!end) { end = pts; }
            // Dry: what it plays now moved on in silence.
            const auto reading = *clock - lag;
            dropped += std::clamp<std::int64_t>(reading - pts, 0, length);
            end = std::max(*end, reading);
            auto start = *end;
            if (const auto jump = pts - *end; jump > 40 * kMs) {
                waited += jump * 9 / 10;
                start += jump * 9 / 10;
            }
            const bool paused = pause_next != 0;
            lag += std::exchange(pause_next, 0);
            end = start + length;
            // It takes the write when its read position reaches the write's start.
            const auto taken = std::max(*clock, start + lag);
            if (!paused && lag != 0) { longest_block = std::max(longest_block, taken - *clock); }
            *clock = taken;
        };
    }
};

TEST_F(AudioPipelineRoute, WiredDefaultsPlayOnThroughASinkThatStoodStill) {
    for (const std::int64_t pause_ms : {300, 1'000, 1'600}) {
        SCOPED_TRACE(pause_ms);
        std::int64_t clock = 0;
        WiredRoute route{{}, [&clock](aa::audio::RouterConfig& config) {
            config.clock = [&clock] { return aa::core::Nanoseconds{clock}; };
        }};
        ASSERT_TRUE(route.router);
        PausingSink sink{&clock};
        sink.attach(*route.sink);
        std::int64_t next = 0;
        const auto run_to = [&](std::int64_t chunk) {
            while (next < chunk) {
                clock = std::max(clock, route.start + next * kA26ChunkNs);
                do { ASSERT_TRUE(route.submit_media(next * kA26ChunkNs)); }
                while (++next < chunk && route.start + next * kA26ChunkNs <= clock);
                ASSERT_TRUE(route.drain());
            }
        };
        run_to(100);
        EXPECT_LE(route.largest_offset(), 60 * kMs + 1'000);
        sink.pause_next = pause_ms * kMs;
        run_to(101);
        run_to(101 + 10'000 / 42); // 10 s
        const auto& stats = route.stats();
        expect_nothing_lost(route);
        EXPECT_EQ(stats.playout_overflows, 0U);
        EXPECT_EQ(stats.frames_overflowed, 0U);
        EXPECT_EQ(stats.playout_underruns, 0U);
        // One retime, held by the sink whole: no gap, nothing behind its read
        // position, no write blocked longer than a chunk again.
        EXPECT_EQ(stats.playout_retimes, 1U);
        EXPECT_EQ(sink.waited, 0);
        EXPECT_EQ(sink.dropped, 0);
        EXPECT_LE(sink.longest_block, kA26ChunkNs);
        const auto events = route.router->take_playout_events();
        ASSERT_EQ(events.size(), 2U);
        EXPECT_EQ(events[1].kind, aa::audio::PlayoutEvent::Kind::retime);
        EXPECT_EQ(events[1].held, events[1].late);
        EXPECT_EQ(sink.held, events[1].late.count);
        // The offset carries what the sink plays with: about the pause.
        EXPECT_NEAR(static_cast<double>(route.largest_offset()),
                    static_cast<double>((60 + pause_ms) * kMs), 45.0e6);
        // It drains by the rate correction, about 10 ms/s.
        const auto after = route.stats().playout_depth.count;
        run_to(101 + 70'000 / 42); // 60 s more
        EXPECT_LT(route.stats().playout_depth.count, std::max(80 * kMs, after - 400 * kMs));
        EXPECT_EQ(route.stats().playout_retimes, 1U);
        EXPECT_EQ(sink.waited, 0);
        EXPECT_EQ(sink.dropped, 0);
        expect_nothing_lost(route);
        expect_whole_tone(route, 0);
        EXPECT_EQ(route.stats().frames_decoded + route.stats().frames_stretched,
                  route.stats().frames_retained + route.stats().frames_compressed);
    }
}

// A sink that cannot hold still cascades after a pause (as d0c5b41 did with
// pulsesink) until the 2 s bound cuts. That cut can take a role's whole queue
// in one step; d0c5b41 then planned the runs it had just popped and crashed.
TEST_F(AudioPipelineRoute, WiredDefaultsCutAWholeQueueBehindASinkThatCannotHold) {
    std::int64_t clock = 0;
    WiredRoute route{{}, [&clock](aa::audio::RouterConfig& config) {
        config.clock = [&clock] { return aa::core::Nanoseconds{clock}; };
    }};
    ASSERT_TRUE(route.router);
    PausingSink sink{&clock};
    sink.holds = false;
    sink.attach(*route.sink);
    std::int64_t next = 0;
    const auto run_to = [&](std::int64_t chunk) {
        while (next < chunk) {
            clock = std::max(clock, route.start + next * kA26ChunkNs);
            do { ASSERT_TRUE(route.submit_media(next * kA26ChunkNs)); }
            while (++next < chunk && route.start + next * kA26ChunkNs <= clock);
            ASSERT_TRUE(route.drain());
        }
    };
    run_to(100);
    sink.pause_next = 1'000 * kMs;
    run_to(101 + 30'000 / 42);
    const auto& stats = route.stats();
    EXPECT_FALSE(route.router->report().render_failure);
    EXPECT_GT(stats.playout_retimes, 3U);
    EXPECT_GT(stats.playout_overflows, 0U);
    EXPECT_EQ(stats.frames_trimmed, stats.frames_overflowed);
    EXPECT_GT(sink.waited, 1'000 * kMs);
    EXPECT_EQ(sink.held, 0);
}

// The other way a slot passes: this thread was held up between writes and the
// sink played on dry. Nothing is held; its stamps move on with the schedule,
// or what follows lands behind its read position. A sink that cannot hold
// gets moved stamps after a pause, as before.
TEST_F(AudioPipelineRoute, WiredDefaultsMoveTheStampsOfASinkThatPlayedOnDry) {
    for (const bool holds : {true, false}) {
        SCOPED_TRACE(holds);
        std::int64_t clock = 0;
        WiredRoute route{{}, [&clock](aa::audio::RouterConfig& config) {
            config.clock = [&clock] { return aa::core::Nanoseconds{clock}; };
        }};
        ASSERT_TRUE(route.router);
        PausingSink sink{&clock};
        sink.holds = holds;
        sink.attach(*route.sink);
        std::int64_t next = 0;
        const auto run_to = [&](std::int64_t chunk) {
            while (next < chunk) {
                clock = std::max(clock, route.start + next * kA26ChunkNs);
                do { ASSERT_TRUE(route.submit_media(next * kA26ChunkNs)); }
                while (++next < chunk && route.start + next * kA26ChunkNs <= clock);
                ASSERT_TRUE(route.drain());
            }
        };
        run_to(100);
        clock += 1'000 * kMs; // held up before the next write
        run_to(101 + 10'000 / 42);
        expect_nothing_lost(route);
        EXPECT_EQ(route.stats().playout_overflows, 0U);
        EXPECT_EQ(route.stats().playout_retimes, 1U);
        EXPECT_EQ(sink.held, 0);
        EXPECT_EQ(sink.dropped, 0);
        EXPECT_EQ(sink.waited, 0);
        const auto events = route.router->take_playout_events();
        ASSERT_EQ(events.size(), 2U);
        EXPECT_EQ(events[1].held.count, 0);
    }
}

// Sol re-check of 89875dc (#229): media resumes after a quiet gap, the step
// that skips the gap returns, and this thread is then held while the sink's
// clock runs. The refill silence and the media after it are planned again by
// the clock instead of written in the past, and the silence going in late is
// no hold: the packet plays, or what is too old by its receipt is cut and
// counted.
TEST_F(AudioPipelineRoute, WiredDefaultsPlaceAResumeAgainAfterAPauseThatFollowsItsSkip) {
    for (const std::int64_t pause_ms : {0, 300, 2'100}) {
        SCOPED_TRACE(pause_ms);
        std::int64_t clock = 0;
        WiredRoute route{{}, [&clock](aa::audio::RouterConfig& config) {
            config.clock = [&clock] { return aa::core::Nanoseconds{clock}; };
        }};
        ASSERT_TRUE(route.router);
        PausingSink sink{&clock};
        sink.attach(*route.sink);
        // Received when stamped, on this clock.
        const auto submit = [&](std::int64_t offset_ns) {
            clock = route.start + offset_ns;
            const auto pcm = tone_pcm(route.media_chunks * kA26Frames, kA26Frames);
            return route.router->submit(make_message(aa::protocol::ChannelRole::media_audio, ++route.media_chunks,
                                                     {clock}, pcm), aa::core::Nanoseconds{clock}).has_value();
        };
        ASSERT_TRUE(submit(0));
        ASSERT_TRUE(route.drain());
        ASSERT_TRUE(submit(600 * kMs));
        auto stepped = route.router->render_step();
        ASSERT_TRUE(stepped);
        ASSERT_TRUE(stepped.value().pending);
        clock += pause_ms * kMs;
        ASSERT_TRUE(route.drain());
        const auto& stats = route.stats();
        EXPECT_FALSE(route.router->report().render_failure);
        EXPECT_EQ(sink.dropped, 0);
        EXPECT_EQ(sink.held, 0);
        EXPECT_EQ(stats.frames_retained + stats.frames_overflowed, 2 * kA26Frames);
        EXPECT_EQ(stats.frames_overflowed, pause_ms > 2'000 ? kA26Frames : 0U);
    }
}

// What on_write_completed says the piece really plays with: the blocked piece
// from its return, the next from the retimed slot. Stamps of another clock are
// not lateness.
TEST_F(AudioPipelineRoute, WiredDefaultsReportTheEndpointOfABlockedWrite) {
    std::int64_t clock = 0;
    WiredRoute route{{}, [&clock](aa::audio::RouterConfig& config) {
        config.clock = [&clock] { return aa::core::Nanoseconds{clock}; };
    }};
    ASSERT_TRUE(route.router);
    clock = route.start;
    ASSERT_TRUE(route.media(0));
    ASSERT_EQ(route.writes.size(), 1U);
    EXPECT_EQ(route.writes[0].endpoint_offset, route.writes[0].playout_offset);
    EXPECT_EQ(route.writes[0].endpoint_end_offset, route.writes[0].playout_end_offset);
    clock = route.start + kA26ChunkNs;
    route.sink->during_write = [&] { clock += 360 * kMs; }; // 300 ms past its slot
    ASSERT_TRUE(route.submit_media(kA26ChunkNs));
    ASSERT_TRUE(route.router->render_step());
    route.sink->during_write = {};
    ASSERT_EQ(route.writes.size(), 2U);
    EXPECT_NEAR(static_cast<double>(route.writes[1].endpoint_offset.count), 360.0e6, 1.0e5);
    EXPECT_NEAR(static_cast<double>(route.writes[1].endpoint_end_offset.count),
                380.0e6 - static_cast<double>(kA26ChunkNs), 1.0e5);
    for (int k = 2; k < 9; ++k) { ASSERT_TRUE(route.submit_media(k * kA26ChunkNs)); }
    ASSERT_TRUE(route.drain());
    EXPECT_EQ(route.stats().playout_retimes, 1U);
    EXPECT_NEAR(static_cast<double>(route.writes[2].playout_offset.count),
                380.0e6 - static_cast<double>(kA26ChunkNs), 1.0e5);
    const auto events = route.router->take_playout_events();
    ASSERT_EQ(events.size(), 2U);
    EXPECT_EQ(events[1].kind, aa::audio::PlayoutEvent::Kind::retime);

    WiredRoute stale{{}, [&clock](aa::audio::RouterConfig& config) {
        config.clock = [&clock] { return aa::core::Nanoseconds{clock}; };
    }};
    ASSERT_TRUE(stale.router);
    clock = stale.start + 3'600'000 * kMs;
    for (int k = 0; k < 4; ++k) { ASSERT_TRUE(stale.media(k * kA26ChunkNs)); }
    EXPECT_EQ(stale.stats().playout_retimes, 0U);
    EXPECT_EQ(stale.writes.back().endpoint_offset, stale.writes.back().playout_offset);
}

// end_stream(): everything submitted plays out, decoder tail included; nothing
// waits for the role; the next chunk starts a stream of its own.
TEST_F(AudioPipelineRoute, WiredDefaultsEndOfStreamPlaysOutAndStartsAfresh) {
    std::int64_t clock = 0;
    WiredRoute route{{}, [&clock](aa::audio::RouterConfig& config) {
        config.clock = [&clock] { return aa::core::Nanoseconds{clock}; };
    }};
    ASSERT_TRUE(route.router);
    const auto guidance = aa::protocol::ChannelRole::guidance_audio;
    clock = route.start;
    ASSERT_TRUE(route.submit_guidance(0, 12'000));
    EXPECT_TRUE(route.router->end_stream(guidance));
    EXPECT_FALSE(route.router->end_stream(aa::protocol::ChannelRole::media_audio));
    ASSERT_TRUE(route.submit_media(0));
    // Media is ended too: the prompt does not wait for its next chunk.
    EXPECT_TRUE(route.router->end_stream(aa::protocol::ChannelRole::media_audio));
    ASSERT_TRUE(route.drain());
    EXPECT_GE(route.stats().frames_retained, 9'598U + kA26Frames);
    EXPECT_EQ(route.stats().frames_decoded, route.stats().frames_retained);
    EXPECT_FALSE(route.router->end_stream(guidance)); // nothing left to play
    // 400 ms after the first prompt's end: not an underrun, a new stream.
    clock = route.start + 660 * kMs;
    const auto anchors = route.stats().playout_anchors;
    ASSERT_TRUE(route.submit_guidance(660 * kMs, -12'000));
    route.sink->writes.clear();
    ASSERT_TRUE(route.drain());
    EXPECT_EQ(route.stats().playout_underruns, 0U);
    EXPECT_EQ(route.stats().playout_anchors, anchors + 1);
    EXPECT_EQ(route.stats().playout_depth, aa::core::Nanoseconds{60 * kMs});
    const auto samples = route.left();
    EXPECT_EQ(std::count_if(samples.begin(), samples.end(), [](auto v) { return v > 1'000; }), 0);
    // Only lossless playout plays a stopped stream out.
    auto config = aa::audio::pcm_router_config();
    config.playout.lossless = false;
    auto bounded = aa::audio::detail::open_router_with_sink_for_test(
        config, std::make_unique<RecordingAudioSink>());
    ASSERT_TRUE(bounded);
    EXPECT_FALSE(bounded.value().end_stream(guidance));
}

namespace {

constexpr std::int64_t kBoundNs = 2'000 * kMs; // PlayoutPolicy::emergency_backlog
constexpr double kFrameNs = 1e9 / 48000.0;

// Nothing is unaccounted for, and every cut sample is an overflow.
void expect_only_overflow(const WiredRoute& route) {
    const auto& stats = route.stats();
    EXPECT_EQ(stats.frames_trimmed, stats.frames_overflowed);
    EXPECT_EQ(stats.frames_decoded + stats.frames_stretched,
              stats.frames_retained + stats.frames_compressed + stats.frames_trimmed);
    EXPECT_FALSE(route.router->report().render_failure);
}

} // namespace

// Third review of #229, P2: the overflow decision was cached before the retime
// that moved the run beyond the bound. 07b82f0 placed the second chunk here
// 2,009 ms after its arrival and cut nothing.
TEST_F(AudioPipelineRoute, WiredDefaultsBoundAHeadThatARetimeMovesBeyondIt) {
    std::int64_t clock = 0;
    WiredRoute route{{}, [&clock](aa::audio::RouterConfig& config) {
        config.clock = [&clock] { return aa::core::Nanoseconds{clock}; };
    }};
    ASSERT_TRUE(route.router);
    clock = route.start;
    ASSERT_TRUE(route.media(0));
    clock = route.start + 1'990 * kMs;
    ASSERT_TRUE(route.media(1 * kMs));
    EXPECT_EQ(route.stats().playout_retimes, 1U);
    // 1,990 + 20 ms of lead, 1 ms after the arrival: 9 ms beyond.
    EXPECT_NEAR(static_cast<double>(route.stats().frames_overflowed), 432.0, 1.0);
    EXPECT_EQ(route.stats().playout_overflows, 1U);
    EXPECT_LE(route.stats().playout_offset.count, kBoundNs + 21'000);
    expect_only_overflow(route);
}

// The same with a backlog before the stall: the retimed head itself crosses the
// bound, after the rate correction had already shortened it, and is not warped
// again.
TEST_F(AudioPipelineRoute, WiredDefaultsBoundADeepBacklogThatAStallMovesBeyondIt) {
    std::int64_t clock = 0;
    WiredRoute route{{}, [&clock](aa::audio::RouterConfig& config) {
        config.clock = [&clock] { return aa::core::Nanoseconds{clock}; };
    }};
    ASSERT_TRUE(route.router);
    // A burst 1.8 s deep, then 6 s at the phone's cadence: the window holds nothing
    // of the burst any more and the rate correction runs.
    clock = route.start;
    for (int k = 0; 60 * kMs + k * kA26ChunkNs < 1'800 * kMs; ++k) { ASSERT_TRUE(route.media(0)); }
    std::int64_t at = 0;
    for (int k = 1; k <= 140; ++k) {
        at = k * kA26ChunkNs;
        clock = route.start + at;
        ASSERT_TRUE(route.media(at));
    }
    EXPECT_EQ(route.stats().frames_overflowed, 0U);
    const auto compressed = route.stats().frames_compressed;
    ASSERT_GT(compressed, 0U);
    // The next chunk's write blocks; the head behind it, 1 ms younger, is then
    // retimed to 15 ms beyond the bound.
    at += kA26ChunkNs;
    clock = route.start + at;
    bool stall = true;
    route.sink->during_write = [&] {
        if (std::exchange(stall, false)) { clock = route.start + at + kMs + kBoundNs + (15 - 20) * kMs; }
    };
    ASSERT_TRUE(route.submit_media(at));
    ASSERT_TRUE(route.submit_media(at + kMs));
    ASSERT_TRUE(route.drain());
    EXPECT_EQ(route.stats().playout_retimes, 1U);
    EXPECT_NEAR(static_cast<double>(route.stats().frames_overflowed), 720.0, 1.0);
    EXPECT_EQ(route.stats().playout_overflows, 1U);
    EXPECT_LE(route.stats().playout_offset.count, kBoundNs + 21'000);
    EXPECT_LE(route.stats().frames_compressed - compressed, 2U * 21U); // each chunk once
    expect_only_overflow(route);
}

// A retime in the middle of a placed chunk cuts the rest of it.
TEST_F(AudioPipelineRoute, WiredDefaultsBoundTheRestOfAPlacedChunkAcrossARetime) {
    // 250 ms of stall leaves part of the rest, 1,900 ms nothing.
    for (const std::int64_t stall_ms : {250, 1'900}) {
        SCOPED_TRACE(stall_ms);
        std::int64_t clock = 0;
        WiredRoute route{{}, [&clock](aa::audio::RouterConfig& config) {
            config.clock = [&clock] { return aa::core::Nanoseconds{clock}; };
        }};
        ASSERT_TRUE(route.router);
        clock = route.start;
        const auto submit = [&](std::int64_t at) { // 100 ms parents: two pieces each
            const auto pcm = tone_pcm(route.media_chunks * 4'800, 4'800);
            return route.router->submit(make_message(aa::protocol::ChannelRole::media_audio,
                                                     ++route.media_chunks, {route.start + at}, pcm))
                .has_value();
        };
        std::size_t writes = 0;
        route.sink->during_write = [&] {
            // The first piece of the 18th parent, whose slot is 1,760 ms.
            if (++writes == 35) { clock = route.start + (1'760 + 50 + stall_ms) * kMs; }
        };
        for (int k = 0; k < 18; ++k) { ASSERT_TRUE(submit(0)); }
        ASSERT_TRUE(route.drain());
        EXPECT_EQ(route.stats().playout_retimes, 1U);
        // Second half: content 50 ms, placed at 1,830 ms + stall.
        const auto beyond = (1'760 + 50 + stall_ms + 20 - 50) * kMs - kBoundNs;
        const auto rest = std::min<std::int64_t>(beyond, 50 * kMs);
        EXPECT_GE(static_cast<double>(route.stats().frames_overflowed) * kFrameNs,
                  static_cast<double>(rest) - 1e5);
        EXPECT_LE(static_cast<double>(route.stats().frames_overflowed) * kFrameNs,
                  static_cast<double>(rest) + 1e5);
        // The stream goes on where the cut left it.
        route.sink->during_write = {};
        clock = std::max(clock, route.start + 1'900 * kMs);
        ASSERT_TRUE(submit(1'900 * kMs));
        ASSERT_TRUE(route.drain());
        EXPECT_EQ(route.stats().playout_underruns, 0U);
        EXPECT_LE(route.stats().playout_depth.count, kBoundNs);
        expect_only_overflow(route);
    }
}

// Third review of #229, P2: a stream's first run queues behind the stream before
// it, so stopping after every chunk must not escape the bound. 07b82f0 cut 64,879
// frames of 80 chunks 1 ms apart in one stream, and none with a stop between any
// two: the last one was placed 3.4 s after its arrival.
TEST_F(AudioPipelineRoute, WiredDefaultsBoundFreshStreamsLikeOneStream) {
    const auto media = aa::protocol::ChannelRole::media_audio;
    std::array<std::uint64_t, 2> cut{};
    for (const bool stops : {false, true}) {
        SCOPED_TRACE(stops);
        WiredRoute route;
        ASSERT_TRUE(route.router);
        for (int k = 0; k < 80; ++k) {
            ASSERT_TRUE(route.submit_media(k * kMs));
            if (stops) { EXPECT_TRUE(route.router->end_stream(media)); }
        }
        ASSERT_TRUE(route.drain());
        cut[stops ? 1 : 0] = route.stats().frames_overflowed;
        EXPECT_GT(route.stats().frames_overflowed, 60'000U);
        // The last write ends 2 s after the last chunk's end at the latest.
        const auto last = route.writes.back().scheduled_end.count - route.start - 79 * kMs;
        EXPECT_LE(last, kBoundNs + kA26ChunkNs + 100'000);
        EXPECT_LE(route.stats().playout_offset.count, kBoundNs + 21'000);
        expect_only_overflow(route);
    }
    EXPECT_NEAR(static_cast<double>(cut[0]), static_cast<double>(cut[1]), 80.0); // a frame each
}

// Bot review of #229, P2: what follows a wholly cut chunk is still planned, played
// and accounted for, through pump() and flush(). 07b82f0 took the next chunks in
// the same step, before they had a playout decision: their cut was no overflow
// and what was played had no offset.
TEST_F(AudioPipelineRoute, WiredDefaultsKeepGoingAfterWhollyCutChunks) {
    for (const bool flush : {false, true}) {
        SCOPED_TRACE(flush);
        WiredRoute route;
        ASSERT_TRUE(route.router);
        ASSERT_TRUE(route.media(0)); // the incremental path
        for (int k = 0; k < 60; ++k) { ASSERT_TRUE(route.submit_media(0)); }
        ASSERT_TRUE(route.submit_media(600 * kMs)); // 1.4 s deep: kept
        ASSERT_TRUE(route.submit_media(1'700 * kMs));
        ASSERT_TRUE(flush ? route.router->flush() : route.router->pump());
        EXPECT_EQ(route.stats().frames_decoded, 63U * kA26Frames);
        EXPECT_GT(route.stats().frames_overflowed, 13U * kA26Frames);
        expect_only_overflow(route);
        // Both later chunks are whole, and reported at the depth they play with.
        ASSERT_GE(route.writes.size(), 2U);
        const auto& last = route.writes.back();
        EXPECT_EQ(route.stats().playout_depth.count, last.scheduled.count - route.start - 1'700 * kMs);
        EXPECT_NEAR(static_cast<double>(last.scheduled_end.count - route.start),
                    static_cast<double>(kBoundNs + 3 * kA26ChunkNs), 1e5);
        EXPECT_GT(route.stats().playout_offset.count, 1'400 * kMs);
    }
}

TEST_F(AudioPipelineRoute, RateCorrectionKernelIsTransparent) {
    const auto samples_of = [](const std::vector<std::byte>& pcm) {
        std::vector<std::int16_t> out(pcm.size() / 4);
        for (std::size_t i = 0; i < out.size(); ++i) { std::memcpy(&out[i], pcm.data() + i * 4, 2); }
        return out;
    };
    // Unity, too short a run, or too large a change: untouched.
    const auto tone = tone_pcm(0, kA26Frames);
    EXPECT_EQ(aa::audio::detail::warp_frames(tone, 2, 0), tone);
    EXPECT_EQ(aa::audio::detail::warp_frames(tone, 2, 22), tone);
    EXPECT_EQ(aa::audio::detail::warp_frames(tone, 2, -22), tone);
    const auto short_run = tone_pcm(0, 15);
    EXPECT_EQ(aa::audio::detail::warp_frames(short_run, 2, -1), short_run);
    struct Case {
        std::uint64_t frames;
        std::int64_t delta;
        std::size_t edge;
        double tolerance;
    };
    for (const Case c : {Case{kA26Frames, -21, 32, 2.0}, Case{kA26Frames, -10, 32, 2.0},
                         Case{kA26Frames, -1, 32, 2.0}, Case{kA26Frames, 1, 32, 2.0},
                         Case{kA26Frames, 21, 32, 2.0}, Case{256, -3, 32, 2.0}, Case{240, -3, 16, 2.0},
                         Case{240, 1, 16, 2.0}, Case{64, -1, 16, 2.0}, Case{64, 1, 16, 2.0},
                         Case{63, -1, 4, 12.0}, Case{48, 1, 4, 12.0}, Case{16, -1, 4, 12.0},
                         Case{16, 1, 4, 12.0}}) {
        SCOPED_TRACE(std::to_string(c.frames) + (c.delta < 0 ? " " : " +") + std::to_string(c.delta));
        const auto run = tone_pcm(0, c.frames);
        const auto constant = samples_of(aa::audio::detail::warp_frames(
            constant_pcm({12345, c.frames}), 2, c.delta));
        EXPECT_EQ(constant.size(), c.frames + static_cast<std::uint64_t>(c.delta));
        EXPECT_TRUE(std::all_of(constant.begin(), constant.end(), [](auto v) { return v == 12345; }));
        const auto warped = samples_of(aa::audio::detail::warp_frames(run, 2, c.delta));
        const auto original = samples_of(run);
        ASSERT_EQ(warped.size(), constant.size());
        const auto edge = static_cast<std::ptrdiff_t>(c.edge);
        // Both ends are bit-exact, so neighbouring runs join without a step.
        EXPECT_TRUE(std::equal(original.begin(), original.begin() + edge, warped.begin()));
        EXPECT_TRUE(std::equal(original.end() - edge, original.end(), warped.end() - edge));
        // Between them it is the tone at one constant rate, within rounding.
        const double rate = static_cast<double>(c.frames - 2 * c.edge)
            / static_cast<double>(warped.size() - 2 * c.edge);
        double worst = 0.0;
        for (std::size_t i = c.edge; i < warped.size() - c.edge; ++i) {
            const double frame = static_cast<double>(c.edge) + static_cast<double>(i - c.edge) * rate;
            const double ideal = kToneAmplitude
                * std::sin(2.0 * 3.14159265358979323846 * kToneHz * frame / 48000.0);
            worst = std::max(worst, std::abs(ideal - static_cast<double>(warped[i])));
        }
        EXPECT_LT(worst, c.tolerance);
    }
}
