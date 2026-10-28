// SPDX-License-Identifier: GPL-3.0-or-later
#include <aa/helper/HfpSlc.hpp>
#include <gtest/gtest.h>
#include <algorithm>
#include <array>
#include <cerrno>
#include <fcntl.h>
#include <limits>
#include <memory>
#include <string>
#include <sys/socket.h>
#include <unistd.h>

namespace {
thread_local int engine_fd = -1;
thread_local std::size_t chunk = std::numeric_limits<std::size_t>::max();
thread_local int permits = -1;
thread_local int write_error = 0;
thread_local int read_error = 0;
thread_local bool interrupt_reads = false;
thread_local std::size_t read_calls = 0;
}
extern "C" ssize_t real_send(int, const void*, std::size_t, int) __asm__("__real_send");
extern "C" ssize_t real_recv(int, void*, std::size_t, int) __asm__("__real_recv");
extern "C" ssize_t wrap_send(int, const void*, std::size_t, int) __asm__("__wrap_send");
extern "C" ssize_t wrap_recv(int, void*, std::size_t, int) __asm__("__wrap_recv");
extern "C" ssize_t wrap_send(int fd, const void* data, std::size_t size, int flags) {
    if (fd == engine_fd) {
        if (write_error != 0 || permits == 0) {
            errno = write_error != 0 ? write_error : EAGAIN;
            return -1;
        }
        if (permits > 0) --permits;
        size = std::min(size, chunk);
    }
    return real_send(fd, data, size, flags);
}
extern "C" ssize_t wrap_recv(int fd, void* data, std::size_t size, int flags) {
    if (fd == engine_fd) {
        ++read_calls;
        if (interrupt_reads || read_error != 0) {
            errno = interrupt_reads ? EINTR : read_error;
            return -1;
        }
    }
    return real_recv(fd, data, size, flags);
}

