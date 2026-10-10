// SPDX-License-Identifier: GPL-3.0-or-later
// TCP and USB byte-endpoint transports (task 16): a real TCP loopback and an
// in-memory USB byte endpoint both move complete AAP frames with identical
// bounded framing and typed malformed-input handling.

#include <aa/core/Cancellation.hpp>
#include <aa/transport/ByteEndpoint.hpp>
#include <aa/transport/Framing.hpp>
#include <aa/transport/Frames.hpp>
#include <aa/transport/TcpTransport.hpp>
#include <aa/transport/UsbByteTransport.hpp>

#include <array>
#include <cstddef>
#include <memory>
#include <optional>
#include <thread>
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
        out[i] = static_cast<std::byte>((i * 17u) & 0xFFu);
    }
    return out;
}

// Drive a message's frames through a transport pair and reassemble on the far end.
struct Link final {
    tr::Transport& send;
    tr::Transport& recv;
};

void roundtrip(Link link, const tr::Message& message) {
    auto frames = tr::encode_message(message, Encryption::plain, nullptr);
    ASSERT_TRUE(frames.has_value());
    for (const auto& frame : frames.value()) {
        auto sent = link.send.send(frame);
        ASSERT_TRUE(sent.has_value());
    }
    tr::MessageReassembler reassembler(nullptr);
    std::optional<tr::Message> out;
    for (std::size_t i = 0; i < frames.value().size(); ++i) {
        auto got = link.recv.receive();
        ASSERT_TRUE(got.has_value()) << "receive failed";
        auto pushed = reassembler.push(got.value());
        ASSERT_TRUE(pushed.has_value());
        if (pushed.value()) {
            out = pushed.value();
        }
    }
    ASSERT_TRUE(out.has_value());
    EXPECT_EQ(out.value(), message);
}

} // namespace

TEST(TcpTransport, RoundTripsMessageOverLoopback) {
    // Given: a real connected TCP loopback pair.
    auto pair = tr::make_tcp_loopback();
    ASSERT_TRUE(pair.has_value());
    const tr::Message message{0x21, MessageKind::specific, pattern(64)};
    // When: the message is sent client -> server and back.
    roundtrip({*pair.value().client, *pair.value().server}, message);
    const tr::Message reply{0x22, MessageKind::control, pattern(32)};
    roundtrip({*pair.value().server, *pair.value().client}, reply);
}

TEST(TcpTransport, RoundTripsFragmentedMessage) {
    auto pair = tr::make_tcp_loopback();
    ASSERT_TRUE(pair.has_value());
    const tr::Message message{0x30, MessageKind::specific,
                              pattern(tr::kPlainChunkBytes * 2 + 7)};
    roundtrip({*pair.value().client, *pair.value().server}, message);
}

TEST(TcpTransport, ReportsClosedAfterClose) {
    auto pair = tr::make_tcp_loopback();
    ASSERT_TRUE(pair.has_value());
    pair.value().client->close();
    pair.value().client->close(); // idempotent
    auto got = pair.value().client->receive();
    ASSERT_FALSE(got.has_value());
    EXPECT_EQ(got.error().code(), aa::ErrorCode::transport_closed);
}

TEST(TcpTransport, RejectsMalformedOutboundFrame) {
    auto pair = tr::make_tcp_loopback();
    ASSERT_TRUE(pair.has_value());
    // A frame whose size field disagrees with the payload is rejected at the
    // transport boundary with a typed error and closes the channel safely.
    const auto bad = bytes({0x01, 0x03, 0x00, 0x04, 0xAA});
    auto sent = pair.value().client->send(bad);
    ASSERT_FALSE(sent.has_value());
    EXPECT_EQ(sent.error().code(), aa::ErrorCode::transport_malformed_frame);
}

TEST(UsbByteTransport, RoundTripsMessageOverLoopback) {
    // Given: an in-memory USB byte-endpoint pair (the task-28 hardware seam).
    auto pair = tr::make_loopback();
    ASSERT_TRUE(pair.has_value());
    auto transport = tr::UsbByteTransport::over_endpoint(std::move(pair.value().b));
    ASSERT_TRUE(transport.has_value());
    auto peer = tr::UsbByteTransport::over_endpoint(std::move(pair.value().a));
    ASSERT_TRUE(peer.has_value());

    const tr::Message message{0x41, MessageKind::specific, pattern(48)};
    roundtrip({*transport.value(), *peer.value()}, message);
}

TEST(UsbByteTransport, RoundTripsFragmentedMessage) {
    auto pair = tr::make_loopback();
    ASSERT_TRUE(pair.has_value());
    auto transport = tr::UsbByteTransport::over_endpoint(std::move(pair.value().b));
    ASSERT_TRUE(transport.has_value());
    auto peer = tr::UsbByteTransport::over_endpoint(std::move(pair.value().a));
    ASSERT_TRUE(peer.has_value());
    const tr::Message message{0x50, MessageKind::control, pattern(tr::kPlainChunkBytes + 3)};
    roundtrip({*transport.value(), *peer.value()}, message);
}

TEST(UsbByteTransport, MalformedInboundFrameClosesSafely) {
    // Given: a raw USB byte endpoint carrying malformed frame bytes.
    auto pair = tr::make_loopback();
    ASSERT_TRUE(pair.has_value());
    auto transport = tr::UsbByteTransport::over_endpoint(std::move(pair.value().b));
    ASSERT_TRUE(transport.has_value());
    // When: the peer writes raw malformed bytes (reserved flag bits).
    const auto bad = bytes({0x01, 0xF3, 0x00, 0x01, 0xAA});
    auto written = pair.value().a->write(bad);
    ASSERT_TRUE(written.has_value());
    // Then: receive surfaces a typed error and the channel is closed/safe.
    auto got = transport.value()->receive();
    ASSERT_FALSE(got.has_value());
    EXPECT_EQ(got.error().code(), aa::ErrorCode::transport_malformed_frame);
    transport.value()->close();
    auto after = transport.value()->receive();
    ASSERT_FALSE(after.has_value());
    EXPECT_EQ(after.error().code(), aa::ErrorCode::transport_closed);
}

