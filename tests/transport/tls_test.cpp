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
#include <cstdlib>
#include <optional>
#include <string>
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

TEST(TlsRecordCodec, HuClientFactoryEmitsClientHelloAndHandshakesWithServer) {
    auto hu = tr::make_client_handshake_cipher([] { return true; });
    ASSERT_NE(hu, nullptr);
    ASSERT_TRUE(hu->step_handshake().has_value());
    EXPECT_FALSE(hu->is_active());
    auto first_flight = hu->take_handshake();
    ASSERT_GT(first_flight.size(), 5U);
    EXPECT_EQ(first_flight[0], std::byte{0x16});
    EXPECT_EQ(first_flight[5], std::byte{0x01});

    bool approved = true;
    tls::TlsRecordCodec server(approved_policy(&approved), tls::TlsRecordCodec::Role::server);
    server.give_handshake(first_flight);
    for (int round = 0; round < 16 && (!hu->is_active() || !server.is_active()); ++round) {
        ASSERT_TRUE(server.step_handshake().has_value());
        hu->give_handshake(server.take_handshake());
        ASSERT_TRUE(hu->step_handshake().has_value());
        server.give_handshake(hu->take_handshake());
    }
    ASSERT_TRUE(hu->is_active());
    ASSERT_TRUE(server.is_active());
    const auto plain = pattern(37);
    auto sealed = hu->seal(plain);
    ASSERT_TRUE(sealed.has_value());
    auto opened = server.open(sealed.value());
    ASSERT_TRUE(opened.has_value());
    EXPECT_EQ(opened.value(), plain);
}

// Exercise the production cipher transition, rather than a synthetic active flag.
TEST(TlsRecordCodec, PlaintextOnlyBeforeHandshake) {
    bool approved = true;
    tls::TlsRecordCodec client(approved_policy(&approved), tls::TlsRecordCodec::Role::client);
    tls::TlsRecordCodec server(approved_policy(&approved), tls::TlsRecordCodec::Role::server);
    tr::MessageReassembler reassembler(&server);
    const tr::Message message{0, MessageKind::specific, pattern(16)};
    auto frames = tr::encode_message(message, Encryption::plain, nullptr);
    ASSERT_TRUE(frames.has_value());
    auto before = reassembler.push(frames.value().front());
    ASSERT_TRUE(before.has_value());
    ASSERT_TRUE(before.value().has_value());
    EXPECT_EQ(*before.value(), message);
    ASSERT_FALSE(client.step_handshake().value());
    server.give_handshake(client.take_handshake());
    ASSERT_FALSE(server.step_handshake().value());
    auto during = reassembler.push(frames.value().front());
    ASSERT_TRUE(during.has_value());
    EXPECT_EQ(*during.value(), message);
    client.give_handshake(server.take_handshake());
    handshake(client, server);
    auto after = reassembler.push(frames.value().front());
    ASSERT_FALSE(after.has_value());
    EXPECT_EQ(after.error().code(), aa::ErrorCode::transport_plaintext_after_tls);
    EXPECT_TRUE(reassembler.closed());
    // Every channel and fragmentation flag is subject to the same policy.
    for (const auto channel : {std::uint8_t{0}, std::uint8_t{99}}) {
        for (const auto type : {FrameType::bulk, FrameType::first, FrameType::middle, FrameType::last}) {
            tr::MessageReassembler fresh(&server);
            auto frame = tr::encode_frame({channel, type, MessageKind::specific, Encryption::plain},
                                          message.payload, 16);
            ASSERT_TRUE(frame.has_value());
            auto rejected = fresh.push(frame.value());
            ASSERT_FALSE(rejected.has_value());
            EXPECT_EQ(rejected.error().code(), aa::ErrorCode::transport_plaintext_after_tls);
            EXPECT_TRUE(fresh.closed());
        }
    }
    EXPECT_EQ(reassembler.push(frames.value().front()).error().code(),
              aa::ErrorCode::transport_closed);
}

// Review P2: the rejection must not depend on cipher health. seal() clears
// active_ on any failure (here a mid-session revoke); plaintext must still fail.
TEST(TlsRecordCodec, PlaintextStaysRejectedAfterSealFailureClearsActive) {
    bool approved = true;
    tls::TlsRecordCodec client(approved_policy(&approved), tls::TlsRecordCodec::Role::client);
    tls::TlsRecordCodec server(approved_policy(&approved), tls::TlsRecordCodec::Role::server);
    handshake(client, server);
    tr::MessageReassembler reassembler(&server);
    const tr::Message message{0, MessageKind::specific, pattern(16)};

    // Encrypted traffic is accepted while the cipher is healthy (this also
    // latches the requirement on the first push that sees an active cipher).
    auto sealed = tr::encode_message(message, Encryption::encrypted, &client);
    ASSERT_TRUE(sealed.has_value());
    auto opened = reassembler.push(sealed.value().front());
    ASSERT_TRUE(opened.has_value());
    ASSERT_TRUE(opened.value().has_value());
    EXPECT_TRUE(reassembler.encryption_required());

    // Revoke mid-session: the next seal fails and clears active_.
    approved = false;
    ASSERT_FALSE(server.seal(pattern(8)).has_value());
    ASSERT_FALSE(server.is_active());
    approved = true;

    auto plain = tr::encode_message(message, Encryption::plain, nullptr);
    ASSERT_TRUE(plain.has_value());
    auto rejected = reassembler.push(plain.value().front());
    ASSERT_FALSE(rejected.has_value());
    EXPECT_EQ(rejected.error().code(), aa::ErrorCode::transport_plaintext_after_tls);
    EXPECT_TRUE(reassembler.closed());
}

