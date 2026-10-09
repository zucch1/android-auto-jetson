// SPDX-License-Identifier: GPL-3.0-or-later
// Task 20 followup (defect 3): a successful replay must reproduce the exact
// expected transition/output trace. Wrong-but-legal traces are re-sealed with
// a correct hash and rejected by the real player, never by the checksum.

#include "fixtures.hpp"

#include <aa/replay/Player.hpp>
#include <aa/replay/Schema.hpp>

#include <optional>
#include <vector>

#include <gtest/gtest.h>

namespace {
using namespace replaytest;
namespace replay = aa::replay;

// Loads the sealed golden fixture, then re-exports it with one expectation
// edit. The free export gate re-seals the hash, so only the player can notice.
std::vector<std::byte> golden_with(replay::Expectation (*edit)(replay::Expectation)) {
    replay::Recorder recorder;
    fill_golden(recorder);
    auto exported = recorder.export_fixture();
    EXPECT_TRUE(exported);
    auto loaded = replay::load_fixture(exported.value().file_bytes);
    EXPECT_TRUE(loaded);
    replay::Fixture mutated = loaded.value();
    mutated.expect = edit(mutated.expect);
    auto resealed = replay::export_fixture(mutated);
    EXPECT_TRUE(resealed);
    return resealed.value().file_bytes;
}

std::vector<std::byte> export_or_fail(replay::Recorder& recorder) {
    auto exported = recorder.export_fixture();
    EXPECT_TRUE(exported);
    return exported.value().file_bytes;
}

std::optional<aa::Error> run_failure(const std::vector<std::byte>& file) {
    auto loaded = replay::load_fixture(file);
    if (!loaded) {
        return loaded.error();
    }
    MapTrustStore trust;
    replay::ReplayPlayer player(loaded.value(), replay::ReplayPlayer::Deps{trust});
    RecordingSink video_sink;
    RecordingSink input_sink;
    AudioCapture audio;
    (void)player.register_sink(proto::ChannelRole::video, video_sink);
    (void)player.register_sink(proto::ChannelRole::input, input_sink);
    (void)player.register_audio_sink(audio);
    auto observed = player.run();
    if (observed) {
        return std::nullopt;
    }
    return observed.error();
}

replay::Expectation drop_active(replay::Expectation expect) {
    expect.states.pop_back();
    return expect;
}

replay::Expectation wrong_payload(replay::Expectation expect) {
    expect.deliveries[0].payload = kVideoTwo;
    return expect;
}

replay::Expectation wrong_sequence(replay::Expectation expect) {
    expect.deliveries[0].sequence = 7;
    return expect;
}

replay::Expectation wrong_role(replay::Expectation expect) {
    expect.deliveries[0].role = proto::ChannelRole::input;
    return expect;
}

TEST(ReplayVerdict, LegalExpectedStatesMissingActiveRejected) {
    // Given: the golden script re-sealed with the active state dropped (still legal).
    const auto file = golden_with(&drop_active);
    // When: the real player runs it.
    const auto failure = run_failure(file);
    // Then: the output-trace mismatch is a typed rejection.
    ASSERT_TRUE(failure.has_value());
    EXPECT_EQ(failure->code(), aa::ErrorCode::invalid_argument);
}

TEST(ReplayVerdict, WrongExpectedPayloadRejected) {
    // Given: the golden script re-sealed with a different valid payload expectation.
    const auto file = golden_with(&wrong_payload);
    // When/Then: the player rejects the payload mismatch.
    const auto failure = run_failure(file);
    ASSERT_TRUE(failure.has_value());
    EXPECT_EQ(failure->code(), aa::ErrorCode::invalid_argument);
}

TEST(ReplayVerdict, WrongExpectedSequenceRejected) {
    // Given: the golden script re-sealed with a wrong delivery sequence.
    const auto file = golden_with(&wrong_sequence);
    // When/Then: the player rejects the sequence mismatch.
    const auto failure = run_failure(file);
    ASSERT_TRUE(failure.has_value());
    EXPECT_EQ(failure->code(), aa::ErrorCode::invalid_argument);
}

TEST(ReplayVerdict, WrongExpectedRoleRejected) {
    // Given: the golden script re-sealed with a service-valid but wrong role.
    const auto file = golden_with(&wrong_role);
    // When/Then: the player rejects the role mismatch.
    const auto failure = run_failure(file);
    ASSERT_TRUE(failure.has_value());
    EXPECT_EQ(failure->code(), aa::ErrorCode::invalid_argument);
}

TEST(ReplayVerdict, MatchingExpectationAccepted) {
    // Given: the untouched golden script.
    replay::Recorder recorder;
    fill_golden(recorder);
    // When: the real player runs the re-sealed fixture.
    // Then: the run succeeds and the trace equals the explicit expectation.
    const auto failure = run_failure(export_or_fail(recorder));
    EXPECT_FALSE(failure.has_value());
}

} // namespace