TEST(TcpTransport, OutstandingReceiveIsCancelled) {
    // Given: a TCP loopback with no inbound data and a bound cancellation source.
    auto pair = tr::make_tcp_loopback();
    ASSERT_TRUE(pair.has_value());
    auto& client = pair.value().client;
    aa::core::CancellationSource source;
    ASSERT_TRUE(client->open(source.get_token()).has_value());
    bool had_value = true;
    aa::ErrorCode code = aa::ErrorCode::internal;
    // When: a receive blocks with no data and the token fires.
    std::thread receiver([&] {
        auto got = client->receive();
        had_value = got.has_value();
        if (!had_value) {
            code = got.error().code();
        }
    });
    source.request_stop();
    receiver.join();
    // Then: the outstanding receive returns cancelled, not a hang.
    EXPECT_FALSE(had_value);
    EXPECT_EQ(code, aa::ErrorCode::cancelled);
}

TEST(TcpTransport, SendToClosedPeerIsSafe) {
    // Given: a TCP loopback whose remote peer has closed.
    auto pair = tr::make_tcp_loopback();
    ASSERT_TRUE(pair.has_value());
    pair.value().server->close();
    auto frame = tr::encode_frame(
        tr::FrameHeader{0x01, tr::FrameType::bulk, tr::MessageKind::specific, tr::Encryption::plain},
        pattern(256), 0);
    ASSERT_TRUE(frame.has_value());
    // When: we keep sending valid frames into the dead peer.
    bool saw_error = false;
    for (int i = 0; i < 4096; ++i) {
        auto sent = pair.value().client->send(frame.value());
        if (!sent.has_value()) {
            saw_error = true;
            EXPECT_EQ(aa::domain_of(sent.error().code()), aa::ErrorDomain::transport);
            break;
        }
    }
    // Then: no SIGPIPE kills the process AND the dead peer is observable as a
    // typed transport error (not a silent success).
    EXPECT_TRUE(saw_error);
}

TEST(UsbByteTransport, MalformedInboundClosesRealHandle) {
    // Given: a raw USB byte endpoint carrying malformed frame bytes.
    auto pair = tr::make_loopback();
    ASSERT_TRUE(pair.has_value());
    auto transport = tr::UsbByteTransport::over_endpoint(std::move(pair.value().b));
    ASSERT_TRUE(transport.has_value());
    const auto bad = bytes({0x01, 0xF3, 0x00, 0x01, 0xAA});
    ASSERT_TRUE(pair.value().a->write(bad).has_value());
    // When: receive hits the malformed frame.
    auto got = transport.value()->receive();
    ASSERT_FALSE(got.has_value());
    EXPECT_EQ(got.error().code(), aa::ErrorCode::transport_malformed_frame);
    // Then: the underlying endpoint is already closed (no explicit close()).
    auto after = transport.value()->receive();
    ASSERT_FALSE(after.has_value());
    EXPECT_EQ(after.error().code(), aa::ErrorCode::transport_closed);
    auto good = tr::encode_frame(
        tr::FrameHeader{0x01, tr::FrameType::bulk, tr::MessageKind::specific, tr::Encryption::plain},
        pattern(8), 0);
    ASSERT_TRUE(good.has_value());
    auto sent = transport.value()->send(good.value());
    ASSERT_FALSE(sent.has_value());
    EXPECT_EQ(sent.error().code(), aa::ErrorCode::transport_closed);
    // And the underlying endpoint is observably closed: the peer's reader sees
    // end-of-stream once the failed transport released its endpoint.
    std::array<std::byte, 8> drain{};
    aa::core::CancellationSource source;
    auto peer_read = pair.value().a->read(drain, source.get_token());
    ASSERT_TRUE(peer_read.has_value());
    EXPECT_EQ(peer_read.value(), std::size_t{0});
}

TEST(UsbByteTransport, RejectsOversizeFrameWithoutHugeAllocation) {
    // Given: a size field claiming a 65535-byte payload (well past the bound).
    auto pair = tr::make_loopback();
    ASSERT_TRUE(pair.has_value());
    auto transport = tr::UsbByteTransport::over_endpoint(std::move(pair.value().b));
    ASSERT_TRUE(transport.has_value());
    const auto oversize = bytes({0x01, 0x03, 0xFF, 0xFF});
    ASSERT_TRUE(pair.value().a->write(oversize).has_value());
    // When: the transport parses the frame.
    auto got = transport.value()->receive();
    // Then: the oversize length is rejected before buffering the huge payload.
    ASSERT_FALSE(got.has_value());
    EXPECT_EQ(got.error().code(), aa::ErrorCode::transport_oversize_frame);
}

TEST(TcpTransport, RoundTripsLargeFragmentedMessage) {
    // Given: a message spanning several plaintext chunks.
    auto pair = tr::make_tcp_loopback();
    ASSERT_TRUE(pair.has_value());
    const tr::Message message{0x30, tr::MessageKind::specific,
                              pattern(tr::kPlainChunkBytes * 3 + 100)};
    // When/Then: it round-trips within bounded memory over a real loopback.
    roundtrip({*pair.value().client, *pair.value().server}, message);
}
