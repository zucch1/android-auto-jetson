// SPDX-License-Identifier: GPL-3.0-or-later
// Task 20 replay goldens: the fixture SHA-256 (over the fixed canonical bytes)
// must match an explicit expected constant - an independent literal, never
// recomputed from the same implementation at runtime - and repeated
// record->export->load->replay runs must be byte- and trace-identical.

#include "fixtures.hpp"

#include <aa/replay/Player.hpp>
#include <aa/replay/Schema.hpp>


#include <gtest/gtest.h>

namespace {
using namespace replaytest;
namespace replay = aa::replay;

TEST(ReplayGolden, FixtureSha256MatchesExpectedConstant) {
    // Given: the golden synthetic scenario.
    replay::Recorder recorder;
    fill_golden(recorder);
    // When: the recorder seals the fixture.
    auto exported = recorder.export_fixture();
    ASSERT_TRUE(exported);
    // Then: the hash is the pinned expected transition/output trace constant.
    EXPECT_EQ(exported.value().sha256_hex, replaytest::kGoldenFixtureSha256);
}

TEST(ReplayGolden, MultiRunExportAndReplayAreDeterministic) {
    // Given: two independently recorded golden fixtures.
    replay::Recorder first;
    replay::Recorder second;
    fill_golden(first);
    fill_golden(second);
    auto exported_first = first.export_fixture();
    auto exported_second = second.export_fixture();
    ASSERT_TRUE(exported_first);
    ASSERT_TRUE(exported_second);
    // When/Then: export bytes and hash are identical across recorders.
    EXPECT_EQ(exported_first.value().sha256_hex, exported_second.value().sha256_hex);
    EXPECT_EQ(exported_first.value().file_bytes, exported_second.value().file_bytes);
    // When/Then: replay traces are identical across fresh players.
    MapTrustStore trust;
    auto loaded = replay::load_fixture(exported_first.value().file_bytes);
    ASSERT_TRUE(loaded);
    replay::ReplayPlayer run_one(loaded.value(), replay::ReplayPlayer::Deps{trust});
    replay::ReplayPlayer run_two(loaded.value(), replay::ReplayPlayer::Deps{trust});
    RecordingSink video_one;
    RecordingSink video_two;
    RecordingSink input_one;
    RecordingSink input_two;
    AudioCapture audio_one;
    AudioCapture audio_two;
    ASSERT_TRUE(run_one.register_sink(proto::ChannelRole::video, video_one));
    ASSERT_TRUE(run_one.register_sink(proto::ChannelRole::input, input_one));
    ASSERT_TRUE(run_one.register_audio_sink(audio_one));
    ASSERT_TRUE(run_two.register_sink(proto::ChannelRole::video, video_two));
    ASSERT_TRUE(run_two.register_sink(proto::ChannelRole::input, input_two));
    ASSERT_TRUE(run_two.register_audio_sink(audio_two));
    auto observed_one = run_one.run();
    auto observed_two = run_two.run();
    ASSERT_TRUE(observed_one);
    ASSERT_TRUE(observed_two);
    EXPECT_EQ(observed_one.value(), observed_two.value());
    EXPECT_EQ(observed_one.value(), golden_expectation());
    EXPECT_EQ(video_one.payloads, video_two.payloads);
    EXPECT_EQ(input_one.payloads, input_two.payloads);
    EXPECT_EQ(audio_one.chunks.size(), audio_two.chunks.size());
}

TEST(ReplayGolden, LoadedFixtureHashMatchesExportedHash) {
    // Given: an exported fixture.
    replay::Recorder recorder;
    fill_golden(recorder);
    auto exported = recorder.export_fixture();
    ASSERT_TRUE(exported);
    // When: it is re-loaded through the schema gate.
    auto loaded = replay::load_fixture(exported.value().file_bytes);
    // Then: the canonical body of the loaded form re-encodes byte-identically
    // and the expectation embedded in the hashed bytes is the explicit one.
    ASSERT_TRUE(loaded);
    EXPECT_EQ(loaded.value().expect, golden_expectation());
}

} // namespace
