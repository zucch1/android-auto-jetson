// SPDX-License-Identifier: GPL-3.0-or-later
// Task 20 replay bounds: the fixture schema gate rejects malformed record
// shapes, oversized counts/payload/time, nonmonotonic timestamps, wrong
// schema, inconsistent IDs/metadata, illegal state traces, raw identifiers,
// secret-bearing unknown keys, missing opaque policy and hash corruption -
// each with a typed error and no unbounded allocation.

#include "fixtures.hpp"

#include <aa/replay/Player.hpp>
#include <aa/replay/Schema.hpp>

#include <cstdint>
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

std::string discovery_hex() { return hex_of(discovery_request_frame()); }

void expect_reject(const std::vector<std::byte>& file, aa::ErrorCode code) {
    const auto loaded = replay::load_fixture(file);
    ASSERT_FALSE(loaded);
    EXPECT_EQ(loaded.error().code(), code);
}

TEST(ReplayBounds, WrongSchemaVersionRejected) {
    const auto bad = tamper_resealed(golden_file(), R"("aa-replay-fixture-v1")",
                            R"("aa-replay-fixture-v9")");
    ASSERT_TRUE(bad);
    expect_reject(*bad, aa::ErrorCode::invalid_argument);
}

TEST(ReplayBounds, MalformedRecordShapeRejected) {
    const auto bad = tamper_resealed(golden_file(), R"("kind":"audio")", R"("kind":"bogus")");
    ASSERT_TRUE(bad);
    expect_reject(*bad, aa::ErrorCode::invalid_argument);
}

TEST(ReplayBounds, MissingOpaquePolicyRejected) {
    const auto from = R"("opaque":"synthetic","frame":")" + discovery_hex() + R"(")";
    const auto bad = tamper_resealed(golden_file(), from, R"("frame":")" + discovery_hex() + R"(")");
    ASSERT_TRUE(bad);
    expect_reject(*bad, aa::ErrorCode::invalid_argument);
}

TEST(ReplayBounds, LyingOpaquePolicyRejected) {
    const auto from = R"("opaque":"synthetic","frame":")" + discovery_hex() + R"(")";
    const auto bad = tamper_resealed(golden_file(), from, R"("opaque":"raw","frame":")" + discovery_hex()
                                                    + R"(")");
    ASSERT_TRUE(bad);
    expect_reject(*bad, aa::ErrorCode::invalid_argument);
}

TEST(ReplayBounds, OversizedPayloadRejected) {
    const auto from = R"("frame":")" + discovery_hex() + R"(")";
    const auto bad = tamper_resealed(golden_file(), from, R"("frame":")" + std::string(40'000, 'a') + R"(")");
    ASSERT_TRUE(bad);
    expect_reject(*bad, aa::ErrorCode::transport_oversize_frame);
}

TEST(ReplayBounds, NonmonotonicTimestampRejected) {
    const auto bad = tamper_resealed(golden_file(), R"("t":12000000)", R"("t":1)");
    ASSERT_TRUE(bad);
    expect_reject(*bad, aa::ErrorCode::invalid_argument);
}

TEST(ReplayBounds, OversizedTraceDurationRejected) {
    const auto bad = tamper_resealed(golden_file(), R"("t":12000000)", R"("t":5000000000000)");
    ASSERT_TRUE(bad);
    expect_reject(*bad, aa::ErrorCode::invalid_argument);
}

TEST(ReplayBounds, IllegalStateTraceRejected) {
    const auto bad =
        tamper_resealed(golden_file(),
               R"("states":["disconnected","discovering","connecting","negotiating","active"])",
               R"("states":["disconnected","active"])");
    ASSERT_TRUE(bad);
    expect_reject(*bad, aa::ErrorCode::session_illegal_transition);
}

TEST(ReplayBounds, InconsistentServiceIdRejected) {
    const auto bad = tamper_resealed(golden_file(), R"("id":100,)", R"("id":99,)");
    ASSERT_TRUE(bad);
    expect_reject(*bad, aa::ErrorCode::protocol_malformed_message);
}

