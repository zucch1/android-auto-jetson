// SPDX-License-Identifier: GPL-3.0-or-later
// Task 20 replay trace: record -> export -> load -> replay on the real
// accepted core (task-17 lifecycle, real frame parser stack, task-18
// negotiated routing, pinned protocol encoders, typed audio sink) with exact
// states/payload outcomes.

#include "fixtures.hpp"

#include <aa/replay/Player.hpp>
#include <aa/replay/Schema.hpp>

#include <cstddef>
#include <vector>

#include <gtest/gtest.h>

namespace {
using namespace replaytest;
namespace replay = aa::replay;

replay::Recorder& fill_session(replay::Recorder& recorder, const replay::Expectation& expect) {
    (void)recorder.set_expectation(expect);
    return recorder;
}

std::vector<std::byte> export_or_fail(replay::Recorder& recorder) {
    auto exported = recorder.export_fixture();
    EXPECT_TRUE(exported);
    return exported.value().file_bytes;
}

TEST(ReplayTrace, RecordExportLoadReplayFiveKindsExactOutcomes) {
    // Given: the golden synthetic scenario through the bounded recorder.
    replay::Recorder recorder;
    fill_golden(recorder);
    // When: export -> load -> replay against the real core.
    const auto file = export_or_fail(recorder);
    auto loaded = replay::load_fixture(file);
    ASSERT_TRUE(loaded);
    EXPECT_EQ(loaded.value().expect, golden_expectation());
    MapTrustStore trust;
    replay::ReplayPlayer player(loaded.value(), replay::ReplayPlayer::Deps{trust});
    RecordingSink video_sink;
    RecordingSink input_sink;
    AudioCapture audio;
    ASSERT_TRUE(player.register_sink(proto::ChannelRole::video, video_sink));
    ASSERT_TRUE(player.register_sink(proto::ChannelRole::input, input_sink));
    ASSERT_TRUE(player.register_audio_sink(audio));
    auto observed = player.run();
    // Then: exact states, exact payload outcomes and every expected send.
    ASSERT_TRUE(observed);
    EXPECT_EQ(observed.value(), golden_expectation());
    EXPECT_EQ(player.state(), aa::session::State::active);
    ASSERT_EQ(video_sink.payloads.size(), 2u);
    EXPECT_EQ(video_sink.payloads[0], kVideoOne);
    EXPECT_EQ(video_sink.payloads[1], kVideoTwo);
    ASSERT_EQ(input_sink.payloads.size(), 1u);
    EXPECT_EQ(input_sink.payloads[0], kInputOne);
    ASSERT_EQ(audio.chunks.size(), 1u);
    EXPECT_EQ(audio.chunks[0].pcm, kPcmOne);
    EXPECT_EQ(audio.flushes, 1);
}

TEST(ReplayTrace, TeardownResetClearsRoutingUntilReopened) {
    // Given: an open video route that is torn down with reset_channels.
    replay::Recorder recorder;
    replay::Expectation expect;
    expect.states = {"disconnected", "discovering", "connecting"};
    expect.sends = 2;
    fill_session(recorder, expect);
    const auto services = golden_services();
    ASSERT_TRUE(recorder.set_services(services));
    using aa::core::Nanoseconds;
    using replay::Direction;
    using replay::SessionOp;
    using replay::SessionRecord;
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
                                        SessionRecord{SessionOp::reset_channels}));
    ASSERT_TRUE(recorder.record_video(
        Nanoseconds{5'000'000},
        replay::VideoRecord{1, 1, Nanoseconds{5'000'000}, true, kVideoOne}));
    // When: the post-teardown video frame is replayed without reopening.
    MapTrustStore trust;
    auto loaded = replay::load_fixture(export_or_fail(recorder));
    ASSERT_TRUE(loaded);
    replay::ReplayPlayer player(loaded.value(), replay::ReplayPlayer::Deps{trust});
    RecordingSink video_sink;
    ASSERT_TRUE(player.register_sink(proto::ChannelRole::video, video_sink));
    auto observed = player.run();
    // Then: the reset dropped the route and dispatch fails closed.
    ASSERT_FALSE(observed);
    EXPECT_EQ(observed.error().code(), aa::ErrorCode::channel_not_registered);
    EXPECT_TRUE(video_sink.payloads.empty());
}

TEST(ReplayTrace, AudioWithoutSinkFailsClosed) {
    // Given: an audio record and no registered audio sink.
    replay::Recorder recorder;
    replay::Expectation expect;
    expect.states = {"disconnected"};
    fill_session(recorder, expect);
    ASSERT_TRUE(recorder.record_audio(
        aa::core::Nanoseconds{1'000'000},
        replay::AudioRecord{aa::audio::Role::media, aa::audio::AudioFormat{48000, 2, 16},
                            aa::core::Nanoseconds{1'000'000}, kPcmOne}));
    MapTrustStore trust;
    auto loaded = replay::load_fixture(export_or_fail(recorder));
    ASSERT_TRUE(loaded);
    replay::ReplayPlayer player(loaded.value(), replay::ReplayPlayer::Deps{trust});
    // When: the audio record runs.
    auto observed = player.run();
    // Then: the typed audio error surfaces, no sink is invented.
    ASSERT_FALSE(observed);
    EXPECT_EQ(observed.error().code(), aa::ErrorCode::audio_sink_unavailable);
}

