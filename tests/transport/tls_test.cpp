// SPDX-License-Identifier: GPL-3.0-or-later
// TLS integration (task 16): a real TLS handshake and encrypted-message round
// trip through the production transport, using the hardened task-8 credential and
// admission policy. Proves the decrypt-29 record-length flaw is avoided (a short
// record is a typed error, never an underflow) and that an unknown or revoked
// peer fails closed.

#include <aa/core/Cancellation.hpp>
#include <aa/transport/Framing.hpp>
#include <aa/transport/Frames.hpp>
#include <aa/transport/TcpTransport.hpp>
#include <aa/transport/TlsTransport.hpp>

#include "../src/transport/tls_adapter/TlsRecordCodec.hpp"

#include <atomic>
#include <cstddef>
#include <optional>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

namespace {

namespace tr = aa::transport;
namespace tls = aa::transport::tls_adapter;
using tr::FrameType;
using tr::MessageKind;
using tr::Encryption;

std::vector<std::byte> pattern(std::size_t size) {
    std::vector<std::byte> out(size);
    for (std::size_t i = 0; i < size; ++i) {
        out[i] = static_cast<std::byte>((i * 13u) & 0xFFu);
    }
    return out;
}

[[nodiscard]] aa::tls::Policy approved_policy(bool* approved) {
    return aa::tls::Policy([approved] { return *approved; },
                           aa::tls::Mode::encryption_only_compatibility);
}

// Handshake a client and server record codec over memory BIOs (real TLS records).
void handshake(tls::TlsRecordCodec& client, tls::TlsRecordCodec& server) {
    tls::pump_handshake(client, server);
    ASSERT_TRUE(client.is_active());
    ASSERT_TRUE(server.is_active());
}

} // namespace

TEST(TlsRecordCodec, SealOpenRoundTripsWithExactOverhead) {
    bool approved = true;
    tls::TlsRecordCodec client(approved_policy(&approved), tls::TlsRecordCodec::Role::client);
    tls::TlsRecordCodec server(approved_policy(&approved), tls::TlsRecordCodec::Role::server);
    handshake(client, server);

    const auto plain = pattern(100);
    auto sealed = client.seal(plain);
    ASSERT_TRUE(sealed.has_value());
    // One plaintext chunk is exactly one record: ciphertext = plaintext + 29.
    EXPECT_EQ(sealed.value().size(), plain.size() + 29u);
    EXPECT_EQ(client.record_overhead(), 29u);

    auto opened = server.open(sealed.value());
    ASSERT_TRUE(opened.has_value());
    EXPECT_EQ(opened.value(), plain);
}

TEST(TlsRecordCodec, RejectsShortRecordWithoutUnderflow) {
    // The decrypt-29 safety case: a record shorter than the fixed overhead must
    // be rejected as malformed, never subtracted into a negative length.
    bool approved = true;
    tls::TlsRecordCodec client(approved_policy(&approved), tls::TlsRecordCodec::Role::client);
    tls::TlsRecordCodec server(approved_policy(&approved), tls::TlsRecordCodec::Role::server);
    handshake(client, server);

    for (std::size_t size : {std::size_t{0}, std::size_t{5}, std::size_t{28}}) {
        const std::vector<std::byte> short_record(size, std::byte{0xAB});
        auto opened = server.open(short_record);
        ASSERT_FALSE(opened.has_value());
        EXPECT_EQ(opened.error().code(), aa::ErrorCode::transport_malformed_frame);
    }
}

TEST(TlsRecordCodec, ReassemblesEncryptedMessage) {
    bool approved = true;
    tls::TlsRecordCodec client(approved_policy(&approved), tls::TlsRecordCodec::Role::client);
    tls::TlsRecordCodec server(approved_policy(&approved), tls::TlsRecordCodec::Role::server);
    handshake(client, server);

    // A fragmented encrypted message: each chunk is sealed into one TLS record.
    const tr::Message message{0x60, MessageKind::specific, pattern(tr::kPlainChunkBytes * 2 + 9)};
    auto frames = tr::encode_message(message, Encryption::encrypted, &client);
    ASSERT_TRUE(frames.has_value());
    ASSERT_GT(frames.value().size(), 1u); // fragmented

    tr::MessageReassembler reassembler(&server);
    std::optional<tr::Message> out;
    for (const auto& frame : frames.value()) {
        auto pushed = reassembler.push(frame);
        ASSERT_TRUE(pushed.has_value());
        if (pushed.value()) {
            out = pushed.value();
        }
    }
    ASSERT_TRUE(out.has_value());
    EXPECT_EQ(out.value(), message);
}