TEST(ReplayBounds, InconsistentDeliveryRoleRejected) {
    const auto bad =
        tamper_resealed(golden_file(), R"("deliveries":[{"role":"video","sequence":1)",
               R"("deliveries":[{"role":"microphone","sequence":1)");
    ASSERT_TRUE(bad);
    expect_reject(*bad, aa::ErrorCode::invalid_argument);
}

TEST(ReplayBounds, InconsistentMetadataCountRejected) {
    const auto bad = tamper_resealed(golden_file(), R"(\"frames\":19)", R"(\"frames\":199)");
    ASSERT_TRUE(bad);
    expect_reject(*bad, aa::ErrorCode::invalid_argument);
}

TEST(ReplayBounds, ExpectedSendCountMismatchRejected) {
    const auto bad = tamper_resealed(golden_file(), R"("sends":5)", R"("sends":7)");
    ASSERT_TRUE(bad);
    expect_reject(*bad, aa::ErrorCode::invalid_argument);
}

TEST(ReplayBounds, RawIdentifierOnInnocuousKeyRejected) {
    const auto bad =
        tamper_resealed(golden_file(), R"(\"frames\":19)", R"(\"frames\":123456789012345)");
    ASSERT_TRUE(bad);
    expect_reject(*bad, aa::ErrorCode::invalid_argument);
}

TEST(ReplayBounds, RawIdentifierOnSensitiveKeyRejected) {
    const auto bad = tamper_resealed(golden_file(), R"(\"phone_id\":\"[redacted]\")",
                            R"(\"phone_id\":\"AA:BB:CC:DD:EE:FF\")");
    ASSERT_TRUE(bad);
    expect_reject(*bad, aa::ErrorCode::invalid_argument);
}

TEST(ReplayBounds, UnknownSecretKeyRejected) {
    const auto bad =
        tamper_resealed(golden_file(), R"(\"codec\":\"h264\")",
               R"(\"codec\":\"h264\",\"api_key\":\"hunter2\")");
    ASSERT_TRUE(bad);
    expect_reject(*bad, aa::ErrorCode::invalid_argument);
}

TEST(ReplayBounds, CorruptedHashRejected) {
    const auto bad = flip_hash_char(golden_file());
    ASSERT_TRUE(bad);
    expect_reject(*bad, aa::ErrorCode::invalid_argument);
}

TEST(ReplayBounds, ExportGateRejectsRawIdentifierMetadata) {
    // Given: a fixture whose metadata carries an IMEI-shaped count.
    replay::Recorder recorder;
    fill_golden(recorder);
    replay::Fixture dirty = recorder.fixture();
    dirty.metadata =
        R"({"kind":"transport_stats","session":1,"mono_ns":0,"frames":123456789012345})";
    // When: the export gate seals it.
    const auto exported = replay::export_fixture(dirty);
    // Then: the gate refuses instead of laundering the identifier.
    ASSERT_FALSE(exported);
    EXPECT_EQ(exported.error().code(), aa::ErrorCode::invalid_argument);
}

