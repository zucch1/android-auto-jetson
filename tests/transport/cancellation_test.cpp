// SPDX-License-Identifier: GPL-3.0-or-later
// In-flight cancellation on real TCP/USB transports (task 16 followup): prove an
// outstanding blocking read/send is genuinely interrupted by the cancellation
// token, not merely observed before the operation schedules.

#include <aa/core/Cancellation.hpp>
#include <aa/transport/ByteEndpoint.hpp>
#include <aa/transport/Frames.hpp>
#include <aa/transport/TcpTransport.hpp>
#include <aa/transport/UsbByteTransport.hpp>

#include <atomic>
#include <cstddef>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

namespace {

namespace tr = aa::transport;

std::vector<std::byte> pattern(std::size_t size) {
    std::vector<std::byte> out(size);
    for (std::size_t i = 0; i < size; ++i) {
        out[i] = static_cast<std::byte>((i * 17u) & 0xFFu);
    }
    return out;
}

// Signal `started` immediately before the blocking call, then let it enter the
// blocking operation. This is the synchronization proving the operation began
// before the stop is requested (unlike stopping before the thread schedules).
void yield_to_block() {
    for (int i = 0; i < 2000; ++i) {
        std::this_thread::yield();
    }
}

struct Outcome {
    bool had_value{true};
    aa::ErrorCode code{aa::ErrorCode::internal};
};

} // namespace

TEST(InFlightCancellation, TcpReceiveIsCancelledAfterReadBegins) {
    // Given: a TCP loopback with no inbound data and a bound cancellation source.
    auto pair = tr::make_tcp_loopback();
    ASSERT_TRUE(pair.has_value());
    auto& client = pair.value().client;
    aa::core::CancellationSource source;
    ASSERT_TRUE(client->open(source.get_token()).has_value());
    std::atomic<bool> started{false};
    Outcome outcome;
    // When: the receive begins (blocks with no data) before the token fires.
    std::thread receiver([&] {
        started.store(true);
        auto got = client->receive();
        outcome.had_value = got.has_value();
        if (!outcome.had_value) {
            outcome.code = got.error().code();
        }
    });
    while (!started.load()) {
        std::this_thread::yield();
    }
    yield_to_block();
    source.request_stop();
    receiver.join();
    // Then: the outstanding read returns cancelled, not a hang.
    EXPECT_FALSE(outcome.had_value);
    EXPECT_EQ(outcome.code, aa::ErrorCode::cancelled);
}

TEST(InFlightCancellation, UsbReceiveIsCancelledAfterReadBegins) {
    // Given: a USB byte-endpoint loopback with no inbound data.
    auto pair = tr::make_loopback();
    ASSERT_TRUE(pair.has_value());
    auto transport = tr::UsbByteTransport::over_endpoint(std::move(pair.value().b));
    ASSERT_TRUE(transport.has_value());
    aa::core::CancellationSource source;
    ASSERT_TRUE(transport.value()->open(source.get_token()).has_value());
    std::atomic<bool> started{false};
    Outcome outcome;
    // When: the receive begins (blocks with no data) before the token fires.
    std::thread receiver([&] {
        started.store(true);
        auto got = transport.value()->receive();
        outcome.had_value = got.has_value();
        if (!outcome.had_value) {
            outcome.code = got.error().code();
        }
    });
    while (!started.load()) {
        std::this_thread::yield();
    }
    yield_to_block();
    source.request_stop();
    receiver.join();
    // Then: the outstanding read returns cancelled, not a hang.
    EXPECT_FALSE(outcome.had_value);
    EXPECT_EQ(outcome.code, aa::ErrorCode::cancelled);
}

TEST(InFlightCancellation, TcpSendIsCancelledOnSaturatedSocket) {
    // Given: a TCP loopback whose peer never reads, so the send buffer saturates.
    auto pair = tr::make_tcp_loopback();
    ASSERT_TRUE(pair.has_value());
    auto& client = pair.value().client;
    aa::core::CancellationSource source;
    ASSERT_TRUE(client->open(source.get_token()).has_value());
    auto frame = tr::encode_frame(
        tr::FrameHeader{0x01, tr::FrameType::bulk, tr::MessageKind::specific, tr::Encryption::plain},
        pattern(tr::kPlainChunkBytes), 0);
    ASSERT_TRUE(frame.has_value());
    std::atomic<bool> started{false};
    Outcome outcome;
    // When: the send saturates the socket and blocks in-flight before the token fires.
    std::thread sender([&] {
        started.store(true);
        for (int i = 0; i < 10000; ++i) {
            auto sent = client->send(frame.value());
            if (!sent.has_value()) {
                outcome.had_value = false;
                outcome.code = sent.error().code();
                return;
            }
        }
    });
    while (!started.load()) {
        std::this_thread::yield();
    }
    yield_to_block();
    source.request_stop();
    sender.join();
    // Then: the in-flight send returns cancelled, not a hang.
    EXPECT_FALSE(outcome.had_value);
    EXPECT_EQ(outcome.code, aa::ErrorCode::cancelled);
}
