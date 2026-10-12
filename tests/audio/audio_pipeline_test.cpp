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

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <optional>
#include <signal.h>
#include <sys/stat.h>
#include <cerrno>
#include <cstring>
#include <array>
#include <sstream>
#include <spawn.h>
#include <string>
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

class AudioPipelineRoute : public ::testing::Test {
protected:
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
    // while the router stays open. The next write must surface the loss typed
    // and degrade the router with a recorded cause - never a silent success.
    ASSERT_TRUE(router.value().flush().has_value());
    EXPECT_GE(router.value().report().render_barrier.drains_completed, 1U);

    const auto second = make_message(aa::protocol::ChannelRole::media_audio, 1, now_ns(), media);
    ASSERT_TRUE(router.value().submit(second).has_value());
    auto pumped = router.value().pump();
    ASSERT_FALSE(pumped.has_value());
    EXPECT_EQ(pumped.error().code(), aa::ErrorCode::audio_sink_unavailable);

    EXPECT_EQ(router.value().state(), aa::audio::RouterState::degraded);
    const auto& cause = router.value().report().sink.cause;
    EXPECT_FALSE(cause.empty());
    EXPECT_NE(cause.find("disabled"), std::string::npos);

    const auto third = make_message(aa::protocol::ChannelRole::media_audio, 2, now_ns(), media);
    auto late_submit = router.value().submit(third);
    ASSERT_FALSE(late_submit.has_value());
    EXPECT_EQ(late_submit.error().code(), aa::ErrorCode::audio_sink_unavailable);
    router.value().close();
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
                    return {ConfigOutcome::Stage::denied_module_verify,
                            "retained forbidden module " + token + " in " + dest_path};
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
            if (!output.good()) {
                return {ConfigOutcome::Stage::write_close, "write failed " + dest_path};
            }
            return {};
        }
        std::ofstream output(dest_path);
        if (!output) {
            return {ConfigOutcome::Stage::destination_open, "cannot open " + dest_path};
        }
        output << kept;
        if (!output.good()) {
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
        GTEST_SKIP() << "pipewire/wireplumber unavailable on this host";
    }
    auto daemon = std::move(*started.backend);

    auto config = pcm_router_config();
    config.backend = aa::audio::SinkBackend::pipewire;
    auto router = aa::audio::AudioRouter::open(config);
    ASSERT_TRUE(router.has_value());
    ASSERT_EQ(router.value().state(), aa::audio::RouterState::active);

    const std::vector<std::byte> media = constant_pcm({kMediaLevel, 960});
    const auto first = make_message(aa::protocol::ChannelRole::media_audio, 0, now_ns(), media);
    ASSERT_TRUE(router.value().submit(first).has_value());
    ASSERT_TRUE(router.value().pump().has_value());
    EXPECT_GE(router.value().report().render_barrier.handoff_events, 1U);

    daemon.kill_backend();
    usleep(1000000);

    const auto second = make_message(aa::protocol::ChannelRole::media_audio, 1, now_ns(), media);
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
