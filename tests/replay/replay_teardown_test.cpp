// SPDX-License-Identifier: GPL-3.0-or-later
// Task 20 followup (defect 4): actual lifecycle observation - terminal
// cleanup and link loss - tears down channel state. A fresh session/link needs
// fresh advertisement and open; a stale partial fragment never crosses a
// reconnect. Cancel-no-delivery lives in replay_cancel_test.cpp.

#include "fixtures.hpp"

#include <aa/replay/Player.hpp>
#include <aa/replay/Schema.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
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

struct RunOutcome final {
    std::optional<aa::Error> failure;
    RecordingSink video;
    RecordingSink input;
    AudioCapture audio;
    std::vector<std::string> states;
};

void run_file(const std::vector<std::byte>& file, RunOutcome& outcome) {
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
    outcome.states = player.observed().states;
    if (!observed) {
        outcome.failure = observed.error();
    }
}

replay::Expectation expectation_of(std::vector<std::string> states,
                                    std::int64_t sends) {
    replay::Expectation expect;
    expect.states = std::move(states);
    expect.sends = sends;
    return expect;
}

std::vector<std::vector<std::byte>> fragmented(std::uint8_t channel) {
    std::vector<std::byte> payload(17'000, std::byte{0x5A});
    auto frames = aa::transport::encode_message(
        aa::transport::Message{channel, aa::transport::MessageKind::specific,
                               std::move(payload)},
        aa::transport::Encryption::plain, nullptr);
    if (!frames || frames.value().size() != 2u) {
        ADD_FAILURE() << "fragmented() encode failed";
        return {};
    }
    return frames.value();
}

// Session script shared by the cleanup scenarios: connect, advertise, open,
// deliver one video, then a full owner-thread teardown (stop -> cleanup).
void fill_connect_open_video_teardown(replay::Recorder& recorder) {
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
    ASSERT_TRUE(recorder.record_video(
        Nanoseconds{4'000'000},
        replay::VideoRecord{1, 1, Nanoseconds{4'000'000}, true, kVideoOne}));
    ASSERT_TRUE(recorder.record_session(Nanoseconds{5'000'000},
                                        SessionRecord{SessionOp::request_stop}));
    ASSERT_TRUE(recorder.record_session(Nanoseconds{6'000'000}, SessionRecord{SessionOp::tick}));
    ASSERT_TRUE(recorder.record_session(
        Nanoseconds{7'000'000},
        SessionRecord{SessionOp::event, aa::session::Event::cleanup_finished}));
    ASSERT_TRUE(recorder.record_session(Nanoseconds{8'000'000},
                                        SessionRecord{SessionOp::attach_fresh}));
    ASSERT_TRUE(recorder.record_session(
        Nanoseconds{9'000'000}, SessionRecord{SessionOp::event, aa::session::Event::start_discovery}));
    ASSERT_TRUE(recorder.record_session(
        Nanoseconds{10'000'000},
        SessionRecord{SessionOp::phone_discovered, {}, kApproved.key}));
}

TEST(ReplayTeardown, CleanupTeardownRequiresFreshAdvertisement) {
    // Given: a torn-down session whose fresh link has not re-advertised/reopened.
    replay::Recorder recorder;
    fill_connect_open_video_teardown(recorder);
    ASSERT_TRUE(recorder.record_video(
        aa::core::Nanoseconds{11'000'000},
        replay::VideoRecord{2, 2, aa::core::Nanoseconds{11'000'000}, false, kVideoTwo}));
    ASSERT_TRUE(recorder.set_expectation(
        expectation_of({"disconnected", "discovering", "connecting", "stopping",
                           "disconnected", "discovering", "connecting"},
                          2)));
    // When: the post-teardown video record runs on the fresh link.
    RunOutcome outcome;
    run_file(export_or_fail(recorder), outcome);
    // Then: stale routes are gone; only the pre-teardown delivery happened.
    ASSERT_TRUE(outcome.failure.has_value());
    EXPECT_EQ(outcome.failure->code(), aa::ErrorCode::channel_not_registered);
    ASSERT_EQ(outcome.video.payloads.size(), 1u);
    EXPECT_EQ(outcome.video.payloads[0], kVideoOne);
}

TEST(ReplayTeardown, FreshReopenAfterCleanupDeliversAgain) {
    // Given: a torn-down session that re-advertises and reopens on the fresh link.
    replay::Recorder recorder;
    fill_connect_open_video_teardown(recorder);
    const auto services = golden_services();
    using aa::core::Nanoseconds;
    using replay::Direction;
    ASSERT_TRUE(recorder.record_transport(Nanoseconds{11'000'000}, Direction::inbound,
                                          discovery_request_frame()));
    ASSERT_TRUE(recorder.record_transport(Nanoseconds{11'500'000}, Direction::outbound,
                                          discovery_response_frame(services)));
    ASSERT_TRUE(recorder.record_transport(Nanoseconds{12'000'000}, Direction::inbound,
                                          open_request_frame(99)));
    ASSERT_TRUE(recorder.record_transport(Nanoseconds{12'500'000}, Direction::outbound,
                                          open_response_frame()));
    ASSERT_TRUE(recorder.record_video(
        Nanoseconds{13'000'000},
        replay::VideoRecord{2, 2, Nanoseconds{13'000'000}, false, kVideoTwo}));
    replay::Expectation expect = expectation_of(
        {"disconnected", "discovering", "connecting", "stopping", "disconnected",
         "discovering", "connecting"},
        4);
    expect.deliveries = {
        {proto::ChannelRole::video, 1, Nanoseconds{4'000'000}, kVideoOne},
        {proto::ChannelRole::video, 2, Nanoseconds{13'000'000}, kVideoTwo},
    };
    ASSERT_TRUE(recorder.set_expectation(expect));
    // When: the reopened session runs to completion.
    RunOutcome outcome;
    run_file(export_or_fail(recorder), outcome);
    // Then: both deliveries arrive exactly as expected.
    EXPECT_FALSE(outcome.failure.has_value());
    ASSERT_EQ(outcome.video.payloads.size(), 2u);
    EXPECT_EQ(outcome.video.payloads[0], kVideoOne);
    EXPECT_EQ(outcome.video.payloads[1], kVideoTwo);
}

TEST(ReplayTeardown, FailureResetTeardownRequiresFreshAdvertisement) {
    // Given: a session that fails and resets into a fresh cycle.
    replay::Recorder recorder;
    fill_connect_open_video_teardown(recorder);
    using aa::core::Nanoseconds;
    using replay::SessionOp;
    using replay::SessionRecord;
    ASSERT_TRUE(recorder.record_session(
        Nanoseconds{11'000'000},
        SessionRecord{SessionOp::fail, {}, {}, aa::ErrorCode::internal}));
    ASSERT_TRUE(recorder.record_session(
        Nanoseconds{12'000'000},
        SessionRecord{SessionOp::event, aa::session::Event::reset}));
    ASSERT_TRUE(recorder.record_video(
        Nanoseconds{13'000'000},
        replay::VideoRecord{2, 2, Nanoseconds{13'000'000}, false, kVideoTwo}));
    ASSERT_TRUE(recorder.set_expectation(
        expectation_of({"disconnected", "discovering", "connecting", "stopping",
                        "disconnected", "discovering", "connecting", "failed",
                        "disconnected"},
                       2)));
    // When: the post-reset video record runs without fresh advertisement.
    RunOutcome outcome;
    run_file(export_or_fail(recorder), outcome);
    // Then: the failure teardown cleared the routes.
    ASSERT_TRUE(outcome.failure.has_value());
    EXPECT_EQ(outcome.failure->code(), aa::ErrorCode::channel_not_registered);
    ASSERT_EQ(outcome.video.payloads.size(), 1u);
}

TEST(ReplayTeardown, StalePartialFragmentDoesNotCrossReconnect) {
    // Given: a fragmented message whose FIRST half is stranded by link loss.
    replay::Recorder recorder;
    const auto services = golden_services();
    const auto parts = fragmented(99);
    using aa::core::Nanoseconds;
    using replay::Direction;
    using replay::SessionOp;
    using replay::SessionRecord;
    ASSERT_TRUE(recorder.set_services(services));
    ASSERT_TRUE(recorder.record_session(
        Nanoseconds{0}, SessionRecord{SessionOp::event, aa::session::Event::start_discovery}));
    ASSERT_TRUE(recorder.record_session(
        Nanoseconds{1'000'000}, SessionRecord{SessionOp::phone_discovered, {}, kApproved.key}));
    ASSERT_TRUE(recorder.record_session(
        Nanoseconds{2'000'000},
        SessionRecord{SessionOp::event, aa::session::Event::transport_ready}));
    ASSERT_TRUE(recorder.record_session(
        Nanoseconds{3'000'000},
        SessionRecord{SessionOp::event, aa::session::Event::negotiation_succeeded}));
    ASSERT_TRUE(recorder.record_transport(Nanoseconds{4'000'000}, Direction::inbound,
                                          parts[0]));
    ASSERT_TRUE(recorder.record_session(
        Nanoseconds{5'000'000},
        SessionRecord{SessionOp::event, aa::session::Event::link_lost}));
    ASSERT_TRUE(recorder.record_session(Nanoseconds{6'000'000},
                                        SessionRecord{SessionOp::attach_fresh}));
    ASSERT_TRUE(recorder.record_session(
        Nanoseconds{7'000'000},
        SessionRecord{SessionOp::event, aa::session::Event::reconnect}));
    ASSERT_TRUE(recorder.record_transport(Nanoseconds{8'000'000}, Direction::inbound,
                                          discovery_request_frame()));
    ASSERT_TRUE(recorder.record_transport(Nanoseconds{8'500'000}, Direction::outbound,
                                          discovery_response_frame(services)));
    ASSERT_TRUE(recorder.record_transport(Nanoseconds{9'000'000}, Direction::inbound,
                                          open_request_frame(99)));
    ASSERT_TRUE(recorder.record_transport(Nanoseconds{9'500'000}, Direction::outbound,
                                          open_response_frame()));
    ASSERT_TRUE(recorder.record_transport(Nanoseconds{10'000'000}, Direction::inbound,
                                          parts[1]));
    ASSERT_TRUE(recorder.set_expectation(expectation_of(
        {"disconnected", "discovering", "connecting", "negotiating", "active",
         "reconnecting", "connecting"},
        2)));
    // When: the stranded LAST half arrives after the reconnect and fresh open.
    RunOutcome outcome;
    run_file(export_or_fail(recorder), outcome);
    // Then: the stale FIRST half is gone - the half is malformed alone and
    // nothing from the pre-reconnect message is delivered.
    ASSERT_TRUE(outcome.failure.has_value());
    EXPECT_EQ(outcome.failure->code(), aa::ErrorCode::transport_malformed_frame);
    EXPECT_TRUE(outcome.video.payloads.empty());
}

} // namespace