namespace aa::helper {
namespace {
constexpr std::array<std::string_view, 4> commands{
    "AT+BRSF=1023\r", "AT+CIND=?\r", "AT+CIND?\r", "AT+CMER=3,0,0,1\r"};
constexpr std::array<std::string_view, 4> replies{
    "\r\n+BRSF:0\r\n\r\nOK\r\n",
    "\r\n+CIND:(\"service\",(0,1)),(\"call\",(0,1)),(\"callsetup\",(0-3)),"
    "(\"callheld\",(0-2)),(\"signal\",(0-5)),(\"roam\",(0,1)),"
    "(\"battchg\",(0-5))\r\n\r\nOK\r\n",
    "\r\n+CIND:0,0,0,0,0,0,0\r\n\r\nOK\r\n", "\r\nOK\r\n"};
class HfpSlcTest : public ::testing::Test {
protected:
    int peer{-1};
    std::unique_ptr<HfpSlc> engine;
    void SetUp() override {
        int sockets[2]{};
        ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets), 0);
        engine_fd = sockets[0];
        peer = sockets[1];
        chunk = std::numeric_limits<std::size_t>::max();
        permits = -1;
        write_error = 0;
        read_error = 0;
        interrupt_reads = false;
        read_calls = 0;
        engine = std::make_unique<HfpSlc>(engine_fd);
    }
    void TearDown() override {
        engine.reset();
        if (peer >= 0) ::close(peer);
        engine_fd = -1;
    }
    void put(std::string_view wire) {
        ASSERT_EQ(::send(peer, wire.data(), wire.size(), MSG_DONTWAIT | MSG_NOSIGNAL),
                  static_cast<ssize_t>(wire.size()));
    }
    std::string take() {
        std::string result;
        std::array<char, 2048> bytes{};
        const auto count = ::recv(peer, bytes.data(), bytes.size(), MSG_DONTWAIT);
        if (count > 0) result.assign(bytes.data(), static_cast<std::size_t>(count));
        else EXPECT_TRUE(count == 0 || errno == EAGAIN || errno == EWOULDBLOCK);
        return result;
    }
    void advance(std::size_t stages) {
        for (std::size_t i = 0; i < stages; ++i) {
            put(commands[i]);
            engine->tick();
            ASSERT_EQ(take(), replies[i]);
        }
    }
};
TEST_F(HfpSlcTest, CompletesWhenCommandsAreCoalesced) {
    // Given: the complete HF initialization in one transport write.
    std::string wire, expected;
    for (std::size_t i = 0; i < commands.size(); ++i) {
        wire += commands[i];
        wire += '\n';
        expected += replies[i];
    }
    put(wire);
    // When: one tick processes the coalesced commands.
    engine->tick();
    // Then: framing is exact and SLC is connected.
    EXPECT_EQ(take(), expected);
    EXPECT_TRUE(engine->connected());
}
TEST_F(HfpSlcTest, WaitsForCmerWhenCommandsAreFragmented) {
    // Given: every command is fragmented at every byte boundary.
    for (std::size_t stage = 0; stage < commands.size(); ++stage) {
        for (const char byte : commands[stage]) {
            // When: one byte arrives per tick.
            EXPECT_FALSE(engine->connected());
            put(std::string_view{&byte, 1});
            engine->tick();
            // Then: no reply or connection precedes the command terminator.
            EXPECT_EQ(take(), byte == '\r' ? replies[stage] : "");
        }
    }
    EXPECT_TRUE(engine->connected());
}
TEST_F(HfpSlcTest, RetainsResponseWhenWritesArePartial) {
    // Given: the first reply can send only two bytes before backpressure.
    put(commands[0]);
    chunk = 2;
    permits = 1;
    engine->tick();
    std::string response = take();
    ASSERT_EQ(response, "\r\n");
    EXPECT_FALSE(engine->connected());
    // When: writable capacity returns, still with two-byte writes.
    permits = -1;
    engine->tick();
    response += take();
    // Then: no bytes are lost or duplicated.
    EXPECT_EQ(response, replies[0]);
    EXPECT_EQ(engine->state(), HfpSlc::State::cind_test);
}
TEST_F(HfpSlcTest, ConnectsOnlyWhenCmerReplyIsFullyWritten) {
    // Given: negotiation has reached CMER, whose reply stalls before its last byte.
    advance(3);
    put(commands[3]);
    chunk = replies[3].size() - 1;
    permits = 1;
    engine->tick();
    EXPECT_EQ(take(), replies[3].substr(0, replies[3].size() - 1));
    EXPECT_FALSE(engine->connected());
    // When: the last byte can be sent.
    permits = -1;
    engine->tick();
    // Then: connection is published only now.
    EXPECT_EQ(take(), "\n");
    EXPECT_TRUE(engine->connected());
}
class WrongOrder : public HfpSlcTest, public ::testing::WithParamInterface<std::size_t> {};
TEST_P(WrongOrder, ClosesWhenInitializationIsOutOfOrder) {
    // Given: a valid prefix followed by the wrong initialization command.
    advance(GetParam());
    put(commands[(GetParam() + 1) % commands.size()]);
    // When: the invalid stage is processed.
    engine->tick();
    // Then: failure is terminal, not a held-open successful SLC.
    EXPECT_EQ(engine->state(), HfpSlc::State::closed);
    engine->tick();
    EXPECT_FALSE(engine->connected());
}
INSTANTIATE_TEST_SUITE_P(Stages, WrongOrder, ::testing::Values(0U, 1U, 2U, 3U));
class BadInput : public HfpSlcTest, public ::testing::WithParamInterface<std::string> {};
TEST_P(BadInput, ClosesWhenInputIsMalformedOrOversize) {
    // Given: invalid numeric, framing, binary, or oversized input.
    put(GetParam());
    // When: input is processed.
    engine->tick();
    // Then: the owned fd is closed without a successful reply.
    EXPECT_EQ(engine->state(), HfpSlc::State::closed);
    EXPECT_EQ(::fcntl(engine_fd, F_GETFD), -1);
    EXPECT_EQ(errno, EBADF);
}
INSTANTIATE_TEST_SUITE_P(Invalid, BadInput, ::testing::Values(
    "\r", "AT+BRSF=\r", "AT+BRSF=-1\r", "AT+BRSF=+1\r", "AT+BRSF=1x\r",
    "AT+BRSF=4294967296\r", "AT+BRSF=1\n", std::string{"AT\0+BRSF=1\r", 11},
    std::string(257, 'A'), "AT+CMER=3,0,0,0\r"));
