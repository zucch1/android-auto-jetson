// SPDX-License-Identifier: GPL-3.0-or-later
// Task 20 boundary followup: typed enum/text/ID values must be validated at
// every public boundary. A successful admission or export can never emit
// unknown text, normalize an invalid direction/provenance, or produce JSON the
// loader refuses; expectation text and roles stay symmetric with the loader.

#include "fixtures.hpp"

#include <aa/replay/Player.hpp>
#include <aa/replay/Schema.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <gtest/gtest.h>

namespace {
using namespace replaytest;
namespace replay = aa::replay;

std::vector<std::byte> golden_file() {
    replay::Recorder recorder;
    fill_golden(recorder);
    auto exported = recorder.export_fixture();
    EXPECT_TRUE(exported);
    return exported.value().file_bytes;
}

// The sealed golden fixture for typed (not re-encoded) boundary edits.
replay::Fixture typed_golden() {
    auto loaded = replay::load_fixture(golden_file());
    EXPECT_TRUE(loaded);
    return loaded.value();
}

void expect_error(const aa::core::Result<void>& result, aa::ErrorCode code) {
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code(), code);
}

// Runs a typed fixture and returns the player so the partial trace can be
// asserted: validation rejection leaves the observed trace untouched.
struct TypedRun final {
    std::optional<aa::Error> failure;
    replay::ReplayPlayer& player;
};

TEST(ReplayTyped, UnknownSessionEventRejectedNotEmitted) {
    // Given: a typed session record holding an out-of-range lifecycle event.
    auto fixture = typed_golden();
    auto& step = std::get<replay::SessionRecord>(fixture.records[0].body);
    step.event = static_cast<aa::session::Event>(42);
    // When: the export gate seals it.
    const auto exported = replay::export_fixture(fixture);
    // Then: export refuses instead of emitting "event":"unknown" text.
    ASSERT_FALSE(exported);
    EXPECT_EQ(exported.error().code(), aa::ErrorCode::invalid_argument);
}

TEST(ReplayTyped, UnknownSessionEventRejectedAtRecorder) {
    // Given: an empty recorder.
    replay::Recorder recorder;
    // When: the typed event value is out of range.
    const auto refused = recorder.record_session(
        aa::core::Nanoseconds{0},
        replay::SessionRecord{replay::SessionOp::event, static_cast<aa::session::Event>(42)});
    // Then: admission refuses rather than poisoning export.
    expect_error(refused, aa::ErrorCode::invalid_argument);
}

TEST(ReplayTyped, UnknownSessionEventRejectedAtRun) {
    // Given: the typed fixture handed straight to the player.
    auto fixture = typed_golden();
    auto& step = std::get<replay::SessionRecord>(fixture.records[0].body);
    step.event = static_cast<aa::session::Event>(42);
    MapTrustStore trust;
    replay::ReplayPlayer player(fixture, replay::ReplayPlayer::Deps{trust});
    RecordingSink video_sink;
    RecordingSink input_sink;
    AudioCapture audio;
    ASSERT_TRUE(player.register_sink(proto::ChannelRole::video, video_sink));
    ASSERT_TRUE(player.register_sink(proto::ChannelRole::input, input_sink));
    ASSERT_TRUE(player.register_audio_sink(audio));
    // When: the player runs it.
    auto observed = player.run();
    // Then: validation rejects before playback starts.
    ASSERT_FALSE(observed);
    EXPECT_EQ(observed.error().code(), aa::ErrorCode::invalid_argument);
    EXPECT_TRUE(player.observed().states.empty());
}

TEST(ReplayTyped, InvalidDirectionRejectedNotNormalized) {
    // Given: a typed transport record holding an out-of-range direction.
    auto fixture = typed_golden();
    auto& body = std::get<replay::TransportRecord>(fixture.records[3].body);
    body.dir = static_cast<replay::Direction>(9);
    // When: the export gate seals it.
    const auto exported = replay::export_fixture(fixture);
    // Then: export refuses instead of silently normalizing to "out".
    ASSERT_FALSE(exported);
    EXPECT_EQ(exported.error().code(), aa::ErrorCode::invalid_argument);
}

TEST(ReplayTyped, InvalidDirectionRejectedAtRecorder) {
    // Given: an empty recorder and a well-formed frame.
    replay::Recorder recorder;
    // When: the direction enum value is out of range.
    const auto refused = recorder.record_transport(
        aa::core::Nanoseconds{0}, static_cast<replay::Direction>(9), discovery_request_frame());
    // Then: admission refuses.
    expect_error(refused, aa::ErrorCode::invalid_argument);
}

