// SPDX-License-Identifier: GPL-3.0-or-later
// Task 20 followup (defect 1): the bounded recorder must be atomic - a
// rejected record mutates nothing - and every budget (record count, trace
// duration, payload bytes, metadata, expectation, phone keys) is enforced
// before allocation/copy/commit at the recorder boundary.

#include "fixtures.hpp"

#include <aa/replay/Recorder.hpp>
#include <aa/replay/Schema.hpp>

#include <cstdint>
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

TEST(ReplayRecorder, BackdatedRecordLeavesExportByteIdentical) {
    // Given: a recorder holding the golden scenario.
    replay::Recorder recorder;
    fill_golden(recorder);
    const auto reference = export_or_fail(recorder);
    // When: a backdated record is refused.
    const auto refused = recorder.record_input(
        aa::core::Nanoseconds{1}, replay::InputRecord{9, aa::core::Nanoseconds{1}, kInputOne});
    // Then: nothing mutates and export stays byte-identical.
    ASSERT_FALSE(refused);
    EXPECT_EQ(refused.error().code(), aa::ErrorCode::invalid_argument);
    EXPECT_EQ(export_or_fail(recorder), reference);
}

TEST(ReplayRecorder, TraceDurationEnforcedAtRecordTime) {
    // Given: an empty recorder.
    replay::Recorder recorder;
    ASSERT_TRUE(recorder.record_input(
        aa::core::Nanoseconds{0}, replay::InputRecord{1, aa::core::Nanoseconds{0}, kInputOne}));
    // When: the next record exceeds the trace duration budget.
    const auto refused = recorder.record_input(
        aa::core::Nanoseconds{replay::kMaxTraceNs + 1},
        replay::InputRecord{2, aa::core::Nanoseconds{0}, kInputOne});
    // Then: the record is refused at record time with a typed error.
    ASSERT_FALSE(refused);
    EXPECT_EQ(refused.error().code(), aa::ErrorCode::invalid_argument);
}

TEST(ReplayRecorder, OversizedExpectationRefused) {
    // Given: an empty recorder and an expectation past the count budget.
    replay::Recorder recorder;
    replay::Expectation huge;
    huge.states = {"disconnected"};
    for (int i = 0; i < 3000; ++i) {
        huge.deliveries.push_back(
            replay::DeliveryOutcome{proto::ChannelRole::video, 1,
                                    aa::core::Nanoseconds{0}, kVideoOne});
    }
    // When/Then: the expectation is refused at set time.
    const auto refused = recorder.set_expectation(huge);
    ASSERT_FALSE(refused);
    EXPECT_EQ(refused.error().code(), aa::ErrorCode::invalid_argument);
}

TEST(ReplayRecorder, OversizedExpectationPayloadRefused) {
    // Given: few deliveries whose aggregate payload exceeds the total budget.
    replay::Recorder recorder;
    replay::Expectation huge;
    huge.states = {"disconnected"};
    const std::vector<std::byte> blob(std::size_t{3} * 1024U * 1024U, std::byte{0xAA});
    for (int i = 0; i < 4; ++i) {
        huge.deliveries.push_back(replay::DeliveryOutcome{
            proto::ChannelRole::video, static_cast<std::uint64_t>(i),
            aa::core::Nanoseconds{0}, blob});
    }
    const auto refused = recorder.set_expectation(huge);
    ASSERT_FALSE(refused);
    EXPECT_EQ(refused.error().code(), aa::ErrorCode::transport_oversize_frame);
}

TEST(ReplayRecorder, UnboundedMetadataRefused) {
    // Given: metadata past the field-count budget.
    replay::Recorder recorder;
    bool refused = false;
    for (int i = 0; i < 40 && !refused; ++i) {
        refused = !recorder
                       .set_metadata(aa::diagnostics::EventField::count(
                           "count" + std::to_string(i), i))
                       .has_value();
    }
    EXPECT_TRUE(refused);
    // Given: one metadata text past the text budget.
    replay::Recorder long_text;
    const auto refused_text = long_text.set_metadata(
        aa::diagnostics::EventField::text("version", std::string(5000, '1')));
    ASSERT_FALSE(refused_text);
    EXPECT_EQ(refused_text.error().code(), aa::ErrorCode::invalid_argument);
}

