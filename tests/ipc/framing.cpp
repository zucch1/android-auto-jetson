// SPDX-License-Identifier: GPL-3.0-or-later
#include "transport.hpp"
#include <gtest/gtest.h>
#include <atomic>
#include <cstdlib>
#include <memory>
#include <new>
#include <vector>
#include <sys/socket.h>

using namespace aa::ipc;
namespace {
// Count heap allocations so the malformed-flood regression enforces bounded, non-retained
// storage instead of only checking rejection counters. The counters/delete are harmless for
// every other test in this binary.
std::atomic<long long> allocations{0};
long long allocation_count() { return allocations.load(std::memory_order_relaxed); }
}
void* operator new(std::size_t size) {
    allocations.fetch_add(1, std::memory_order_relaxed);
    void* pointer = std::malloc(size ? size : 1);
    if (!pointer) throw std::bad_alloc();
    return pointer;
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void* operator new(std::size_t size, const std::nothrow_t&) noexcept { return ::operator new(size); }
void* operator new[](std::size_t size, const std::nothrow_t&) noexcept { return ::operator new(size); }
void operator delete(void* pointer) noexcept { std::free(pointer); }
void operator delete[](void* pointer) noexcept { std::free(pointer); }
void operator delete(void* pointer, std::size_t) noexcept { std::free(pointer); }
void operator delete[](void* pointer, std::size_t) noexcept { std::free(pointer); }
void operator delete(void* pointer, const std::nothrow_t&) noexcept { std::free(pointer); }
void operator delete[](void* pointer, const std::nothrow_t&) noexcept { std::free(pointer); }
namespace {
Packet make_packet(Header h) {
    Packet packet{}; encode(packet, h);
    std::fill(packet.begin() + header_size, packet.end(), 0x5a);
    return packet;
}
Receive feed(Reassembler& receiver, Header h) {
    const auto packet = make_packet(h);
    return receiver.accept({packet.data(), header_size + h.payload_bytes});
}
}
TEST(IpcFraming, WireRoundTrip) {
    const Header input{0x123456789abcdef0, 0xfedcba9876543210, 123456789, 0, 1, 100, 100, true};
    auto packet = make_packet(input); Header output;
    ASSERT_TRUE(decode({packet.data(), header_size + 100}, output));
    EXPECT_EQ(output.frame, input.frame); EXPECT_EQ(output.sequence, input.sequence);
    EXPECT_EQ(output.timestamp_ns, input.timestamp_ns); EXPECT_TRUE(output.idr);
    EXPECT_EQ(packet[0], 0x41); EXPECT_EQ(packet[4], 0); EXPECT_EQ(packet[5], 1);
}
TEST(IpcFraming, RejectHeaderMutations) {
    const auto good = make_packet({1, 1, 2, 0, 1, 100, 100, true});
    Header header;
    for (const auto offset : {0, 4, 6, 8, 52}) {
        auto bad = good; bad[offset] ^= 0x80;
        EXPECT_FALSE(decode({bad.data(), header_size + 100}, header)) << offset;
    }
    EXPECT_FALSE(decode({}, header));
    EXPECT_FALSE(decode({good.data(), header_size - 1}, header));
    EXPECT_FALSE(decode({good.data(), header_size + 99}, header));
    EXPECT_FALSE(decode({good.data(), header_size + 101}, header));
}
TEST(IpcFraming, RejectLengthAndFragmentBounds) {
    Header h{1, 1, 2, 0, 1, 100, 100, true}; Header out;
    for (const auto bytes : {0u, static_cast<unsigned>(frame_limit + 1), UINT32_MAX}) {
        auto bad = h; bad.frame_bytes = bytes; auto packet = make_packet(bad);
        EXPECT_FALSE(decode({packet.data(), header_size + 100}, out));
    }
    for (const auto count : {0u, 2u, UINT32_MAX}) {
        auto bad = h; bad.count = count; auto packet = make_packet(bad);
        EXPECT_FALSE(decode({packet.data(), header_size + 100}, out));
    }
    auto bad = h; bad.index = UINT32_MAX; auto packet = make_packet(bad);
    EXPECT_FALSE(decode({packet.data(), header_size + 100}, out));
    bad = h; bad.payload_bytes = payload_limit + 1; packet = make_packet(bad);
    EXPECT_FALSE(decode(packet, out));
}
TEST(IpcFraming, ReassembleMaximumAndRaggedFrames) {
    auto receiver = std::make_unique<Reassembler>();
    std::uint64_t sequence = 0, id = 0;
    for (const auto size : {payload_limit, payload_limit + 7, frame_limit}) {
        const auto count = static_cast<std::uint32_t>((size + payload_limit - 1) / payload_limit);
        for (std::uint32_t i = 0; i < count; ++i) {
            const auto length = static_cast<std::uint32_t>(std::min(payload_limit, size - i * payload_limit));
            EXPECT_EQ(feed(*receiver, {id, sequence++, 42, i, count, static_cast<std::uint32_t>(size), length, true}),
                      i + 1 == count ? Receive::complete : Receive::fragment);
        }
        EXPECT_EQ(receiver->frame().size(), size);
        EXPECT_TRUE(std::all_of(receiver->frame().begin(), receiver->frame().end(), [](auto byte) { return byte == 0x5a; }));
        ++id;
    }
    EXPECT_LE(sizeof(Reassembler), frame_limit + 256);
}
TEST(IpcFraming, LossRequiresCompleteIdr) {
    auto receiver = std::make_unique<Reassembler>();
    EXPECT_EQ(feed(*receiver, {1, 0, 42, 0, 1, 4, 4, false}), Receive::waiting);
    EXPECT_EQ(feed(*receiver, {2, 1, 42, 0, 2, payload_limit + 4, payload_limit, true}), Receive::fragment);
    EXPECT_EQ(feed(*receiver, {2, 3, 42, 1, 2, payload_limit + 4, 4, true}), Receive::waiting);
    EXPECT_TRUE(receiver->waiting_idr()); EXPECT_EQ(receiver->resyncs, 0);
    EXPECT_EQ(feed(*receiver, {3, 4, 42, 0, 1, 4, 4, false}), Receive::waiting);
    EXPECT_EQ(feed(*receiver, {4, 5, 42, 0, 1, 4, 4, true}), Receive::complete);
    EXPECT_FALSE(receiver->waiting_idr()); EXPECT_EQ(receiver->resyncs, 1);
    EXPECT_EQ(feed(*receiver, {5, 6, 42, 0, 1, 4, 4, false}), Receive::complete);
}
TEST(IpcFraming, RejectInconsistentFragmentsAndReplay) {
    for (unsigned mutation = 0; mutation < 4; ++mutation) {
        auto receiver = std::make_unique<Reassembler>();
        ASSERT_EQ(feed(*receiver, {1, 0, 42, 0, 2, payload_limit + 4, payload_limit, true}), Receive::fragment);
        Header h{1, 1, 42, 1, 2, payload_limit + 4, 4, true};
        if (mutation == 0) h.timestamp_ns++;
        if (mutation == 1) h.idr = false;
        if (mutation == 2) h.frame++;
        if (mutation == 3) { h.frame_bytes++; h.payload_bytes++; }
        EXPECT_EQ(feed(*receiver, h), Receive::rejected);
        EXPECT_TRUE(receiver->waiting_idr()); EXPECT_TRUE(receiver->frame().empty());
    }
    auto receiver = std::make_unique<Reassembler>();
    ASSERT_EQ(feed(*receiver, {1, 0, 42, 0, 1, 4, 4, true}), Receive::complete);
    EXPECT_EQ(feed(*receiver, {1, 1, 42, 0, 1, 4, 4, true}), Receive::rejected);
}
TEST(IpcFraming, MalformedFloodKeepsFixedStorage) {
    auto receiver = std::make_unique<Reassembler>();
    Packet packet{};
    const auto before = allocation_count();
    unsigned rejected = 0;
    for (unsigned i = 0; i < 10000; ++i) rejected += receiver->accept(packet) == Receive::rejected;
    const auto allocated = allocation_count() - before;
    EXPECT_EQ(rejected, 10000);
    EXPECT_EQ(allocated, 0) << "malformed flood retained dynamic allocation";
    EXPECT_EQ(receiver->rejected, 10000); EXPECT_TRUE(receiver->frame().empty());
    EXPECT_EQ(feed(*receiver, {1, 0, 42, 0, 1, 4, 4, true}), Receive::complete);
}
TEST(IpcFraming, EmptyRecordIsMalformedNotEof) {
    SocketPair sockets; Sender sender(sockets.producer());
    auto receiver = std::make_unique<Reassembler>(); Packet packet{}; bool eof = false;
    std::array<std::uint8_t, 4> idr{{1, 2, 3, 4}};
    ASSERT_EQ(sender.send_frame(idr, 1, true, 7), Send::sent);
    ASSERT_EQ(receive(sockets.consumer(), packet, *receiver, eof), Receive::complete);
    ASSERT_FALSE(eof);
    // A still-connected peer sends a zero-length record; it must be rejected with loss,
    // never mistaken for end-of-stream.
    ASSERT_EQ(::send(sockets.producer(), nullptr, 0, MSG_NOSIGNAL), 0);
    Header p{2, 100000, 9, 0, 1, 4, 4, false};
    encode(packet, p);
    ASSERT_EQ(::send(sockets.producer(), packet.data(), header_size + 4, MSG_NOSIGNAL),
              static_cast<ssize_t>(header_size + 4));
    EXPECT_EQ(receive(sockets.consumer(), packet, *receiver, eof), Receive::rejected);
    EXPECT_FALSE(eof);
    EXPECT_EQ(receiver->rejected, 1);
    EXPECT_TRUE(receiver->waiting_idr());
    EXPECT_TRUE(receiver->frame().empty());
    // The queued P frame must stay suppressed until a complete IDR arrives.
    EXPECT_EQ(receive(sockets.consumer(), packet, *receiver, eof), Receive::waiting);
    EXPECT_FALSE(eof);
    const auto baseline = receiver->resyncs;
    std::array<std::uint8_t, 4> recovery{{5, 6, 7, 8}};
    ASSERT_EQ(sender.send_frame(recovery, 3, true, 11), Send::sent);
    EXPECT_EQ(receive(sockets.consumer(), packet, *receiver, eof), Receive::complete);
    EXPECT_FALSE(eof);
    EXPECT_TRUE(receiver->header().idr);
    EXPECT_EQ(receiver->frame().size(), recovery.size());
    EXPECT_EQ(receiver->resyncs, baseline + 1);
}
TEST(IpcFraming, GenuineShutdownTerminatesConsumer) {
    SocketPair sockets; Sender sender(sockets.producer());
    auto receiver = std::make_unique<Reassembler>(); Packet packet{}; bool eof = false;
    std::array<std::uint8_t, 4> idr{{9, 9, 9, 9}};
    ASSERT_EQ(sender.send_frame(idr, 1, true, 3), Send::sent);
    ASSERT_EQ(receive(sockets.consumer(), packet, *receiver, eof), Receive::complete);
    ASSERT_FALSE(eof);
    ::shutdown(sockets.producer(), SHUT_WR);
    EXPECT_EQ(receive(sockets.consumer(), packet, *receiver, eof), Receive::waiting);
    EXPECT_TRUE(eof);
    // An orderly EOF stays EOF and keeps delivering nothing; it is not counted as malformed.
    EXPECT_EQ(receive(sockets.consumer(), packet, *receiver, eof), Receive::waiting);
    EXPECT_TRUE(eof);
    EXPECT_EQ(receiver->rejected, 0);
}
TEST(IpcFraming, SocketFragmentationAndTruncation) {
    SocketPair sockets; Sender sender(sockets.producer());
    auto receiver = std::make_unique<Reassembler>(); Packet packet{}; bool eof = false;
    std::vector<std::uint8_t> bytes(payload_limit + 7, 0x5a);
    ASSERT_EQ(sender.send_frame(bytes, 1, true, 42), Send::sent);
    EXPECT_EQ(receive(sockets.consumer(), packet, *receiver, eof), Receive::fragment);
    EXPECT_EQ(receive(sockets.consumer(), packet, *receiver, eof), Receive::complete);
    EXPECT_EQ(receiver->frame().size(), bytes.size());
    EXPECT_TRUE(std::equal(bytes.begin(), bytes.end(), receiver->frame().begin()));
    bytes.resize(header_size + payload_limit + 1);
    ASSERT_EQ(::send(sockets.producer(), bytes.data(), bytes.size(), MSG_NOSIGNAL), static_cast<ssize_t>(bytes.size()));
    EXPECT_EQ(receive(sockets.consumer(), packet, *receiver, eof), Receive::rejected);
    EXPECT_TRUE(receiver->waiting_idr()); EXPECT_EQ(receiver->rejected, 1);
    bytes.resize(frame_limit + 1);
    EXPECT_EQ(sender.send_frame(bytes, 2, true, 42), Send::invalid);
    EXPECT_EQ(sender.send_frame({}, 2, true, 42), Send::invalid);
    EXPECT_LE(sockets.send_buffer(), 2 * socket_budget);
}