TEST(ReplayLifecycle, TrustedAdmissionReachesConnecting) {
    // Given: a discovered phone approved in the trust store.
    replay::Recorder recorder;
    replay::Expectation expect;
    expect.states = {"disconnected", "discovering", "connecting"};
    fill_session(recorder, expect);
    using aa::core::Nanoseconds;
    using replay::SessionOp;
    using replay::SessionRecord;
    ASSERT_TRUE(recorder.record_session(
        Nanoseconds{0}, SessionRecord{SessionOp::event, aa::session::Event::start_discovery}));
    ASSERT_TRUE(recorder.record_session(
        Nanoseconds{1'000'000}, SessionRecord{SessionOp::phone_discovered, {}, kApproved.key}));
    MapTrustStore trust;
    auto loaded = replay::load_fixture(export_or_fail(recorder));
    ASSERT_TRUE(loaded);
    replay::ReplayPlayer player(loaded.value(), replay::ReplayPlayer::Deps{trust});
    // When: the script runs.
    auto observed = player.run();
    // Then: admission opened connecting exactly as expected.
    ASSERT_TRUE(observed);
    EXPECT_EQ(observed.value(), expect);
    EXPECT_EQ(player.state(), aa::session::State::connecting);
}

TEST(ReplayLifecycle, UnknownAdmissionFailsClosed) {
    // Given: a discovered phone missing from the trust store.
    replay::Recorder recorder;
    replay::Expectation expect;
    expect.states = {"disconnected", "discovering"};
    fill_session(recorder, expect);
    using aa::core::Nanoseconds;
    using replay::SessionOp;
    using replay::SessionRecord;
    ASSERT_TRUE(recorder.record_session(
        Nanoseconds{0}, SessionRecord{SessionOp::event, aa::session::Event::start_discovery}));
    ASSERT_TRUE(recorder.record_session(
        Nanoseconds{1'000'000}, SessionRecord{SessionOp::phone_discovered, {}, kUnknown.key}));
    MapTrustStore trust;
    auto loaded = replay::load_fixture(export_or_fail(recorder));
    ASSERT_TRUE(loaded);
    replay::ReplayPlayer player(loaded.value(), replay::ReplayPlayer::Deps{trust});
    // When: the unknown phone is replayed.
    auto observed = player.run();
    // Then: admission refuses with the typed error and no connecting state.
    ASSERT_FALSE(observed);
    EXPECT_EQ(observed.error().code(), aa::ErrorCode::session_unknown_phone);
    EXPECT_EQ(player.state(), aa::session::State::discovering);
    EXPECT_EQ(player.observed().states, expect.states);
}

TEST(ReplayLifecycle, PairingTimeoutFailsClosed) {
    // Given: a pairing intent whose 60 s contract deadline passes on the clock.
    replay::Recorder recorder;
    replay::Expectation expect;
    expect.states = {"disconnected", "discovering", "pairing", "failed"};
    fill_session(recorder, expect);
    using aa::core::Nanoseconds;
    using replay::SessionOp;
    using replay::SessionRecord;
    ASSERT_TRUE(recorder.record_session(
        Nanoseconds{0}, SessionRecord{SessionOp::event, aa::session::Event::start_discovery}));
    ASSERT_TRUE(recorder.record_session(
        Nanoseconds{1'000'000},
        SessionRecord{SessionOp::authorize_pairing, {}, kUnknown.key}));
    ASSERT_TRUE(recorder.record_session(Nanoseconds{61'000'000'000LL},
                                        SessionRecord{SessionOp::tick}));
    MapTrustStore trust;
    auto loaded = replay::load_fixture(export_or_fail(recorder));
    ASSERT_TRUE(loaded);
    replay::ReplayPlayer player(loaded.value(), replay::ReplayPlayer::Deps{trust});
    // When: the deadline expires under the fake monotonic clock.
    auto observed = player.run();
    // Then: the session fails with the typed pairing timeout and drops trust.
    ASSERT_TRUE(observed);
    EXPECT_EQ(observed.value(), expect);
    ASSERT_TRUE(player.failure().has_value());
    EXPECT_EQ(player.failure()->code(), aa::ErrorCode::session_pairing_timeout);
}

TEST(ReplayLifecycle, CancelStopsThroughBoundToken) {
    // Given: a connected session whose stop token fires before more traffic.
    replay::Recorder recorder;
    replay::Expectation expect;
    expect.states = {"disconnected", "discovering", "connecting"};
    fill_session(recorder, expect);
    using aa::core::Nanoseconds;
    using replay::Direction;
    using replay::SessionOp;
    using replay::SessionRecord;
    ASSERT_TRUE(recorder.record_session(
        Nanoseconds{0}, SessionRecord{SessionOp::event, aa::session::Event::start_discovery}));
    ASSERT_TRUE(recorder.record_session(
        Nanoseconds{1'000'000}, SessionRecord{SessionOp::phone_discovered, {}, kApproved.key}));
    ASSERT_TRUE(recorder.record_session(Nanoseconds{2'000'000},
                                        SessionRecord{SessionOp::request_stop}));
    ASSERT_TRUE(recorder.record_transport(Nanoseconds{3'000'000}, Direction::inbound,
                                          discovery_request_frame()));
    MapTrustStore trust;
    auto loaded = replay::load_fixture(export_or_fail(recorder));
    ASSERT_TRUE(loaded);
    replay::ReplayPlayer player(loaded.value(), replay::ReplayPlayer::Deps{trust});
    // When: the next inbound frame is replayed after cancellation.
    auto observed = player.run();
    // Then: the scripted transport reports the typed cancel error.
    ASSERT_FALSE(observed);
    EXPECT_EQ(observed.error().code(), aa::ErrorCode::cancelled);
}

} // namespace
