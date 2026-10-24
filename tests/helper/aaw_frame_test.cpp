// SPDX-License-Identifier: GPL-3.0-or-later
#include <aa/helper/AawFrame.hpp>
#include <gtest/gtest.h>
#include <array>

namespace aa::helper {
namespace {
class KnownId : public ::testing::TestWithParam<std::uint16_t> {};
TEST_P(KnownId, RoundTripsWhenKnown) {
    // Given: a known ID and opaque binary payload.
    const std::vector<std::uint8_t> payload{0, 0xff, 0x42};
    const auto wire = encode_aaw(GetParam(), payload);
    ASSERT_TRUE(wire);
    EXPECT_EQ(*wire, (std::vector<std::uint8_t>{0, 3, 0,
        static_cast<std::uint8_t>(GetParam()), 0, 0xff, 0x42}));
    AawDecoder decoder;
    // When: the encoded record is fed.
    decoder.feed(*wire);
    const auto result = decoder.next();
    // Then: the original record is recovered.
    ASSERT_TRUE(std::holds_alternative<AawRecord>(result));
    EXPECT_EQ(std::get<AawRecord>(result).id, GetParam());
    EXPECT_EQ(std::get<AawRecord>(result).payload, payload);
    EXPECT_TRUE(std::holds_alternative<std::monostate>(decoder.next()));
    EXPECT_FALSE(decoder.finish());
}
INSTANTIATE_TEST_SUITE_P(AllIds, KnownId, ::testing::Values(1, 2, 3, 4, 5, 6, 7));

TEST(AawFrame, AccumulatesWhenFedByteByByte) {
    // Given: an independently specified record.
    const std::array<std::uint8_t, 7> wire{0, 3, 0, 6, 9, 8, 7};
    AawDecoder decoder;
    // When: all but the final byte arrive separately.
    for (std::size_t i = 0; i + 1 < wire.size(); ++i) {
        decoder.feed(std::span{wire}.subspan(i, 1));
        EXPECT_TRUE(std::holds_alternative<std::monostate>(decoder.next()));
    }
    decoder.feed(std::span{wire}.last(1));
    const auto result = decoder.next();
    // Then: exactly one complete payload is available.
    ASSERT_TRUE(std::holds_alternative<AawRecord>(result));
    EXPECT_EQ(std::get<AawRecord>(result).payload, (std::vector<std::uint8_t>{9, 8, 7}));
}

TEST(AawFrame, SplitsWhenThreeRecordsAreCoalesced) {
    // Given: three records in one transport chunk.
    const std::array<std::uint8_t, 15> wire{0, 1, 0, 1, 9, 0, 1, 0, 2, 8, 0, 1, 0, 3, 7};
    AawDecoder decoder;
    // When: the chunk arrives and EOF is signalled before draining.
    decoder.feed(wire);
    EXPECT_FALSE(decoder.finish());
    // Then: all three records remain available in order.
    for (std::uint16_t id = 1; id <= 3; ++id) {
        const auto result = decoder.next();
        ASSERT_TRUE(std::holds_alternative<AawRecord>(result));
        EXPECT_EQ(std::get<AawRecord>(result).id, id);
        EXPECT_EQ(std::get<AawRecord>(result).payload,
                  (std::vector<std::uint8_t>{static_cast<std::uint8_t>(10 - id)}));
    }
    EXPECT_TRUE(std::holds_alternative<std::monostate>(decoder.next()));
}

TEST(AawFrame, WaitsWhenHeaderIsSplitMidField) {
    // Given: the ID field is split between chunks.
    const std::array<std::uint8_t, 5> wire{0, 1, 0, 7, 42};
    AawDecoder decoder;
    decoder.feed(std::span{wire}.first(3));
    EXPECT_TRUE(std::holds_alternative<std::monostate>(decoder.next()));
    // When: the rest arrives.
    decoder.feed(std::span{wire}.subspan(3));
    const auto result = decoder.next();
    // Then: the ID and payload decode correctly.
    ASSERT_TRUE(std::holds_alternative<AawRecord>(result));
    EXPECT_EQ(std::get<AawRecord>(result).id, 7);
    EXPECT_EQ(std::get<AawRecord>(result).payload, (std::vector<std::uint8_t>{42}));
}

class PayloadSize : public ::testing::TestWithParam<std::size_t> {};
TEST_P(PayloadSize, RoundTripsWhenWithinLimit) {
    // Given: an empty or maximum-size payload.
    const std::vector<std::uint8_t> payload(GetParam(), 0xa5);
    const auto wire = encode_aaw(3, payload);
    ASSERT_TRUE(wire);
    EXPECT_EQ(wire->size(), payload.size() + 4);
    if (!payload.empty()) {
        EXPECT_EQ((*wire)[0], 0x10);
        EXPECT_EQ((*wire)[1], 0);
    }
    AawDecoder decoder;
    // When: the frame arrives.
    decoder.feed(*wire);
    const auto result = decoder.next();
    // Then: all payload bytes survive.
    ASSERT_TRUE(std::holds_alternative<AawRecord>(result));
    EXPECT_EQ(std::get<AawRecord>(result).payload, payload);
}
INSTANTIATE_TEST_SUITE_P(Boundaries, PayloadSize, ::testing::Values(0U, 4096U));

TEST(AawFrame, RejectsWhenPayloadExceedsLimit) {
    // Given: a payload and header declaring 4097 bytes.
    const std::vector<std::uint8_t> payload(4097, 0);
    const std::array<std::uint8_t, 4> header{0x10, 1, 0, 1};
    AawDecoder decoder;
    // When: encoding is attempted and the header alone is decoded.
    const auto encoded = encode_aaw(1, payload);
    decoder.feed(header);
    const auto result = decoder.next();
    // Then: both reject without waiting for the oversized payload.
    EXPECT_FALSE(encoded);
    ASSERT_TRUE(std::holds_alternative<AawError>(result));
    EXPECT_EQ(std::get<AawError>(result), AawError::oversize);
}

class UnknownId : public ::testing::TestWithParam<std::uint16_t> {};
TEST_P(UnknownId, RejectsWhenIdIsUnknown) {
    // Given: an unsupported ID.
    const std::array<std::uint8_t, 4> header{0, 0,
        static_cast<std::uint8_t>(GetParam() >> 8), static_cast<std::uint8_t>(GetParam() & 0xff)};
    AawDecoder decoder;
    // When: encoding and decoding are attempted.
    EXPECT_FALSE(encode_aaw(GetParam(), {}));
    decoder.feed(header);
    const auto result = decoder.next();
    // Then: decoding reports the typed error.
    ASSERT_TRUE(std::holds_alternative<AawError>(result));
    EXPECT_EQ(std::get<AawError>(result), AawError::unknown_id);
}
INSTANTIATE_TEST_SUITE_P(Unsupported, UnknownId, ::testing::Values(0, 8, 256, 65535));

TEST(AawFrame, RemainsFailedWhenMoreDataArrivesAfterError) {
    // Given: an invalid record has failed decoding.
    AawDecoder decoder;
    decoder.feed(std::array<std::uint8_t, 4>{0, 0, 0, 8});
    ASSERT_EQ(std::get<AawError>(decoder.next()), AawError::unknown_id);
    // When: a valid frame arrives afterward.
    decoder.feed(std::array<std::uint8_t, 4>{0, 0, 0, 1});
    // Then: next and EOF preserve the original failure.
    EXPECT_EQ(std::get<AawError>(decoder.next()), AawError::unknown_id);
    EXPECT_EQ(decoder.finish(), AawError::unknown_id);
}

class PartialRecord : public ::testing::TestWithParam<std::size_t> {};
TEST_P(PartialRecord, ReportsTruncatedWhenEofInterruptsRecord) {
    // Given: a partial header or payload, optionally after a complete record.
    const std::array<std::uint8_t, 7> wire{0, 3, 0, 1, 1, 2, 3};
    AawDecoder decoder;
    decoder.feed(std::array<std::uint8_t, 4>{0, 0, 0, 2});
    decoder.feed(std::span{wire}.first(GetParam()));
    // When: EOF is signalled.
    const auto error = decoder.finish();
    // Then: truncation is typed and sticky.
    EXPECT_EQ(error, AawError::truncated);
    EXPECT_EQ(std::get<AawError>(decoder.next()), AawError::truncated);
    decoder.feed(wire);
    EXPECT_EQ(decoder.finish(), AawError::truncated);
}
INSTANTIATE_TEST_SUITE_P(CutPoints, PartialRecord, ::testing::Values(1U, 2U, 3U, 4U, 5U, 6U));
}
}
