// SPDX-License-Identifier: GPL-3.0-or-later
// F3 regression (owner ruling 2026-10-10): bounded framing rejects malformed
// and oversize records typed (transport_malformed_frame /
// transport_oversize_frame / protocol_version_mismatch) with fixed storage -
// the receive path never allocates beyond the one stack record.
#include "pair.hpp"

#include <aa/ipc/InputChannel.hpp>
#include <aa/ipc/InputSocket.hpp>

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <new>
#include <sys/socket.h>
#include <sys/un.h>
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

std::vector<std::uint8_t> raw_record_for(const InputEvent& event) {
    std::array<std::uint8_t, kInputRecordBytes> record{};
    encode_input_record(record, event);
    return std::vector<std::uint8_t>(record.begin(), record.end());
}

void raw_send(int fd, const std::vector<std::uint8_t>& record) {
    (void)::send(fd, record.data(), record.size(), MSG_NOSIGNAL);
}

int raw_connect(const InputSocketPath& path) {
    const int fd = ::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        return -1;
    }
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, path.value.c_str(), path.value.size() + 1);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        ::close(fd);
        return -1;
    }
    return fd;
}
} // namespace

TEST(InputMalformed, MalformedAndOversizeRecordsAreTypedWithFixedStorage) {
    auto pair = inputtest::make_pair();
    ASSERT_TRUE(pair);
    const auto path = input_socket_path_in(pair.value().runtime_dir);
    const int peer = raw_connect(path);
    ASSERT_GE(peer, 0);
    auto endpoint = pair.value().server.accept_sender(aa::core::Milliseconds{2000});
    ASSERT_TRUE(endpoint);

    std::vector<std::uint8_t> bad_magic = raw_record_for(InputEvent{KeyEvent{19, true}});
    bad_magic[0] ^= 0xff;
    std::vector<std::uint8_t> short_record(kInputRecordBytes - 1, 0x33);
    std::vector<std::uint8_t> wrong_version = raw_record_for(InputEvent{KeyEvent{19, true}});
    wrong_version[4] = 0x00;
    wrong_version[5] = 0x02;
    std::vector<std::uint8_t> unknown_kind(kInputRecordBytes, 0x00);
    unknown_kind[11] = 0x07;
    std::vector<std::uint8_t> bad_pressed = raw_record_for(InputEvent{KeyEvent{19, true}});
    bad_pressed[19] = 0x02;
    std::vector<std::uint8_t> bad_action = raw_record_for(
        InputEvent{TouchEvent{1, 2, TouchAction::down}});
    bad_action[23] = 0x09;
    std::vector<std::uint8_t> huge(kInputRecordBytes * 4, 0x11);

    const auto before = allocation_count();
    raw_send(peer, bad_magic);
    raw_send(peer, short_record);
    raw_send(peer, wrong_version);
    raw_send(peer, unknown_kind);
    raw_send(peer, bad_pressed);
    raw_send(peer, bad_action);
    raw_send(peer, huge);

    std::array<aa::ErrorCode, 7> expected{aa::ErrorCode::transport_malformed_frame,
                                          aa::ErrorCode::transport_malformed_frame,
                                          aa::ErrorCode::protocol_version_mismatch,
                                          aa::ErrorCode::transport_malformed_frame,
                                          aa::ErrorCode::transport_malformed_frame,
                                          aa::ErrorCode::transport_malformed_frame,
                                          aa::ErrorCode::transport_oversize_frame};
    for (const auto want : expected) {
        auto step = endpoint.value().receive(aa::core::Milliseconds{500});
        ASSERT_FALSE(step) << "malformed input was accepted";
        EXPECT_EQ(step.error().code(), want);
    }
    EXPECT_EQ(endpoint.value().rejected(), 7U);
    EXPECT_LE(allocation_count() - before, 2) << "malformed flood grew dynamic storage";

    const auto recovery = raw_record_for(InputEvent{KeyEvent{20, true}});
    raw_send(peer, recovery);
    auto good = endpoint.value().receive(aa::core::Milliseconds{500});
    ASSERT_TRUE(good) << "endpoint did not resync after rejected records";
    EXPECT_EQ(good.value(), (InputEvent{KeyEvent{20, true}}));
    ::close(peer);
}

TEST(InputMalformed, ZeroLengthRecordIsMalformed) {
    auto pair = inputtest::make_pair();
    ASSERT_TRUE(pair);
    const auto path = input_socket_path_in(pair.value().runtime_dir);
    const int peer = raw_connect(path);
    ASSERT_GE(peer, 0);
    auto endpoint = pair.value().server.accept_sender(aa::core::Milliseconds{2000});
    ASSERT_TRUE(endpoint);

    raw_send(peer, std::vector<std::uint8_t>{});
    auto step = endpoint.value().receive(aa::core::Milliseconds{500});
    ASSERT_FALSE(step);
    EXPECT_EQ(step.error().code(), aa::ErrorCode::transport_malformed_frame);
    ::close(peer);
}
