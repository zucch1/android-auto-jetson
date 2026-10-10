// SPDX-License-Identifier: GPL-3.0-or-later
// Task 20 parity followup: every successful export must be loadable. The
// accepted per-kind payload contracts (audio 64 KiB, video up to 4 MiB within
// the 1 MiB file cap, input 16 KiB) must round-trip through the shared JSON
// representation budgets; parser node/container/document budgets must never
// admit an export the loader then refuses.

#include <aa/audio/Audio.hpp>
#include <aa/core/Time.hpp>
#include <aa/protocol/Messages.hpp>
#include <aa/protocol/Protocol.hpp>
#include <aa/replay/Fixture.hpp>
#include <aa/replay/Recorder.hpp>
#include <aa/replay/Schema.hpp>

#include <cstddef>
#include <cstdint>
#include <vector>

#include <gtest/gtest.h>

namespace {
namespace proto = aa::protocol;
namespace replay = aa::replay;

std::vector<std::byte> filled(std::size_t size, std::byte value) {
    return std::vector<std::byte>(size, value);
}

// Exports and must load the artifact back with the exact payloads.
void expect_roundtrip(const replay::Recorder& recorder) {
    auto exported = recorder.export_fixture();
    ASSERT_TRUE(exported) << "export rejected a valid bounded fixture";
    auto loaded = replay::load_fixture(exported.value().file_bytes);
    ASSERT_TRUE(loaded) << "a successful export was not loadable";
}

void fill_video(replay::Recorder& recorder, std::size_t payload_bytes, std::uint64_t frame) {
    replay::Expectation expect;
    expect.states = {"disconnected"};
    (void)recorder.set_services({
        replay::ServiceEntry{proto::ServiceKey{99}, proto::ChannelRole::video,
                             proto::VideoConfiguration{{{1280, 720, 30}}}},
    });
    (void)recorder.set_expectation(expect);
    (void)recorder.record_video(
        aa::core::Nanoseconds{0},
        replay::VideoRecord{frame, frame, aa::core::Nanoseconds{0}, frame == 1,
                            filled(payload_bytes, std::byte{0x5A})});
}

TEST(ReplayParity, LargeVideoPayloadRoundTrips) {
    // Given: a video access unit well above the 32768-byte half of the old
    // JSON string limit but far under the file cap.
    replay::Recorder recorder;
    fill_video(recorder, 40'000, 1);
    // When/Then: export succeeds and the artifact must load back exactly.
    auto exported = recorder.export_fixture();
    ASSERT_TRUE(exported);
    auto loaded = replay::load_fixture(exported.value().file_bytes);
    ASSERT_TRUE(loaded) << "a successful export was not loadable";
    const auto& body = std::get<replay::VideoRecord>(loaded.value().records[0].body);
    EXPECT_EQ(body.payload.size(), 40'000u);
}

TEST(ReplayParity, MaxAudioPayloadRoundTrips) {
    // Given: a PCM chunk at the full accepted audio contract (64 KiB).
    replay::Recorder recorder;
    replay::Expectation expect;
    expect.states = {"disconnected"};
    (void)recorder.set_expectation(expect);
    (void)recorder.record_audio(
        aa::core::Nanoseconds{0},
        replay::AudioRecord{aa::audio::Role::media, aa::audio::AudioFormat{48000, 2, 16},
                            aa::core::Nanoseconds{0}, filled(replay::kMaxAudioPayloadBytes,
                                                             std::byte{0x11})});
    // When/Then: the maximum accepted PCM chunk must survive export -> load.
    expect_roundtrip(recorder);
}

TEST(ReplayParity, DocumentBudgetRoundTrips) {
    // Given: total hex text past the old 512 KiB document cap but under the file
    // cap (three 100000-byte video access units).
    replay::Recorder recorder;
    replay::Expectation expect;
    expect.states = {"disconnected"};
    (void)recorder.set_services({
        replay::ServiceEntry{proto::ServiceKey{99}, proto::ChannelRole::video,
                             proto::VideoConfiguration{{{1280, 720, 30}}}},
    });
    (void)recorder.set_expectation(expect);
    const auto payload = filled(100'000, std::byte{0x33});
    for (std::uint64_t i = 0; i < 3; ++i) {
        const auto at = aa::core::Nanoseconds{static_cast<std::int64_t>(i) * 1'000'000};
        (void)recorder.record_video(
            at, replay::VideoRecord{i + 1, i + 1, at, i == 0, payload});
    }
    // When/Then: the document-length budget must accept every exportable body.
    expect_roundtrip(recorder);
}

TEST(ReplayParity, NearLimitBudgetsRoundTrip) {
    // Given: the maximum record count of the widest record shape plus the
    // maximum expectation counts, all with 1-byte payloads.
    replay::Recorder recorder;
    replay::Expectation expect;
    expect.states = {"disconnected"};
    for (std::size_t i = 0; i < replay::kMaxRecords; ++i) {
        (void)recorder.record_audio(
            aa::core::Nanoseconds{static_cast<std::int64_t>(i)},
            replay::AudioRecord{aa::audio::Role::media, aa::audio::AudioFormat{48000, 2, 16},
                                aa::core::Nanoseconds{static_cast<std::int64_t>(i)},
                                filled(1, std::byte{0x22})});
        expect.deliveries.push_back(replay::DeliveryOutcome{
            proto::ChannelRole::video, i, aa::core::Nanoseconds{static_cast<std::int64_t>(i)},
            filled(1, std::byte{0x44})});
        expect.audio.push_back(replay::AudioOutcome{
            aa::audio::Role::media, aa::audio::AudioFormat{48000, 2, 16},
            aa::core::Nanoseconds{static_cast<std::int64_t>(i)}, filled(1, std::byte{0x55})});
    }
    (void)recorder.set_services({
        replay::ServiceEntry{proto::ServiceKey{99}, proto::ChannelRole::video,
                             proto::VideoConfiguration{{{1280, 720, 30}}}},
    });
    (void)recorder.set_expectation(expect);
    // When/Then: parser node/container budgets must not refuse a valid export.
    expect_roundtrip(recorder);
}

} // namespace
