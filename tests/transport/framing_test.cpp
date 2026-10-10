// SPDX-License-Identifier: GPL-3.0-or-later
// AAP frame and message framing (task 16): known wire layout, safe big-endian
// round trips, fragmentation/reassembly coherence, dynamic channel bytes and
// memory bounds.

#include <aa/transport/Framing.hpp>
#include <aa/transport/Frames.hpp>

#include <cstddef>
#include <cstdint>
#include <vector>

#include <gtest/gtest.h>

namespace {

namespace tr = aa::transport;
using tr::FrameType;
using tr::MessageKind;
using tr::Encryption;

std::vector<std::byte> bytes(std::initializer_list<unsigned> values) {
    std::vector<std::byte> out;
    out.reserve(values.size());
    for (unsigned value : values) {
        out.push_back(static_cast<std::byte>(value));
    }
    return out;
}

std::vector<std::byte> pattern(std::size_t size) {
    std::vector<std::byte> out(size);
    for (std::size_t i = 0; i < size; ++i) {
        out[i] = static_cast<std::byte>((i * 31u) & 0xFFu);
    }
    return out;
}

// Round-trip one message through encode_message -> decode_frame -> reassembler.
void roundtrip(const tr::Message& message, Encryption encryption) {
    auto frames = tr::encode_message(message, encryption, nullptr);
    ASSERT_TRUE(frames.has_value()) << "encode failed";
    tr::MessageReassembler reassembler(nullptr);
    std::optional<tr::Message> out;
    for (const auto& frame : frames.value()) {
        auto pushed = reassembler.push(frame);
        ASSERT_TRUE(pushed.has_value()) << "push failed";
        if (pushed.value().has_value()) {
            out = pushed.value();
        }
    }
    ASSERT_TRUE(out.has_value()) << "message did not complete";
    EXPECT_EQ(out.value(), message);
}

} // namespace

TEST(Framing, EncodesKnownBulkLayout) {
    // Given: a control bulk frame on channel 5 with a two-byte payload.
    const tr::FrameHeader header{0x05, FrameType::bulk, MessageKind::control, Encryption::plain};
    const auto payload = bytes({0xAA, 0xBB});
    // When: it is serialized.
    auto encoded = tr::encode_frame(header, payload, 0);
    ASSERT_TRUE(encoded.has_value());
    // Then: the wire layout is channel + flags + short size + payload.
    EXPECT_EQ(encoded.value(), bytes({0x05, 0x07, 0x00, 0x02, 0xAA, 0xBB}));
}

TEST(Framing, EncodesKnownFirstExtendedLayout) {
    // Given: a first frame whose extended size carries a 3-byte total.
    const tr::FrameHeader header{0x05, FrameType::first, MessageKind::specific, Encryption::plain};
    const auto payload = bytes({0xAA});
    // When: it is serialized.
    auto encoded = tr::encode_frame(header, payload, 3);
    ASSERT_TRUE(encoded.has_value());
    // Then: the size field is 2-byte current + 4-byte total big-endian.
    EXPECT_EQ(encoded.value(), bytes({0x05, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00, 0x03, 0xAA}));
}

TEST(Framing, RoundTripsEveryFlagCombination) {
    for (auto type : {FrameType::middle, FrameType::first, FrameType::last, FrameType::bulk}) {
        for (auto kind : {MessageKind::specific, MessageKind::control}) {
            for (auto encryption : {Encryption::plain, Encryption::encrypted}) {
                const tr::FrameHeader header{0x42, type, kind, encryption};
                const auto payload = bytes({0x10, 0x20, 0x30});
                auto encoded = tr::encode_frame(header, payload, 7);
                ASSERT_TRUE(encoded.has_value());
                auto decoded = tr::decode_frame(encoded.value());
                ASSERT_TRUE(decoded.has_value());
                EXPECT_EQ(decoded.value().header, header);
                EXPECT_EQ(decoded.value().payload, payload);
                EXPECT_EQ(decoded.value().total_message_bytes,
                          type == FrameType::first ? 7u : 0u);
            }
        }
    }
}

TEST(Framing, PreservesDynamicChannelBytes) {
    // The channel is a raw wire byte; dynamic service channels must survive.
    for (std::uint8_t channel : {std::uint8_t{0}, std::uint8_t{1}, std::uint8_t{128},
                                 std::uint8_t{200}, std::uint8_t{255}}) {
        const tr::FrameHeader header{channel, FrameType::bulk, MessageKind::specific, Encryption::plain};
        auto encoded = tr::encode_frame(header, bytes({0x01}), 0);
        ASSERT_TRUE(encoded.has_value());
        auto decoded = tr::decode_frame(encoded.value());
        ASSERT_TRUE(decoded.has_value());
        EXPECT_EQ(decoded.value().header.channel, channel);
    }
}