// The cipher can also deactivate before the first post-handshake frame ever
// reaches the reassembler; the explicit latch covers that gap.
TEST(TlsRecordCodec, ExplicitLatchSurvivesCipherDeactivatingBeforeFirstFrame) {
    bool approved = true;
    tls::TlsRecordCodec client(approved_policy(&approved), tls::TlsRecordCodec::Role::client);
    tls::TlsRecordCodec server(approved_policy(&approved), tls::TlsRecordCodec::Role::server);
    handshake(client, server);
    tr::MessageReassembler reassembler(&server);
    EXPECT_FALSE(reassembler.encryption_required());
    reassembler.require_encryption();
    approved = false;
    ASSERT_FALSE(server.seal(pattern(8)).has_value());
    ASSERT_FALSE(server.is_active());
    approved = true;
    const tr::Message message{0, MessageKind::specific, pattern(16)};
    auto plain = tr::encode_message(message, Encryption::plain, nullptr);
    ASSERT_TRUE(plain.has_value());
    auto rejected = reassembler.push(plain.value().front());
    ASSERT_FALSE(rejected.has_value());
    EXPECT_EQ(rejected.error().code(), aa::ErrorCode::transport_plaintext_after_tls);
    // Before any latch the pre-handshake plaintext path is unchanged.
    tr::MessageReassembler fresh(&server);
    EXPECT_FALSE(fresh.encryption_required());
}

// Review P3: a factory failure must say why, instead of surfacing as a bare null.
TEST(TlsRecordCodec, FactoryFailureReasonIsReportedOutOfBand) {
    const char *saved = std::getenv("HU_KEY_PATH");
    const std::string saved_value = saved != nullptr ? saved : "";
    ASSERT_EQ(::unsetenv("HU_KEY_PATH"), 0);
    auto failed = tr::make_client_handshake_cipher([] { return true; });
    EXPECT_EQ(failed, nullptr);
    EXPECT_NE(tr::last_handshake_cipher_failure().find("tls-credential-unset"), std::string::npos)
        << tr::last_handshake_cipher_failure();
    if (saved != nullptr) {
        ASSERT_EQ(::setenv("HU_KEY_PATH", saved_value.c_str(), 1), 0);
        auto ok = tr::make_client_handshake_cipher([] { return true; });
        EXPECT_NE(ok, nullptr);
        EXPECT_TRUE(tr::last_handshake_cipher_failure().empty());
    }
}

// A peer-side reader of HU output opts out explicitly; the production default
// is the strict HU-inbound rule.
TEST(TlsRecordCodec, PeerSideReaderCanOptOutOfInboundPlaintextRule) {
    bool approved = true;
    tls::TlsRecordCodec client(approved_policy(&approved), tls::TlsRecordCodec::Role::client);
    tls::TlsRecordCodec server(approved_policy(&approved), tls::TlsRecordCodec::Role::server);
    handshake(client, server);
    const tr::Message message{0, MessageKind::specific, pattern(16)};
    auto plain = tr::encode_message(message, Encryption::plain, nullptr);
    ASSERT_TRUE(plain.has_value());
    tr::MessageReassembler peer(&server, tr::PlaintextAfterHandshake::allow);
    auto accepted = peer.push(plain.value().front());
    ASSERT_TRUE(accepted.has_value());
    EXPECT_EQ(*accepted.value(), message);
    tr::MessageReassembler strict(&server);
    EXPECT_FALSE(strict.push(plain.value().front()).has_value());
}

TEST(TlsRecordCodec, ClientHelloExcludesStaticRsa) {
    auto hu = tr::make_client_handshake_cipher([] { return true; });
    ASSERT_NE(hu, nullptr);
    ASSERT_TRUE(hu->step_handshake().has_value());
    const auto hello = hu->take_handshake();
    // TLS record (5), handshake header (4), version (2), random (32), session ID.
    ASSERT_GT(hello.size(), 44U);
    const auto byte = [&](std::size_t i) { return std::to_integer<unsigned>(hello.at(i)); };
    const std::size_t offset = 44U + byte(43);
    const std::size_t length = (byte(offset) << 8U) | byte(offset + 1U);
    ASSERT_LE(offset + 2U + length, hello.size());
    ASSERT_EQ(length % 2U, 0U);
    bool ecdhe = false;
    for (std::size_t i = offset + 2U; i < offset + 2U + length; i += 2U) {
        const auto suite = (byte(i) << 8U) | byte(i + 1U);
        ecdhe |= suite == 0xc02fU || suite == 0xc030U;
        // Ignore the renegotiation SCSV, which is not a negotiable cipher.
        if (suite != 0x00ffU) {
#if AA_TLS_ALLOW_STATIC_RSA_COMPATIBILITY
            EXPECT_TRUE(suite == 0xc02fU || suite == 0xc030U || suite == 0x009cU || suite == 0x009dU)
                << suite;
#else
            EXPECT_TRUE(suite == 0xc02fU || suite == 0xc030U) << suite;
#endif
        }
    }
    EXPECT_TRUE(ecdhe);
}

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