TEST(TlsRecordCodec, RevokedPeerFailsClosed) {
    bool approved = true;
    tls::TlsRecordCodec client(approved_policy(&approved), tls::TlsRecordCodec::Role::client);
    tls::TlsRecordCodec server(approved_policy(&approved), tls::TlsRecordCodec::Role::server);
    handshake(client, server);

    approved = false; // the phone is revoked after the handshake
    auto sealed = client.seal(pattern(16));
    ASSERT_FALSE(sealed.has_value());
    EXPECT_EQ(sealed.error().code(), aa::ErrorCode::transport_unauthorized_peer);
}

TEST(TlsIntegration, EncryptedMessageThroughTcpTransport) {
    // Given: a real TLS session (record codecs) and a real TCP loopback transport.
    bool approved = true;
    tls::TlsRecordCodec client(approved_policy(&approved), tls::TlsRecordCodec::Role::client);
    tls::TlsRecordCodec server(approved_policy(&approved), tls::TlsRecordCodec::Role::server);
    handshake(client, server);
    auto pair = tr::make_tcp_loopback();
    ASSERT_TRUE(pair.has_value());

    // When: an encrypted fragmented message is framed (sealed payloads) and sent
    // over the production TCP transport.
    const tr::Message message{0x70, MessageKind::specific, pattern(tr::kPlainChunkBytes + 11)};
    auto frames = tr::encode_message(message, Encryption::encrypted, &client);
    ASSERT_TRUE(frames.has_value());
    for (const auto& frame : frames.value()) {
        ASSERT_TRUE(pair.value().client->send(frame).has_value());
    }
    // Then: it is received and opened back to the original plaintext.
    tr::MessageReassembler reassembler(&server);
    std::optional<tr::Message> out;
    for (std::size_t i = 0; i < frames.value().size(); ++i) {
        auto got = pair.value().server->receive();
        ASSERT_TRUE(got.has_value());
        auto pushed = reassembler.push(got.value());
        ASSERT_TRUE(pushed.has_value());
        if (pushed.value()) {
            out = pushed.value();
        }
    }
    ASSERT_TRUE(out.has_value());
    EXPECT_EQ(out.value(), message);
}

TEST(TlsTransport, HandshakeAndFrameRoundTrip) {
    // Given: a real TLS transport pair over a real TCP socket.
    auto pair = tr::make_tls_loopback([] { return true; },
                                      tr::TlsMode::encryption_only_compatibility);
    ASSERT_TRUE(pair.has_value());
    auto& client = pair.value().client;
    auto& server = pair.value().server;

    // When: both ends drive a genuine TLS handshake concurrently.
    bool client_ok = false;
    bool server_ok = false;
    std::thread client_thread([&] { client_ok = client->open({}).has_value(); });
    std::thread server_thread([&] { server_ok = server->open({}).has_value(); });
    client_thread.join();
    server_thread.join();
    ASSERT_TRUE(client_ok) << "client TLS handshake failed";
    ASSERT_TRUE(server_ok) << "server TLS handshake failed";

    // Then: a frame round-trips through the TLS-protected transport.
    auto frame = tr::encode_frame(
        tr::FrameHeader{0x01, FrameType::bulk, MessageKind::specific, Encryption::plain},
        pattern(24), 0);
    ASSERT_TRUE(frame.has_value());
    ASSERT_TRUE(client->send(frame.value()).has_value());
    auto got = server->receive();
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got.value(), frame.value());
}

TEST(TlsTransport, UnknownPeerFailsClosed) {
    // Given: a TLS transport whose approved-phone predicate rejects the peer.
    auto pair = tr::make_tls_loopback([] { return false; },
                                      tr::TlsMode::encryption_only_compatibility);
    ASSERT_TRUE(pair.has_value());
    // Then: open fails closed before any session is established.
    auto opened = pair.value().client->open({});
    ASSERT_FALSE(opened.has_value());
    EXPECT_EQ(opened.error().code(), aa::ErrorCode::transport_unauthorized_peer);
}

