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
#include <aa/audio/Microphone.hpp>
#include <aa/protocol/Protocol.hpp>

#include "../../src/audio/AudioSinkFactory.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <iterator>
#include <mutex>
#include <optional>
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
    request.format = stereo_48k();
    request.start_timeout = aa::core::Milliseconds{2000};
    request.write_timeout = aa::core::Milliseconds{200};
    auto outcome = aa::audio::detail::open_default_sink(request);
    ASSERT_TRUE(outcome.sink != nullptr);

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

TEST_F(AudioPipelineRoute, MissingDefaultOutputDegradesTypedWithRecordedCause) {
    auto config = pcm_router_config();
    config.backend = aa::audio::SinkBackend::pipewire;
    config.sink_remote = "aa-task25-no-such-output";
    auto router = aa::audio::AudioRouter::open(config);
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
    auto router = aa::audio::AudioRouter::open(config);
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
    auto router = aa::audio::AudioRouter::open(config);
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
    auto lost = aa::audio::AudioRouter::open(config);
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

TEST(AudioPipelineSinkBackend, FullAppsrcQueueHonorsSingleWriteDeadline) {
    // Gate fix 3 regression: with the appsrc queue prefilled to its byte
    // threshold and downstream blocked after readiness, a real write() must
    // fail at ADMISSION within ONE deadline (~200 ms). It must not hang in the
    // enqueue (the old blocking appsrc push never returned), must not push
    // into the full queue (queue levels stay exactly as prefilled), and must
    // not wait for a handoff that cannot come (no acknowledged handoffs).
    aa::audio::detail::SinkOpenRequest request;
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
    ASSERT_TRUE(outcome.sink != nullptr);

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
};

class RecordingAudioSink final : public aa::audio::detail::ObservedAudioSink {
public:
    std::shared_ptr<RecordingSinkState> state{std::make_shared<RecordingSinkState>()};

    aa::core::Result<void> write(const aa::audio::MixedChunk& chunk) override {
        if (state->during_write) { state->during_write(); }
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
