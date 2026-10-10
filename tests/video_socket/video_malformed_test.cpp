// SPDX-License-Identifier: GPL-3.0-or-later
#include "pair.hpp"

#include <aa/ipc/VideoChannel.hpp>

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <new>
#include <sys/socket.h>
#include <vector>

#include <gtest/gtest.h>

namespace {
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
using namespace aa::ipc;

void raw_send(int fd, const std::vector<std::uint8_t>& record) {
    (void)::send(fd, record.data(), record.size(), MSG_NOSIGNAL);
}

std::vector<std::uint8_t> good_record() {
    std::vector<std::uint8_t> record(kFrameHeaderBytes + 4, 0x5a);
    VideoFrameHeader header{1, 1, 7, 0, 1, 4, 4, true};
    encode_frame_header(record, header);
    return record;
}
} // namespace

TEST(VideoMalformed, MalformedRecordsAreTypedAndKeepFixedStorage) {
    NegotiatedLimits limits{};
    auto pair = videotest::make_pair(limits);
    ASSERT_TRUE(pair);
    auto& consumer = pair.value().consumer;
    const auto producer_fd = pair.value().producer.native_handle();

    std::vector<std::uint8_t> bad_magic = good_record();
    bad_magic[3] ^= 0xff;
    std::vector<std::uint8_t> zero_length;
    std::vector<std::uint8_t> truncated(kFrameHeaderBytes - 1, 0x33);
    const auto before = allocation_count();
    raw_send(producer_fd, bad_magic);
    raw_send(producer_fd, zero_length);
    raw_send(producer_fd, truncated);
    std::vector<std::uint8_t> huge(kFrameHeaderBytes + kMaxDatagramPayload + 1, 0x11);
    raw_send(producer_fd, huge);

    int rejected = 0;
    for (int i = 0; i < 4; ++i) {
        const auto step = consumer.receive(aa::core::Milliseconds{500});
        ASSERT_FALSE(step);
        if (step.error().code() == aa::ErrorCode::transport_malformed_frame ||
            step.error().code() == aa::ErrorCode::transport_oversize_frame) {
            ++rejected;
        }
    }
    EXPECT_EQ(rejected, 4);
    EXPECT_LE(allocation_count() - before, 2) << "malformed flood grew dynamic storage";
    EXPECT_TRUE(consumer.waiting_idr());

    const std::vector<std::uint8_t> recovery{0x01, 0x02};
    ASSERT_TRUE(pair.value().producer.send_access_unit({9, 9, true, recovery}));
    for (int i = 0; i < 8; ++i) {
        const auto step = consumer.receive(aa::core::Milliseconds{500});
        if (step && step.value() == ReceiveStatus::complete) {
            EXPECT_EQ(consumer.access_unit().frame, 9U);
            return;
        }
    }
    FAIL() << "consumer did not resync on the next IDR";
}

TEST(VideoMalformed, OversizeAccessUnitIsRejectedBeforeSend) {
    NegotiatedLimits limits{};
    limits.max_access_unit_bytes = 2048;
    auto pair = videotest::make_pair(limits);
    ASSERT_TRUE(pair);
    std::vector<std::uint8_t> oversize(4096, 0x5a);
    const auto out = pair.value().producer.send_access_unit({1, 1, true, oversize});
    ASSERT_FALSE(out);
    EXPECT_EQ(out.error().code(), aa::ErrorCode::transport_oversize_frame);
    const auto empty =
        pair.value().producer.send_access_unit({2, 2, true, std::span<const std::uint8_t>{}});
    ASSERT_FALSE(empty);
    EXPECT_EQ(empty.error().code(), aa::ErrorCode::invalid_argument);
}

TEST(VideoMalformed, InconsistentFragmentIsRejectedWithLoss) {
    NegotiatedLimits limits{};
    auto pair = videotest::make_pair(limits);
    ASSERT_TRUE(pair);
    const auto producer_fd = pair.value().producer.native_handle();
    auto& consumer = pair.value().consumer;

    const std::uint32_t unit = static_cast<std::uint32_t>(kMaxDatagramPayload + 8);
    std::vector<std::uint8_t> first(kFrameHeaderBytes + kMaxDatagramPayload, 0x21);
    encode_frame_header(
        first, VideoFrameHeader{1, 1, 7, 0, 2, unit, static_cast<std::uint32_t>(kMaxDatagramPayload), true});
    std::vector<std::uint8_t> second(kFrameHeaderBytes + 8, 0x22);
    encode_frame_header(second, VideoFrameHeader{1, 2, 8, 1, 2, unit, 8, true});
    raw_send(producer_fd, first);
    raw_send(producer_fd, second);
    const auto fragment = consumer.receive(aa::core::Milliseconds{500});
    ASSERT_TRUE(fragment);
    EXPECT_EQ(fragment.value(), ReceiveStatus::fragment);
    const auto rejected = consumer.receive(aa::core::Milliseconds{500});
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().code(), aa::ErrorCode::transport_malformed_frame);
    EXPECT_TRUE(consumer.waiting_idr());
}
