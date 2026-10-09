// SPDX-License-Identifier: GPL-3.0-or-later
// Malformed-input handling and fuzz smoke (task 16): every deterministic
// malformed length, type and reassembly failure is a typed error that closes the
// stream safely, and 10000 malformed frames must not crash (verified under ASAN).

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

// A minimal valid bulk frame on channel 1 (header + short size + one payload byte).
std::vector<std::byte> good_bulk() {
    return bytes({0x01, 0x03, 0x00, 0x01, 0xAA});
}

// xorshift64: a deterministic PRNG so the fuzz smoke is reproducible.
class Rng final {
public:
    explicit Rng(std::uint64_t seed) : state_(seed ? seed : 0x9E3779B97F4A7C15ull) {}
    std::uint64_t next() {
        state_ ^= state_ << 13;
        state_ ^= state_ >> 7;
        state_ ^= state_ << 17;
        return state_;
    }
private:
    std::uint64_t state_;
};

} // namespace

TEST(Malformed, RejectsReservedFlagBits) {
    // Reserved flags bits 4-7 are not part of the wire layout: fail closed.
    auto decoded = tr::decode_frame(bytes({0x01, 0xF3, 0x00, 0x01, 0xAA}));
    ASSERT_FALSE(decoded.has_value());
    EXPECT_EQ(decoded.error().code(), aa::ErrorCode::transport_malformed_frame);
}

TEST(Malformed, RejectsShortBuffer) {
    auto decoded = tr::decode_frame(bytes({0x01}));
    ASSERT_FALSE(decoded.has_value());
    EXPECT_EQ(decoded.error().code(), aa::ErrorCode::transport_malformed_frame);
}

TEST(Malformed, RejectsSizeFieldThatLies) {
    // Size claims 4 payload bytes but only 1 is present.
    auto decoded = tr::decode_frame(bytes({0x01, 0x03, 0x00, 0x04, 0xAA}));
    ASSERT_FALSE(decoded.has_value());
    EXPECT_EQ(decoded.error().code(), aa::ErrorCode::transport_malformed_frame);
}

TEST(Malformed, RejectsOversizeFramePayload) {
    // A plain payload above the chunk cap is oversize (size 0x4001 = 16385).
    auto decoded = tr::decode_frame(bytes({0x01, 0x03, 0x40, 0x01}));
    ASSERT_FALSE(decoded.has_value());
    EXPECT_EQ(decoded.error().code(), aa::ErrorCode::transport_oversize_frame);
}

TEST(Malformed, RejectsOversizeFirstTotal) {
    // FIRST total above the message cap is oversize.
    auto frame = bytes({0x01, 0x01, 0x00, 0x01, 0xFF, 0xFF, 0xFF, 0xFF, 0xAA});
    auto decoded = tr::decode_frame(frame);
    ASSERT_FALSE(decoded.has_value());
    EXPECT_EQ(decoded.error().code(), aa::ErrorCode::transport_oversize_frame);
}

TEST(Malformed, ReassemblerRejectsMiddleWithoutFirst) {
    tr::MessageReassembler reassembler(nullptr);
    // A MIDDLE frame with no in-progress message on the channel is intertwined.
    auto middle = bytes({0x01, 0x00, 0x00, 0x01, 0xAA});
    auto pushed = reassembler.push(middle);
    ASSERT_FALSE(pushed.has_value());
    EXPECT_EQ(pushed.error().code(), aa::ErrorCode::transport_malformed_frame);
    EXPECT_TRUE(reassembler.closed());
}

TEST(Malformed, ReassemblerRejectsLastWithoutFirst) {
    tr::MessageReassembler reassembler(nullptr);
    auto last = bytes({0x01, 0x02, 0x00, 0x01, 0xAA});
    auto pushed = reassembler.push(last);
    ASSERT_FALSE(pushed.has_value());
    EXPECT_EQ(pushed.error().code(), aa::ErrorCode::transport_malformed_frame);
}