TEST(ReplayTyped, InvalidDirectionRejectedAtRun) {
    // Given: a typed transport record whose direction is out of range.
    auto fixture = typed_golden();
    auto& body = std::get<replay::TransportRecord>(fixture.records[3].body);
    body.dir = static_cast<replay::Direction>(9);
    MapTrustStore trust;
    replay::ReplayPlayer player(fixture, replay::ReplayPlayer::Deps{trust});
    RecordingSink video_sink;
    RecordingSink input_sink;
    AudioCapture audio;
    ASSERT_TRUE(player.register_sink(proto::ChannelRole::video, video_sink));
    ASSERT_TRUE(player.register_sink(proto::ChannelRole::input, input_sink));
    ASSERT_TRUE(player.register_audio_sink(audio));
    // When: the player runs it.
    auto observed = player.run();
    // Then: validation rejects instead of routing the frame as inbound.
    ASSERT_FALSE(observed);
    EXPECT_EQ(observed.error().code(), aa::ErrorCode::invalid_argument);
    EXPECT_TRUE(player.observed().states.empty());
}

TEST(ReplayTyped, InvalidAudioRoleRejectedNotEmitted) {
    // Given: a typed audio record holding an out-of-range role.
    auto fixture = typed_golden();
    auto& body = std::get<replay::AudioRecord>(fixture.records[11].body);
    body.role = static_cast<aa::audio::Role>(9);
    // When: the export gate seals it.
    const auto exported = replay::export_fixture(fixture);
    // Then: export refuses instead of emitting "role":"unknown".
    ASSERT_FALSE(exported);
    EXPECT_EQ(exported.error().code(), aa::ErrorCode::invalid_argument);
}

TEST(ReplayTyped, InvalidAudioRoleRejectedAtRecorder) {
    // Given: an empty recorder and a valid PCM chunk.
    replay::Recorder recorder;
    // When: the role enum value is out of range.
    const auto refused = recorder.record_audio(
        aa::core::Nanoseconds{0},
        replay::AudioRecord{static_cast<aa::audio::Role>(9),
                            aa::audio::AudioFormat{48000, 2, 16}, aa::core::Nanoseconds{0},
                            kPcmOne});
    // Then: admission refuses.
    expect_error(refused, aa::ErrorCode::invalid_argument);
}

TEST(ReplayTyped, InvalidAudioRoleRejectedAtRun) {
    // Given: typed audio record and expectation agreeing on a garbage role.
    auto fixture = typed_golden();
    auto& body = std::get<replay::AudioRecord>(fixture.records[11].body);
    body.role = static_cast<aa::audio::Role>(9);
    fixture.expect.audio[0].role = static_cast<aa::audio::Role>(9);
    MapTrustStore trust;
    replay::ReplayPlayer player(fixture, replay::ReplayPlayer::Deps{trust});
    RecordingSink video_sink;
    RecordingSink input_sink;
    AudioCapture audio;
    ASSERT_TRUE(player.register_sink(proto::ChannelRole::video, video_sink));
    ASSERT_TRUE(player.register_sink(proto::ChannelRole::input, input_sink));
    ASSERT_TRUE(player.register_audio_sink(audio));
    // When: the player runs it.
    auto observed = player.run();
    // Then: validation rejects instead of writing garbage to the sink.
    ASSERT_FALSE(observed);
    EXPECT_EQ(observed.error().code(), aa::ErrorCode::invalid_argument);
    EXPECT_TRUE(audio.chunks.empty());
    EXPECT_TRUE(player.observed().states.empty());
}

TEST(ReplayTyped, InvalidProvenanceRejectedNotNormalized) {
    // Given: a typed fixture whose provenance enum is out of range.
    auto fixture = typed_golden();
    fixture.provenance = static_cast<replay::Provenance>(5);
    // When: the export gate seals it.
    const auto exported = replay::export_fixture(fixture);
    // Then: export refuses instead of normalizing to "synthetic".
    ASSERT_FALSE(exported);
    EXPECT_EQ(exported.error().code(), aa::ErrorCode::invalid_argument);
}