TEST(ReplayRecorder, LongPhoneKeyRefused) {
    // Given: an empty recorder.
    replay::Recorder recorder;
    // When: a synthetic phone key exceeds the phone-key budget.
    const auto refused = recorder.record_session(
        aa::core::Nanoseconds{0},
        replay::SessionRecord{replay::SessionOp::phone_discovered, {},
                              std::string(400, 'p')});
    // Then: it is refused at record time.
    ASSERT_FALSE(refused);
    EXPECT_EQ(refused.error().code(), aa::ErrorCode::invalid_argument);
}

TEST(ReplayRecorder, InvalidAudioFormatRefused) {
    // Given: an empty recorder.
    replay::Recorder recorder;
    // When: a PCM record carries a zero-rate / zero-bits / zero-channel format.
    const auto refused = recorder.record_audio(
        aa::core::Nanoseconds{0},
        replay::AudioRecord{aa::audio::Role::media, aa::audio::AudioFormat{0, 0, 0},
                            aa::core::Nanoseconds{0}, kPcmOne});
    // Then: the typed format contract rejects it before any copy.
    ASSERT_FALSE(refused);
    EXPECT_EQ(refused.error().code(), aa::ErrorCode::invalid_argument);
}

TEST(ReplayRecorder, NegativeMediaTimestampRefused) {
    // Given: an empty recorder.
    replay::Recorder recorder;
    // When: a video record carries a negative media timestamp.
    const auto refused = recorder.record_video(
        aa::core::Nanoseconds{0},
        replay::VideoRecord{1, 1, aa::core::Nanoseconds{-1}, true, kVideoOne});
    // Then: it is refused at record time.
    ASSERT_FALSE(refused);
    EXPECT_EQ(refused.error().code(), aa::ErrorCode::invalid_argument);
}

TEST(ReplayRecorder, ZeroServiceIdRefused) {
    // Given: a service entry with the unset id zero.
    replay::Recorder recorder;
    // When: the unset id reaches the recorder.
    const auto refused = recorder.set_services({
        replay::ServiceEntry{proto::ServiceKey{0}, proto::ChannelRole::video,
                             proto::VideoConfiguration{{{1280, 720, 30}, {800, 480, 30}}}},
    });
    // Then: it is refused before publish; unset ids are never fixture ids.
    ASSERT_FALSE(refused);
    EXPECT_EQ(refused.error().code(), aa::ErrorCode::invalid_argument);
}

TEST(ReplayRecorder, LargeButtonKeycodesAccepted) {
    // Given: the accepted task-18 button keycodes up to 65535.
    replay::Recorder recorder;
    replay::Expectation expect;
    expect.states = {"disconnected"};
    ASSERT_TRUE(recorder.set_expectation(expect));
    ASSERT_TRUE(recorder.set_services({
        replay::ServiceEntry{proto::ServiceKey{77}, proto::ChannelRole::input,
                             proto::ButtonConfiguration{{300, 65535}}},
    }));
    ASSERT_TRUE(recorder.record_input(
        aa::core::Nanoseconds{0}, replay::InputRecord{1, aa::core::Nanoseconds{0}, kInputOne}));
    // When: the fixture round-trips export -> load.
    auto exported = recorder.export_fixture();
    ASSERT_TRUE(exported);
    auto loaded = replay::load_fixture(exported.value().file_bytes);
    // Then: the large keycodes survive untouched.
    ASSERT_TRUE(loaded);
    ASSERT_EQ(loaded.value().services.size(), 1u);
    const auto& configuration = loaded.value().services[0].configuration;
    const auto* buttons = std::get_if<proto::ButtonConfiguration>(&configuration);
    ASSERT_NE(buttons, nullptr);
    EXPECT_EQ(buttons->keycodes, (std::vector<std::int32_t>{300, 65535}));
}

} // namespace