TEST(Malformed, ReassemblerRejectsFirstWithZeroTotal) {
    tr::MessageReassembler reassembler(nullptr);
    // A zero-length message is a BULK frame, never a FIRST.
    auto first = bytes({0x01, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0xAA});
    auto pushed = reassembler.push(first);
    ASSERT_FALSE(pushed.has_value());
    EXPECT_EQ(pushed.error().code(), aa::ErrorCode::transport_malformed_frame);
}

TEST(Malformed, ReassemblerRejectsTotalMismatch) {
    tr::MessageReassembler reassembler(nullptr);
    // FIRST advertises a 4-byte total but the reassembled plaintext is 2 bytes.
    auto first = bytes({0x01, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00, 0x04, 0xAA});
    auto last = bytes({0x01, 0x02, 0x00, 0x01, 0xBB});
    auto r1 = reassembler.push(first);
    ASSERT_TRUE(r1.has_value());
    auto r2 = reassembler.push(last);
    ASSERT_FALSE(r2.has_value());
    EXPECT_EQ(r2.error().code(), aa::ErrorCode::transport_malformed_frame);
}

TEST(Malformed, ReassemblerRejectsKindChangeMidMessage) {
    tr::MessageReassembler reassembler(nullptr);
    // The FIRST is a specific message; a control LAST must not complete it.
    auto first = bytes({0x01, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00, 0x02, 0xAA});
    auto last = bytes({0x01, 0x06, 0x00, 0x01, 0xBB}); // control + last
    auto r1 = reassembler.push(first);
    ASSERT_TRUE(r1.has_value());
    auto r2 = reassembler.push(last);
    ASSERT_FALSE(r2.has_value());
    EXPECT_EQ(r2.error().code(), aa::ErrorCode::transport_malformed_frame);
}

TEST(Malformed, ReassemblerRejectsOverrunBeforeTotal) {
    tr::MessageReassembler reassembler(nullptr);
    // FIRST total is 2 but the first chunk already holds 2, then MIDDLE overruns.
    auto first = bytes({0x01, 0x01, 0x00, 0x02, 0x00, 0x00, 0x00, 0x02, 0xAA, 0xBB});
    auto middle = bytes({0x01, 0x00, 0x00, 0x01, 0xCC});
    auto r1 = reassembler.push(first);
    ASSERT_TRUE(r1.has_value());
    auto r2 = reassembler.push(middle);
    ASSERT_FALSE(r2.has_value());
    EXPECT_EQ(r2.error().code(), aa::ErrorCode::transport_malformed_frame);
}

TEST(Malformed, SafeCloseAfterError) {
    tr::MessageReassembler reassembler(nullptr);
    auto bad = bytes({0x01, 0x00, 0x00, 0x01, 0xAA}); // MIDDLE without FIRST
    auto pushed = reassembler.push(bad);
    ASSERT_FALSE(pushed.has_value());
    ASSERT_TRUE(reassembler.closed());
    // Every later push re-returns transport_closed; the channel is safe to close.
    auto again = reassembler.push(good_bulk());
    ASSERT_FALSE(again.has_value());
    EXPECT_EQ(again.error().code(), aa::ErrorCode::transport_closed);

    tr::FrameStreamParser parser;
    auto fed = parser.feed(bytes({0x01, 0xF3, 0x00, 0x01, 0xAA}));
    ASSERT_FALSE(fed.has_value());
    ASSERT_TRUE(parser.closed());
    auto more = parser.feed(good_bulk());
    ASSERT_FALSE(more.has_value());
    EXPECT_EQ(more.error().code(), aa::ErrorCode::transport_closed);
}