TEST(ReplayBounds, RecorderRejectsBoundOverruns) {
    // Given: a recorder at its record-count, payload and time bounds.
    replay::Recorder recorder;
    const auto frame = make_bytes({0x01, 0x02, 0x03, 0x04});
    ASSERT_TRUE(recorder.record_input(
        aa::core::Nanoseconds{5},
        replay::InputRecord{1, aa::core::Nanoseconds{5}, frame}));
    // When/Then: nondecreasing time is required.
    const auto backwards = recorder.record_input(
        aa::core::Nanoseconds{1}, replay::InputRecord{2, aa::core::Nanoseconds{1}, frame});
    ASSERT_FALSE(backwards);
    EXPECT_EQ(backwards.error().code(), aa::ErrorCode::invalid_argument);
    // When/Then: an oversize transport frame is refused.
    const std::vector<std::byte> huge(20'000, std::byte{0xAA});
    const auto oversize = recorder.record_transport(
        aa::core::Nanoseconds{6}, replay::Direction::inbound, huge);
    ASSERT_FALSE(oversize);
    EXPECT_EQ(oversize.error().code(), aa::ErrorCode::transport_oversize_frame);
    // When/Then: the record count is bounded.
    bool refused = false;
    for (int i = 0; i < 1100 && !refused; ++i) {
        const auto next = recorder.record_input(
            aa::core::Nanoseconds{10 + i},
            replay::InputRecord{static_cast<std::uint64_t>(i + 2),
                                aa::core::Nanoseconds{10 + i}, frame});
        refused = !next.has_value();
        if (refused) {
            EXPECT_EQ(next.error().code(), aa::ErrorCode::invalid_argument);
        }
    }
    EXPECT_TRUE(refused);
}

TEST(ReplayBounds, LoaderRejectsOversizedFileAndGarbage) {
    const std::vector<std::byte> huge(std::size_t{2} * 1024U * 1024U, std::byte{0x41});
    expect_reject(huge, aa::ErrorCode::transport_oversize_frame);
    const std::vector<std::byte> garbage = to_bytes("not a fixture at all");
    expect_reject(garbage, aa::ErrorCode::invalid_argument);
}

TEST(ReplayBounds, KindBodyMismatchRejectedWithoutThrow) {
    // Given: a public Fixture whose record kind disagrees with its variant body.
    replay::Recorder recorder;
    fill_golden(recorder);
    replay::Fixture mismatched = recorder.fixture();
    mismatched.records[0].kind = replay::RecordKind::transport;
    // When: the export gate seals it and the player runs it.
    const auto exported = replay::export_fixture(mismatched);
    // Then: both boundaries reject with typed errors, never an exception.
    ASSERT_FALSE(exported);
    EXPECT_EQ(exported.error().code(), aa::ErrorCode::invalid_argument);
    auto loaded = replay::load_fixture(golden_file());
    ASSERT_TRUE(loaded);
    loaded.value().records[0].kind = replay::RecordKind::transport;
    MapTrustStore trust;
    replay::ReplayPlayer player(loaded.value(), replay::ReplayPlayer::Deps{trust});
    auto observed = player.run();
    ASSERT_FALSE(observed);
    EXPECT_EQ(observed.error().code(), aa::ErrorCode::invalid_argument);
}

TEST(ReplayBounds, InvalidAudioFormatRejected) {
    // Given: an audio record whose typed format violates the accepted contract.
    const auto bad = tamper_resealed(golden_file(), R"("rate":48000,"bits":16,"channels":2)",
                                     R"("rate":0,"bits":0,"channels":0)");
    ASSERT_TRUE(bad);
    // When/Then: the loader rejects it on the format boundary, hash intact.
    expect_reject(*bad, aa::ErrorCode::invalid_argument);
}

TEST(ReplayBounds, NegativeMediaTimestampRejected) {
    // Given: a video record with a negative media timestamp.
    const auto bad = tamper_resealed(golden_file(), R"("ts":5000000,"idr")", R"("ts":-1,"idr")");
    ASSERT_TRUE(bad);
    // When/Then: the loader rejects it on the timestamp boundary, hash intact.
    expect_reject(*bad, aa::ErrorCode::invalid_argument);
}

TEST(ReplayBounds, NegativeExpectationTimestampRejected) {
    // Given: an expected delivery with a negative media timestamp.
    const auto bad =
        tamper_resealed(golden_file(), R"("ts":5000000,"payload":"0000000165888400")",
                        R"("ts":-1,"payload":"0000000165888400")");
    ASSERT_TRUE(bad);
    // When/Then: the loader rejects the expectation too, hash intact.
    expect_reject(*bad, aa::ErrorCode::invalid_argument);
}

TEST(ReplayBounds, ZeroServiceIdRejectedAtGate) {
    // Given: a fixture service carrying the unset id zero.
    const auto bad = tamper_resealed(golden_file(), R"("id":100,)", R"("id":0,)");
    ASSERT_TRUE(bad);
    // When/Then: the gate refuses it instead of silently allocating an id.
    expect_reject(*bad, aa::ErrorCode::invalid_argument);
}

TEST(ReplayBounds, WrongSchemaRejectedWithCorrectHash) {
    // Given: a wrong envelope schema over a body whose hash is correct.
    const auto bad =
        tamper_resealed(golden_file(), R"("aa-replay-fixture-v1")",
                        R"("aa-replay-fixture-v9")");
    ASSERT_TRUE(bad);
    // When/Then: the schema check rejects it, not the checksum.
    expect_reject(*bad, aa::ErrorCode::invalid_argument);
}

} // namespace