namespace {

[[nodiscard]] bool handshake_tls(tr::TlsTransport& client, tr::TlsTransport& server) {
    bool client_ok = false;
    bool server_ok = false;
    std::thread client_thread([&] { client_ok = client.open({}).has_value(); });
    std::thread server_thread([&] { server_ok = server.open({}).has_value(); });
    client_thread.join();
    server_thread.join();
    return client_ok && server_ok;
}

} // namespace

TEST(TlsTransport, SendToClosedPeerIsSafe) {
    // Given: a handshaked TLS transport whose remote peer has closed.
    auto pair = tr::make_tls_loopback([] { return true; },
                                      tr::TlsMode::encryption_only_compatibility);
    ASSERT_TRUE(pair.has_value());
    auto& client = pair.value().client;
    ASSERT_TRUE(handshake_tls(*client, *pair.value().server));
    pair.value().server->close();
    auto frame = tr::encode_frame(
        tr::FrameHeader{0x01, FrameType::bulk, MessageKind::specific, Encryption::plain},
        pattern(64), 0);
    ASSERT_TRUE(frame.has_value());
    // When: we send into the dead TLS session.
    bool saw_error = false;
    for (int i = 0; i < 4096; ++i) {
        auto sent = client->send(frame.value());
        if (!sent.has_value()) {
            saw_error = true;
            break;
        }
    }
    // Then: no SIGPIPE kills the process; the dead peer surfaces a typed error.
    EXPECT_TRUE(saw_error);
}

TEST(TlsTransport, InFlightReceiveIsCancelledAfterReadBegins) {
    // Given: a handshaked TLS transport with no inbound data and a bound source.
    auto pair = tr::make_tls_loopback([] { return true; },
                                      tr::TlsMode::encryption_only_compatibility);
    ASSERT_TRUE(pair.has_value());
    auto& client = pair.value().client;
    auto& server = pair.value().server;
    aa::core::CancellationSource source;
    bool client_ok = false;
    bool server_ok = false;
    std::thread client_thread([&] { client_ok = client->open(source.get_token()).has_value(); });
    std::thread server_thread([&] { server_ok = server->open({}).has_value(); });
    client_thread.join();
    server_thread.join();
    ASSERT_TRUE(client_ok && server_ok);
    std::atomic<bool> started{false};
    bool had_value = true;
    aa::ErrorCode code = aa::ErrorCode::internal;
    // When: the receive begins (blocks) before the token fires.
    std::thread receiver([&] {
        started.store(true);
        auto got = client->receive();
        had_value = got.has_value();
        if (!had_value) {
            code = got.error().code();
        }
    });
    while (!started.load()) {
        std::this_thread::yield();
    }
    for (int i = 0; i < 2000; ++i) {
        std::this_thread::yield();
    }
    source.request_stop();
    receiver.join();
    // Then: the outstanding read returns cancelled, not a hang.
    EXPECT_FALSE(had_value);
    EXPECT_EQ(code, aa::ErrorCode::cancelled);
}

TEST(TlsTransport, RevokedPeerSafelyCloses) {
    // Given: a handshaked TLS transport with an approved phone.
    bool approved = true;
    auto pair = tr::make_tls_loopback([&] { return approved; },
                                      tr::TlsMode::encryption_only_compatibility);
    ASSERT_TRUE(pair.has_value());
    auto& client = pair.value().client;
    ASSERT_TRUE(handshake_tls(*client, *pair.value().server));
    auto frame = tr::encode_frame(
        tr::FrameHeader{0x01, FrameType::bulk, MessageKind::specific, Encryption::plain},
        pattern(8), 0);
    ASSERT_TRUE(frame.has_value());
    // When: the phone is revoked after the handshake and we send.
    approved = false;
    auto sent = client->send(frame.value());
    // Then: the send fails closed AND the transport is observably closed.
    ASSERT_FALSE(sent.has_value());
    EXPECT_EQ(sent.error().code(), aa::ErrorCode::transport_unauthorized_peer);
    auto after = client->receive();
    ASSERT_FALSE(after.has_value());
    EXPECT_EQ(after.error().code(), aa::ErrorCode::transport_closed);
}