TEST(Malformed, ReassemblerRejectsSameChannelFirstInterruption) {
    // Given: a fragmented message already in progress on channel 1.
    tr::MessageReassembler reassembler(nullptr);
    auto first = bytes({0x01, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00, 0x02, 0xAA});
    ASSERT_TRUE(reassembler.push(first).has_value());
    // When: another FIRST arrives on the same channel before the first completes.
    auto interrupt = bytes({0x01, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00, 0x02, 0xBB});
    auto pushed = reassembler.push(interrupt);
    // Then: it is an invalid interruption, not a silent override.
    ASSERT_FALSE(pushed.has_value());
    EXPECT_EQ(pushed.error().code(), aa::ErrorCode::transport_malformed_frame);
    EXPECT_TRUE(reassembler.closed());
}

TEST(Malformed, ReassemblerRejectsSameChannelBulkInterruption) {
    // Given: a fragmented message already in progress on channel 1.
    tr::MessageReassembler reassembler(nullptr);
    auto first = bytes({0x01, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00, 0x02, 0xAA});
    ASSERT_TRUE(reassembler.push(first).has_value());
    // When: a BULK frame arrives on the same channel mid-message.
    auto bulk = bytes({0x01, 0x03, 0x00, 0x01, 0xBB});
    auto pushed = reassembler.push(bulk);
    // Then: it is an invalid interruption and fails closed.
    ASSERT_FALSE(pushed.has_value());
    EXPECT_EQ(pushed.error().code(), aa::ErrorCode::transport_malformed_frame);
}

TEST(Malformed, ParserBoundsReturnedQueueForLargeInput) {
    // Given: a large stream of valid BULK frames (well past the queue bound).
    std::vector<std::byte> stream;
    const std::vector<std::byte> one = bytes({0x01, 0x03, 0x00, 0x64});
    stream.reserve(std::size_t{21} * 1024u * 1024u);
    while (stream.size() < std::size_t{20} * 1024u * 1024u) {
        stream.insert(stream.end(), one.begin(), one.end());
        stream.resize(stream.size() + 100u, std::byte{0xAB});
    }
    // When: the parser is fed the large input in one call.
    tr::FrameStreamParser parser;
    auto frames = parser.feed(stream);
    // Then: the returned queue is bounded (or a typed queue-full), never unbounded.
    if (frames.has_value()) {
        std::size_t total = 0;
        for (const auto& frame : frames.value()) {
            total += frame.size();
        }
        EXPECT_LE(total, tr::kMaxQueuedBytes);
    } else {
        EXPECT_EQ(frames.error().code(), aa::ErrorCode::transport_queue_full);
    }
}

TEST(FuzzSmoke, TenThousandMalformedFramesDoNotCrash) {
    // Deterministic pseudo-random malformed input across decode, the stream
    // parser and the reassembler. Under ASAN any out-of-bounds read/write or
    // use-after-free aborts here; surviving is the proof.
    Rng rng(0x1234567887654321ull);
    tr::MessageReassembler reassembler(nullptr);
    tr::FrameStreamParser parser;
    for (int i = 0; i < 10000; ++i) {
        const std::size_t size = 1u + static_cast<std::size_t>(rng.next() % 40u);
        std::vector<std::byte> buffer(size);
        for (std::size_t j = 0; j < size; ++j) {
            buffer[j] = static_cast<std::byte>(rng.next() & 0xFFu);
        }
        // Byte-level decode: must always return a value or a typed error.
        auto decoded = tr::decode_frame(buffer);
        (void)decoded;
        // Stream parser: feed the malformed chunk; ignore the outcome.
        if (!parser.closed()) {
            auto fed = parser.feed(buffer);
            (void)fed;
        }
        // Reassembler: push the malformed frame; ignore the outcome.
        if (!reassembler.closed()) {
            auto pushed = reassembler.push(buffer);
            (void)pushed;
        }
        // Reset the reassembler periodically so many shapes are exercised.
        if (i % 97 == 0) {
            reassembler = tr::MessageReassembler(nullptr);
            parser = tr::FrameStreamParser();
        }
    }
    SUCCEED();
}