TEST(Framing, ReassemblesSingleBulkMessage) {
    const tr::Message message{0x11, MessageKind::specific, pattern(100)};
    roundtrip(message, Encryption::plain);
}

TEST(Framing, ReassemblesEmptyMessage) {
    const tr::Message message{0x11, MessageKind::control, {}};
    roundtrip(message, Encryption::plain);
}

TEST(Framing, ReassemblesFragmentedMessageAcrossChunks) {
    // Larger than the plaintext chunk cap forces FIRST + MIDDLE* + LAST.
    const tr::Message message{0x33, MessageKind::specific, pattern(tr::kPlainChunkBytes * 2 + 5)};
    roundtrip(message, Encryption::plain);
}

TEST(Framing, ReassemblesExactlyOneChunkBoundary) {
    const tr::Message message{0x33, MessageKind::specific, pattern(tr::kPlainChunkBytes + 1)};
    roundtrip(message, Encryption::plain);
}

TEST(Framing, ReassemblesIntertwinedChannels) {
    // Two channels fragment concurrently; each reassembles independently.
    const tr::Message a{0x01, MessageKind::specific, pattern(tr::kPlainChunkBytes + 10)};
    const tr::Message b{0x02, MessageKind::specific, pattern(tr::kPlainChunkBytes + 20)};
    auto fa = tr::encode_message(a, Encryption::plain, nullptr);
    auto fb = tr::encode_message(b, Encryption::plain, nullptr);
    ASSERT_TRUE(fa.has_value() && fb.has_value());
    tr::MessageReassembler reassembler(nullptr);
    std::optional<tr::Message> got_a;
    std::optional<tr::Message> got_b;
    // Interleave the two channels' frames.
    std::size_t ia = 0;
    std::size_t ib = 0;
    while (ia < fa.value().size() || ib < fb.value().size()) {
        if (ia < fa.value().size()) {
            auto r = reassembler.push(fa.value()[ia++]);
            ASSERT_TRUE(r.has_value());
            if (r.value()) {
                got_a = r.value();
            }
        }
        if (ib < fb.value().size()) {
            auto r = reassembler.push(fb.value()[ib++]);
            ASSERT_TRUE(r.has_value());
            if (r.value()) {
                got_b = r.value();
            }
        }
    }
    ASSERT_TRUE(got_a.has_value() && got_b.has_value());
    EXPECT_EQ(got_a.value(), a);
    EXPECT_EQ(got_b.value(), b);
}

TEST(Framing, RejectsOversizeMessage) {
    const tr::Message message{0x01, MessageKind::specific, pattern(tr::kMaxMessageBytes + 1)};
    auto frames = tr::encode_message(message, Encryption::plain, nullptr);
    ASSERT_FALSE(frames.has_value());
    EXPECT_EQ(frames.error().code(), aa::ErrorCode::transport_oversize_frame);
}

TEST(Framing, ParserSplitsByteStreamIntoFrames) {
    const tr::FrameHeader header{0x07, FrameType::bulk, MessageKind::specific, Encryption::plain};
    auto one = tr::encode_frame(header, bytes({0x01, 0x02}), 0);
    auto two = tr::encode_frame(header, bytes({0x03}), 0);
    ASSERT_TRUE(one.has_value() && two.has_value());
    std::vector<std::byte> stream;
    stream.insert(stream.end(), one.value().begin(), one.value().end());
    stream.insert(stream.end(), two.value().begin(), two.value().end());

    tr::FrameStreamParser parser;
    // Feed the whole stream in one chunk; both frames come out.
    auto frames = parser.feed(stream);
    ASSERT_TRUE(frames.has_value());
    ASSERT_EQ(frames.value().size(), 2u);
}

TEST(Framing, ParserHandlesPartialChunks) {
    const tr::FrameHeader header{0x07, FrameType::bulk, MessageKind::specific, Encryption::plain};
    auto one = tr::encode_frame(header, bytes({0x01, 0x02, 0x03, 0x04}), 0);
    ASSERT_TRUE(one.has_value());
    tr::FrameStreamParser parser;
    std::vector<std::vector<std::byte>> collected;
    // Feed one byte at a time; the frame completes on the last byte.
    for (std::size_t i = 0; i < one.value().size(); ++i) {
        auto frames = parser.feed(std::span<const std::byte>(&one.value()[i], 1));
        ASSERT_TRUE(frames.has_value());
        for (auto& frame : frames.value()) {
            collected.push_back(std::move(frame));
        }
    }
    ASSERT_EQ(collected.size(), 1u);
    EXPECT_EQ(collected[0], one.value());
}
