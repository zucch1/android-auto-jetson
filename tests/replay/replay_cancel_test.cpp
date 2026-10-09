// SPDX-License-Identifier: GPL-3.0-or-later
// Task 20 followup (defect 4): no delivery after cancellation - the typed
// video, input and audio steps observe the bound stop token and refuse work.

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

std::vector<std::byte> export_or_fail(replay::Recorder& recorder) {
    auto exported = recorder.export_fixture();
    EXPECT_TRUE(exported);
    return exported.value().file_bytes;
}

struct CancelOutcome final {
    std::optional<aa::Error> failure;
    RecordingSink video;
    RecordingSink input;
    AudioCapture audio;
};

void run_cancelled(const std::vector<std::byte>& file, CancelOutcome& outcome) {
    auto loaded = replay::load_fixture(file);
    if (!loaded) {
        outcome.failure = loaded.error();
        return;
    }
    MapTrustStore trust;
    replay::ReplayPlayer player(loaded.value(), replay::ReplayPlayer::Deps{trust});
    (void)player.register_sink(proto::ChannelRole::video, outcome.video);
    (void)player.register_sink(proto::ChannelRole::input, outcome.input);
    (void)player.register_audio_sink(outcome.audio);
    auto observed = player.run();
    if (!observed) {
        outcome.failure = observed.error();
    }
}

replay::Expectation expectation_of(std::vector<std::string> states, std::int64_t sends) {
    replay::Expectation expect;
    expect.states = std::move(states);
    expect.sends = sends;
    return expect;
}

TEST(ReplayCancel, NoVideoDeliveryAfterCancel) {
    // Given: a connected script with an open video route, stopped before video.
    replay::Recorder recorder;
    const auto services = golden_services();
    using aa::core::Nanoseconds;
    using replay::Direction;
    using replay::SessionOp;
    using replay::SessionRecord;
    ASSERT_TRUE(recorder.set_services(services));
    ASSERT_TRUE(recorder.set_expectation(
        expectation_of({"disconnected", "discovering", "connecting"}, 2)));
    ASSERT_TRUE(recorder.record_session(
        Nanoseconds{0}, SessionRecord{SessionOp::event, aa::session::Event::start_discovery}));
    ASSERT_TRUE(recorder.record_session(
        Nanoseconds{1'000'000}, SessionRecord{SessionOp::phone_discovered, {}, kApproved.key}));
    ASSERT_TRUE(recorder.record_transport(Nanoseconds{2'000'000}, Direction::inbound,
                                          discovery_request_frame()));
    ASSERT_TRUE(recorder.record_transport(Nanoseconds{2'500'000}, Direction::outbound,
                                          discovery_response_frame(services)));
    ASSERT_TRUE(recorder.record_transport(Nanoseconds{3'000'000}, Direction::inbound,
                                          open_request_frame(99)));
    ASSERT_TRUE(recorder.record_transport(Nanoseconds{3'500'000}, Direction::outbound,
                                          open_response_frame()));
    ASSERT_TRUE(recorder.record_session(Nanoseconds{4'000'000},
                                        SessionRecord{SessionOp::request_stop}));
    ASSERT_TRUE(recorder.record_video(
        Nanoseconds{5'000'000},
        replay::VideoRecord{1, 1, Nanoseconds{5'000'000}, true, kVideoOne}));
    // When: the video record runs after cancellation.
    CancelOutcome outcome;
    run_cancelled(export_or_fail(recorder), outcome);
    // Then: the typed cancel error surfaces and nothing is delivered.
    ASSERT_TRUE(outcome.failure.has_value());
    EXPECT_EQ(outcome.failure->code(), aa::ErrorCode::cancelled);
    EXPECT_TRUE(outcome.video.payloads.empty());
}

TEST(ReplayCancel, NoInputDeliveryAfterCancel) {
    // Given: a connected script with an open input route, stopped before input.
    replay::Recorder recorder;
    const auto services = golden_services();
    using aa::core::Nanoseconds;
    using replay::Direction;
    using replay::SessionOp;
    using replay::SessionRecord;
    ASSERT_TRUE(recorder.set_services(services));
    ASSERT_TRUE(recorder.set_expectation(
        expectation_of({"disconnected", "discovering", "connecting"}, 2)));
    ASSERT_TRUE(recorder.record_session(
        Nanoseconds{0}, SessionRecord{SessionOp::event, aa::session::Event::start_discovery}));
    ASSERT_TRUE(recorder.record_session(
        Nanoseconds{1'000'000}, SessionRecord{SessionOp::phone_discovered, {}, kApproved.key}));
    ASSERT_TRUE(recorder.record_transport(Nanoseconds{2'000'000}, Direction::inbound,
                                          discovery_request_frame()));
    ASSERT_TRUE(recorder.record_transport(Nanoseconds{2'500'000}, Direction::outbound,
                                          discovery_response_frame(services)));
    ASSERT_TRUE(recorder.record_transport(Nanoseconds{3'000'000}, Direction::inbound,
                                          open_request_frame(101)));
    ASSERT_TRUE(recorder.record_transport(Nanoseconds{3'500'000}, Direction::outbound,
                                          open_response_frame()));
    ASSERT_TRUE(recorder.record_session(Nanoseconds{4'000'000},
                                        SessionRecord{SessionOp::request_stop}));
    ASSERT_TRUE(recorder.record_input(
        Nanoseconds{5'000'000}, replay::InputRecord{1, Nanoseconds{5'000'000}, kInputOne}));
    // When: the input record runs after cancellation.
    CancelOutcome outcome;
    run_cancelled(export_or_fail(recorder), outcome);
    // Then: the typed cancel error surfaces and nothing is delivered.
    ASSERT_TRUE(outcome.failure.has_value());
    EXPECT_EQ(outcome.failure->code(), aa::ErrorCode::cancelled);
    EXPECT_TRUE(outcome.input.payloads.empty());
}

TEST(ReplayCancel, NoAudioDeliveryAfterCancel) {
    // Given: a script whose stop token fires before an audio record.
    replay::Recorder recorder;
    using aa::core::Nanoseconds;
    using replay::SessionOp;
    using replay::SessionRecord;
    ASSERT_TRUE(recorder.set_expectation(
        expectation_of({"disconnected", "discovering", "connecting"}, 0)));
    ASSERT_TRUE(recorder.record_session(
        Nanoseconds{0}, SessionRecord{SessionOp::event, aa::session::Event::start_discovery}));
    ASSERT_TRUE(recorder.record_session(
        Nanoseconds{1'000'000}, SessionRecord{SessionOp::phone_discovered, {}, kApproved.key}));
    ASSERT_TRUE(recorder.record_session(Nanoseconds{2'000'000},
                                        SessionRecord{SessionOp::request_stop}));
    ASSERT_TRUE(recorder.record_audio(
        Nanoseconds{3'000'000},
        replay::AudioRecord{aa::audio::Role::media, aa::audio::AudioFormat{48000, 2, 16},
                            Nanoseconds{3'000'000}, kPcmOne}));
    // When: the audio record runs after cancellation.
    CancelOutcome outcome;
    run_cancelled(export_or_fail(recorder), outcome);
    // Then: the typed cancel error surfaces and no PCM reaches the sink.
    ASSERT_TRUE(outcome.failure.has_value());
    EXPECT_EQ(outcome.failure->code(), aa::ErrorCode::cancelled);
    EXPECT_TRUE(outcome.audio.chunks.empty());
}

} // namespace
