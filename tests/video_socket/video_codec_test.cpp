// SPDX-License-Identifier: GPL-3.0-or-later
#include <aa/ipc/VideoSocket.hpp>

#include <array>
#include <cstdint>
#include <vector>

#include <gtest/gtest.h>

namespace {
using namespace aa::ipc;

VideoFrameHeader sample_header() {
    return VideoFrameHeader{0x123456789abcdef0ULL, 7, 123456789ULL, 0, 1, 100, 100, true};
}

std::vector<std::uint8_t> encode(const VideoFrameHeader& header) {
    std::vector<std::uint8_t> record(kFrameHeaderBytes + header.payload_bytes, 0x5a);
    encode_frame_header(record, header);
    return record;
}
} // namespace

TEST(VideoCodec, WireRoundTrip) {
    const auto input = sample_header();
    const auto record = encode(input);
    const auto output = decode_frame_header(record, NegotiatedLimits{});
    ASSERT_TRUE(output);
    EXPECT_EQ(output.value().frame, input.frame);
    EXPECT_EQ(output.value().sequence, input.sequence);
    EXPECT_EQ(output.value().timestamp_ns, input.timestamp_ns);
    EXPECT_TRUE(output.value().idr);
    EXPECT_EQ(record[0], 0x41);
    EXPECT_EQ(record[1], 0x41);
    EXPECT_EQ(record[2], 0x56);
    EXPECT_EQ(record[3], 0x31);
    EXPECT_EQ(record[6], 0x00);
    EXPECT_EQ(record[7], static_cast<std::uint8_t>(kFrameHeaderBytes));
}

TEST(VideoCodec, RejectsHeaderMutations) {
    const auto good = encode(VideoFrameHeader{1, 1, 2, 0, 1, 100, 100, true});
    for (const std::size_t offset : {std::size_t{0}, std::size_t{6}, std::size_t{8}, std::size_t{52}}) {
        auto bad = good;
        bad[offset] ^= 0x80;
        const auto out = decode_frame_header(bad, NegotiatedLimits{});
        ASSERT_FALSE(out) << offset;
        EXPECT_EQ(out.error().code(), aa::ErrorCode::transport_malformed_frame) << offset;
    }
    auto version = good;
    version[5] = 99;
    const auto version_out = decode_frame_header(version, NegotiatedLimits{});
    ASSERT_FALSE(version_out);
    EXPECT_EQ(version_out.error().code(), aa::ErrorCode::protocol_version_mismatch);
}

TEST(VideoCodec, RejectsLengthAndFragmentBounds) {
    const NegotiatedLimits limits{};
    const VideoFrameHeader base{1, 1, 2, 0, 1, 100, 100, true};
    for (const auto bytes : {static_cast<std::uint32_t>(kMaxAccessUnitBytes + 1), UINT32_MAX}) {
        auto bad = base;
        bad.frame_bytes = bytes;
        const auto out = decode_frame_header(encode(bad), limits);
        ASSERT_FALSE(out);
        EXPECT_EQ(out.error().code(), aa::ErrorCode::transport_oversize_frame);
    }
    auto zero = base;
    zero.frame_bytes = 0;
    const auto zero_out = decode_frame_header(encode(zero), limits);
    ASSERT_FALSE(zero_out);
    EXPECT_EQ(zero_out.error().code(), aa::ErrorCode::transport_malformed_frame);
    for (const auto count : {0U, 2U, UINT32_MAX}) {
        auto bad = base;
        bad.count = count;
        const auto out = decode_frame_header(encode(bad), limits);
        ASSERT_FALSE(out);
        EXPECT_EQ(out.error().code(), aa::ErrorCode::transport_malformed_frame);
    }
    auto index = base;
    index.index = UINT32_MAX;
    const auto index_out = decode_frame_header(encode(index), limits);
    ASSERT_FALSE(index_out);
    EXPECT_EQ(index_out.error().code(), aa::ErrorCode::transport_malformed_frame);
    auto payload = base;
    payload.payload_bytes = static_cast<std::uint32_t>(kMaxDatagramPayload + 1);
    std::vector<std::uint8_t> oversize_record(kFrameHeaderBytes + kMaxDatagramPayload + 1, 0);
    encode_frame_header(oversize_record, payload);
    const auto out = decode_frame_header(oversize_record, limits);
    ASSERT_FALSE(out);
    EXPECT_EQ(out.error().code(), aa::ErrorCode::transport_oversize_frame);
    std::vector<std::uint8_t> short_record(kFrameHeaderBytes - 1, 0);
    const auto short_out = decode_frame_header(short_record, limits);
    ASSERT_FALSE(short_out);
    EXPECT_EQ(short_out.error().code(), aa::ErrorCode::transport_malformed_frame);
}

TEST(VideoCodec, NegotiatedLimitShrinksFragmentGrammar) {
    NegotiatedLimits tight{};
    tight.max_datagram_payload = 16;
    tight.max_access_unit_bytes = 64;
    VideoFrameHeader header{1, 1, 2, 0, 4, 64, 16, true};
    ASSERT_TRUE(decode_frame_header(encode(header), tight));
    header.frame_bytes = 65;
    header.payload_bytes = 16;
    const auto out = decode_frame_header(encode(header), tight);
    ASSERT_FALSE(out);
    EXPECT_EQ(out.error().code(), aa::ErrorCode::transport_oversize_frame);
}

TEST(VideoCodec, LimitsRecordRoundTrip) {
    NegotiatedLimits offered{};
    offered.max_datagram_payload = 4096;
    offered.max_access_unit_bytes = 65536;
    offered.socket_budget_bytes = 8192;
    std::array<std::uint8_t, kLimitsRecordBytes> record{};
    encode_limits_record(record, offered);
    const auto decoded = decode_limits_record(record);
    ASSERT_TRUE(decoded);
    EXPECT_EQ(decoded.value(), offered);
    record[24] = 1;
    const auto bad = decode_limits_record(record);
    ASSERT_FALSE(bad);
    EXPECT_EQ(bad.error().code(), aa::ErrorCode::transport_malformed_frame);
}

TEST(VideoCodec, LimitsRecordRejectsOutOfRangeOffer) {
    NegotiatedLimits over{};
    over.max_datagram_payload = static_cast<std::uint32_t>(kMaxDatagramPayload + 1);
    std::array<std::uint8_t, kLimitsRecordBytes> record{};
    encode_limits_record(record, over);
    const auto out = decode_limits_record(record);
    ASSERT_FALSE(out);
    EXPECT_EQ(out.error().code(), aa::ErrorCode::protocol_negotiation_failed);
}

TEST(VideoCodec, NegotiateTakesFieldMinimum) {
    NegotiatedLimits a{4096, 65536, 8192};
    NegotiatedLimits b{1024, 32768, 16384};
    const auto agreed = negotiate_limits(a, b);
    ASSERT_TRUE(agreed);
    EXPECT_EQ(agreed.value().max_datagram_payload, 1024U);
    EXPECT_EQ(agreed.value().max_access_unit_bytes, 32768U);
    EXPECT_EQ(agreed.value().socket_budget_bytes, 8192U);
}

TEST(VideoCodec, AuthorizePeerIsSameUidOnly) {
    const PeerCredentials peer{1000, 42};
    EXPECT_TRUE(authorize_peer(peer, 1000));
    const auto rejected = authorize_peer(peer, 1001);
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().code(), aa::ErrorCode::transport_unauthorized_peer);
}