class EofStage : public HfpSlcTest, public ::testing::WithParamInterface<std::size_t> {};
TEST_P(EofStage, ClosesWhenPeerEndsStream) {
    // Given: an idle or connected SLC, optionally with a truncated command.
    advance(GetParam() % 5);
    if (GetParam() >= 5) put("AT+");
    ASSERT_EQ(::shutdown(peer, SHUT_WR), 0);
    // When: the partial input and EOF are read.
    engine->tick();
    // Then: EOF revokes even a previously connected SLC.
    EXPECT_EQ(engine->state(), HfpSlc::State::closed);
}
INSTANTIATE_TEST_SUITE_P(Stages, EofStage, ::testing::Range(std::size_t{0}, std::size_t{10}));
TEST_F(HfpSlcTest, RejectsUnsupportedCommandsWhenConnected) {
    // Given: an idle SLC and a maximum-length unsupported AT command.
    advance(4);
    put("AT" + std::string(HfpSlc::max_command_size - 2, 'X') + "\rATD123;\rAT+BCS=1\r");
    // When: unsupported commands arrive.
    engine->tick();
    // Then: no call/audio success is fabricated and SLC remains usable.
    EXPECT_EQ(take(), "\r\nERROR\r\n\r\nERROR\r\n\r\nERROR\r\n");
    EXPECT_TRUE(engine->connected());
}
TEST_F(HfpSlcTest, YieldsWhenSyscallsKeepBeingInterrupted) {
    // Given: every read is interrupted.
    interrupt_reads = true;
    // When: a helper tick runs.
    engine->tick();
    // Then: work remains bounded and the incomplete SLC is not connected.
    EXPECT_EQ(read_calls, HfpSlc::tick_budget);
    EXPECT_EQ(engine->state(), HfpSlc::State::brsf);
}
TEST_F(HfpSlcTest, ClosesWhenCmerWriteFails) {
    // Given: CMER reaches a broken output transport.
    advance(3);
    put(commands[3]);
    write_error = EPIPE;
    // When: the OK cannot be sent.
    engine->tick();
    // Then: no successful connection is reported.
    EXPECT_EQ(engine->state(), HfpSlc::State::closed);
}
TEST_F(HfpSlcTest, ResumesWhenWritesWereInterrupted) {
    // Given: a reply blocked by persistent signal interruptions.
    put(commands[0]);
    write_error = EINTR;
    engine->tick();
    EXPECT_EQ(take(), "");
    // When: signals stop interrupting writes.
    write_error = 0;
    engine->tick();
    // Then: the complete pending reply survives.
    EXPECT_EQ(take(), replies[0]);
    EXPECT_EQ(engine->state(), HfpSlc::State::cind_test);
}
TEST_F(HfpSlcTest, ClosesWhenReadingFails) {
    // Given: a connected socket with a fatal receive error.
    advance(4);
    read_error = ECONNRESET;
    // When: the next tick reads the transport.
    engine->tick();
    // Then: connection is revoked.
    EXPECT_EQ(engine->state(), HfpSlc::State::closed);
}
TEST_F(HfpSlcTest, ClosesWhenConnectedCommandExceedsBound) {
    // Given: a connected SLC receiving an oversized command without a terminator.
    advance(4);
    put("AT" + std::string(HfpSlc::max_command_size - 1, 'X'));
    // When: the first byte beyond the bound arrives.
    engine->tick();
    // Then: malformed framing is fatal even after negotiation.
    EXPECT_EQ(engine->state(), HfpSlc::State::closed);
}
TEST_F(HfpSlcTest, ClosesOwnedSocketWhenDestroyed) {
    // Given: an owned live socket.
    // When: the engine is destroyed.
    engine.reset();
    // Then: ownership releases the fd and the peer observes EOF.
    EXPECT_EQ(::fcntl(engine_fd, F_GETFD), -1);
    EXPECT_EQ(errno, EBADF);
    char byte{};
    EXPECT_EQ(::recv(peer, &byte, 1, MSG_DONTWAIT), 0);
}
}
}