TEST(ReplayTyped, InvalidUtf8PhoneKeyRejectedAtRecorderAndExport) {
    // Given: a synthetic phone key carrying an invalid UTF-8 byte.
    const std::string bad_key = std::string("phone-") + static_cast<char>(0xFF);
    replay::Recorder recorder;
    const auto refused = recorder.record_session(
        aa::core::Nanoseconds{0},
        replay::SessionRecord{replay::SessionOp::phone_discovered, {}, bad_key});
    expect_error(refused, aa::ErrorCode::invalid_argument);
    // When: the same key reaches the export gate through a typed fixture.
    auto fixture = typed_golden();
    auto& step = std::get<replay::SessionRecord>(fixture.records[1].body);
    step.phone_key = bad_key;
    const auto exported = replay::export_fixture(fixture);
    // Then: export refuses too - successful export stays loadable JSON.
    ASSERT_FALSE(exported);
    EXPECT_EQ(exported.error().code(), aa::ErrorCode::invalid_argument);
}

TEST(ReplayTyped, WireIdOverflowRejectedAtRecorder) {
    // Given: an empty recorder.
    replay::Recorder recorder;
    const auto overflow = std::uint64_t{1} << 63U;
    // When: video and input wire ids exceed the signed encoding bound.
    const auto refused = recorder.record_video(
        aa::core::Nanoseconds{0},
        replay::VideoRecord{overflow, 1, aa::core::Nanoseconds{0}, true, kVideoOne});
    expect_error(refused, aa::ErrorCode::invalid_argument);
    const auto refused_input = recorder.record_input(
        aa::core::Nanoseconds{0},
        replay::InputRecord{overflow, aa::core::Nanoseconds{0}, kInputOne});
    // Then: admission refuses instead of poisoning export.
    expect_error(refused_input, aa::ErrorCode::invalid_argument);
}

TEST(ReplayTyped, ExpectationStateTextBounded) {
    // Given: an empty recorder.
    replay::Recorder recorder;
    replay::Expectation huge;
    huge.states = {"disconnected", std::string(4000, 'x')};
    // When: an expected state name is an unbounded string.
    const auto refused = recorder.set_expectation(huge);
    // Then: admission refuses instead of storing giant text.
    expect_error(refused, aa::ErrorCode::invalid_argument);
}

TEST(ReplayTyped, ExpectationUnknownStateRejected) {
    // Given: an empty recorder.
    replay::Recorder recorder;
    replay::Expectation bogus;
    bogus.states = {"disconnected", "bogus"};
    // When: an expected state name is outside the accepted vocabulary.
    const auto refused = recorder.set_expectation(bogus);
    // Then: admission refuses instead of poisoning export.
    expect_error(refused, aa::ErrorCode::invalid_argument);
}

TEST(ReplayTyped, ExpectationRolesMustMatchServices) {
    // Given: video-only support.
    replay::Recorder recorder;
    ASSERT_TRUE(recorder.set_services({
        replay::ServiceEntry{proto::ServiceKey{99}, proto::ChannelRole::video,
                             proto::VideoConfiguration{{{1280, 720, 30}}}},
    }));
    replay::Expectation expect;
    expect.states = {"disconnected"};
    expect.deliveries = {
        {proto::ChannelRole::input, 1, aa::core::Nanoseconds{0}, kInputOne},
    };
    // When: the expectation names a role the services do not carry.
    const auto refused = recorder.set_expectation(expect);
    // Then: admission refuses, symmetric with the loader's role check.
    expect_error(refused, aa::ErrorCode::invalid_argument);
}

TEST(ReplayTyped, SetServicesRechecksExpectationRoles) {
    // Given: a recorded expectation valid for the current services.
    replay::Recorder recorder;
    replay::Expectation expect;
    expect.states = {"disconnected"};
    expect.deliveries = {
        {proto::ChannelRole::video, 1, aa::core::Nanoseconds{0}, kVideoOne},
    };
    ASSERT_TRUE(recorder.set_services({
        replay::ServiceEntry{proto::ServiceKey{99}, proto::ChannelRole::video,
                             proto::VideoConfiguration{{{1280, 720, 30}}}},
    }));
    ASSERT_TRUE(recorder.set_expectation(expect));
    // When: the services shrink away from the recorded expectation.
    const auto refused = recorder.set_services({
        replay::ServiceEntry{proto::ServiceKey{101}, proto::ChannelRole::input,
                             proto::ButtonConfiguration{{85}}},
    });
    // Then: the boundary refuses the inconsistent pair.
    expect_error(refused, aa::ErrorCode::invalid_argument);
}

} // namespace
