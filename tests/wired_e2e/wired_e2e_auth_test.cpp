// SPDX-License-Identifier: GPL-3.0-or-later
//
// AAP authentication/TLS progression (finding F1): a fake phone walks
// version_response -> exchange encapsulated_ssl handshake chunks ->
// HU sends auth_complete. These tests assert the wire contract: the session reaches the
// authenticated/active progression only after auth_complete, pre-auth channel
// and input traffic is refused, and post-auth frames carry the negotiated cipher
// (encrypted, sealed with the handshake cipher) - never plaintext.

#include "harness.hpp"

#include "../session/Fakes.hpp"

#include <gtest/gtest.h>

#include <memory>
#include <optional>
#include <vector>

namespace {

namespace app = aa::app;
namespace proto = aa::protocol;
namespace ss = aa::session;
namespace tr = aa::transport;
using wiredtest::FakeHandshakeCipher;
using wiredtest::ScriptedTransport;

const aa::trust::PhoneIdentity kApproved{"phone-approved"};

struct AuthFixture {
    ss::test::MapTrustStore trust;
    ss::test::ManualClock clock;
    wiredtest::CaptureSink capture;
    aa::diagnostics::Diagnostics diagnostics{capture};
    ScriptedTransport transport;
    app::WiredSession session;
    FakeHandshakeCipher *cipher{nullptr};

    AuthFixture()
        : session(app::WiredSessionDeps{trust, clock, diagnostics}, app::WiredSessionConfig{}) {
        trust.set(kApproved, aa::trust::Decision::approved);
        EXPECT_TRUE(session.configure(wiredtest::golden_services()).has_value());
        auto owned = std::make_unique<FakeHandshakeCipher>();
        cipher = owned.get();
        session.attach_cipher(std::move(owned));
        session.attach_transport(&transport);
        EXPECT_TRUE(session.apply(ss::Event::start_discovery).has_value());
        EXPECT_TRUE(session.phone_discovered(kApproved).has_value());
        EXPECT_TRUE(session.apply(ss::Event::transport_ready).has_value());
    }
};

TEST(WiredE2eAuth, AuthenticatedOnlyAfterAuthComplete) {
    AuthFixture f;
    EXPECT_FALSE(f.session.authenticated());

    f.transport.push_inbound(
        wiredtest::control_frame(proto::encode(proto::VersionResponse{1, 6, 0}).value()));
    ASSERT_TRUE(f.session.poll_transport().has_value());
    EXPECT_FALSE(f.session.authenticated());
    EXPECT_EQ(f.session.version_responses(), 1U);
    EXPECT_EQ(f.session.auth_completes(), 0U);
    auto handshake = tr::decode_frame(f.transport.last_sent());
    ASSERT_TRUE(handshake.has_value());
    EXPECT_EQ(handshake.value().header.encryption, tr::Encryption::plain);
    auto ssl = proto::decode_encapsulated_ssl(handshake.value().payload);
    ASSERT_TRUE(ssl.has_value());
    EXPECT_EQ(ssl.value().bytes, (std::vector<std::byte>{std::byte{0xEE}}));

    const auto before = f.transport.sent_count();
    f.transport.push_inbound(
        wiredtest::control_frame(proto::encode(proto::EncapsulatedSsl{{std::byte{0x01}}}).value()));
    ASSERT_TRUE(f.session.poll_transport().has_value());
    EXPECT_GT(f.session.encapsulated_ssls(), 0U);
    EXPECT_TRUE(f.cipher->is_active());
    ASSERT_EQ(f.transport.sent_count(), before + 2U); // final TLS chunk, then auth
    auto frame = tr::decode_frame(f.transport.last_sent());
    ASSERT_TRUE(frame.has_value());
    EXPECT_EQ(frame.value().header.channel, 0);
    EXPECT_EQ(frame.value().header.encryption, tr::Encryption::plain);
    auto complete = proto::decode_auth_complete(frame.value().payload);
    ASSERT_TRUE(complete.has_value());
    EXPECT_EQ(complete.value().status, 0); // MessageStatus::STATUS_SUCCESS
    EXPECT_TRUE(f.session.authenticated());
    EXPECT_EQ(f.session.auth_completes(), 1U);

    // Only now does channel traffic advance the session toward active.
    f.transport.push_inbound(wiredtest::open_request_frame(99));
    ASSERT_TRUE(f.session.poll_transport().has_value());
    EXPECT_EQ(f.session.channel_opens(), 1U);
    ASSERT_TRUE(f.session.apply(ss::Event::negotiation_succeeded).has_value());
    EXPECT_EQ(f.session.state(), ss::State::active);
}

TEST(WiredE2eAuth, RedrivenHandshakeDoesNotResendAuthComplete) {
    AuthFixture f;
    wiredtest::walk_auth(f.session, f.transport);
    ASSERT_TRUE(f.session.authenticated());
    const auto before = f.transport.sent_count();
    f.transport.push_inbound(
        wiredtest::control_frame(proto::encode(proto::EncapsulatedSsl{{std::byte{0x01}}}).value()));
    ASSERT_TRUE(f.session.poll_transport().has_value());
    EXPECT_EQ(f.session.auth_completes(), 1U);
    EXPECT_EQ(f.transport.sent_count(), before);
    f.transport.push_inbound(
        wiredtest::control_frame(proto::encode(proto::VersionResponse{1, 6, 0}).value()));
    auto refused = f.session.poll_transport();
    ASSERT_FALSE(refused.has_value());
    EXPECT_EQ(refused.error().code(), aa::ErrorCode::transport_unauthorized_peer);
    EXPECT_TRUE(f.session.authenticated());
    EXPECT_EQ(f.session.auth_completes(), 1U);
    EXPECT_EQ(f.transport.sent_count(), before);
}

TEST(WiredE2eAuth, FailedAuthCompleteSendDoesNotAuthenticate) {
    AuthFixture f;
    f.transport.push_inbound(
        wiredtest::control_frame(proto::encode(proto::VersionResponse{1, 6, 0}).value()));
    ASSERT_TRUE(f.session.poll_transport().has_value());
    f.transport.fail_send_at(f.transport.sent_count() + 1U);
    f.transport.push_inbound(
        wiredtest::control_frame(proto::encode(proto::EncapsulatedSsl{{std::byte{0x01}}}).value()));
    auto failed = f.session.poll_transport();
    ASSERT_FALSE(failed.has_value());
    EXPECT_EQ(failed.error().code(), aa::ErrorCode::transport_io);
    EXPECT_TRUE(f.cipher->is_active());
    EXPECT_FALSE(f.session.authenticated());
    EXPECT_EQ(f.session.auth_completes(), 0U);
    f.transport.push_inbound(
        wiredtest::control_frame(proto::encode(proto::AuthComplete{0}).value()));
    auto refused = f.session.poll_transport();
    ASSERT_FALSE(refused.has_value());
    EXPECT_EQ(refused.error().code(), aa::ErrorCode::transport_unauthorized_peer);
    EXPECT_FALSE(f.session.authenticated());
    EXPECT_EQ(f.session.auth_completes(), 0U);
    f.transport.fail_send_at(std::nullopt);
    f.transport.push_inbound(
        wiredtest::control_frame(proto::encode(proto::EncapsulatedSsl{{std::byte{0x01}}}).value()));
    ASSERT_TRUE(f.session.poll_transport().has_value());
    EXPECT_TRUE(f.session.authenticated());
    EXPECT_EQ(f.session.auth_completes(), 1U);
    auto frame = tr::decode_frame(f.transport.last_sent());
    ASSERT_TRUE(frame.has_value());
    auto complete = proto::decode_auth_complete(frame.value().payload);
    ASSERT_TRUE(complete.has_value());
    EXPECT_EQ(complete.value().status, 0);
}

TEST(WiredE2eAuth, InboundAuthCompleteAfterSslIsRefusedWithoutStateChange) {
    AuthFixture f;
    wiredtest::walk_auth(f.session, f.transport);
    ASSERT_TRUE(f.session.authenticated());
    const auto state = f.session.state();
    const auto before = f.transport.sent_count();
    f.transport.push_inbound(
        wiredtest::control_frame(proto::encode(proto::AuthComplete{0}).value()));
    auto refused = f.session.poll_transport();
    ASSERT_FALSE(refused.has_value());
    EXPECT_EQ(refused.error().code(), aa::ErrorCode::transport_unauthorized_peer);
    EXPECT_TRUE(f.session.authenticated());
    EXPECT_EQ(f.session.state(), state);
    EXPECT_EQ(f.session.auth_completes(), 1U);
    EXPECT_EQ(f.transport.sent_count(), before);
}

TEST(WiredE2eAuth, PreAuthChannelTrafficIsRefused) {
    AuthFixture f;
    ASSERT_FALSE(f.session.authenticated());

    const auto before = f.transport.sent_count();
    f.transport.push_inbound(wiredtest::open_request_frame(99));
    auto refused = f.session.poll_transport();
    ASSERT_FALSE(refused.has_value());
    EXPECT_EQ(refused.error().code(), aa::ErrorCode::transport_unauthorized_peer);
    EXPECT_EQ(f.session.channel_opens(), 0U);
    EXPECT_EQ(f.transport.sent_count(), before);
}

TEST(WiredE2eAuth, PreAuthServiceDiscoveryIsRefused) {
    AuthFixture f;
    ASSERT_FALSE(f.session.authenticated());

    f.transport.push_inbound(wiredtest::discovery_request_frame());
    auto refused = f.session.poll_transport();
    ASSERT_FALSE(refused.has_value());
    EXPECT_EQ(refused.error().code(), aa::ErrorCode::transport_unauthorized_peer);
}

TEST(WiredE2eAuth, PreAuthInputIsRefused) {
    AuthFixture f;
    ASSERT_FALSE(f.session.authenticated());

    const auto before = f.transport.sent_count();
    (void)f.session.submit_input(aa::ipc::InputEvent{aa::ipc::KeyEvent{66, true}});
    EXPECT_EQ(f.session.input_sends(), 0U);
    EXPECT_EQ(f.session.input_send_failures(), 1U);
    EXPECT_EQ(f.transport.sent_count(), before);
}

TEST(WiredE2eAuth, PostAuthControlCarriesNegotiatedCipher) {
    AuthFixture f;
    wiredtest::walk_auth(f.session, f.transport);
    ASSERT_TRUE(f.session.authenticated());

    const auto before = f.transport.sent_count();
    f.transport.push_inbound(wiredtest::discovery_request_frame());
    ASSERT_TRUE(f.session.poll_transport().has_value());
    ASSERT_GT(f.transport.sent_count(), before);

    auto frame = tr::decode_frame(f.transport.last_sent());
    ASSERT_TRUE(frame.has_value());
    EXPECT_EQ(frame.value().header.channel, 0);
    EXPECT_EQ(frame.value().header.encryption, tr::Encryption::encrypted);
    auto opened = f.cipher->open(frame.value().payload);
    ASSERT_TRUE(opened.has_value());
    // Sealed post-auth control payload decodes back to a ServiceDiscoveryResponse
    // (control id 0x0006) - proving it travelled under the negotiated cipher.
    ASSERT_GE(opened.value().size(), 2U);
    EXPECT_EQ(opened.value()[0], std::byte{0x00});
    EXPECT_EQ(opened.value()[1], std::byte{0x06});
    auto response = proto::decode_service_discovery_response(opened.value());
    ASSERT_TRUE(response.has_value());
}

TEST(WiredE2eAuth, PostAuthInputCarriesNegotiatedCipher) {
    AuthFixture f;
    wiredtest::walk_auth(f.session, f.transport);
    ASSERT_TRUE(f.session.authenticated());

    const auto before = f.transport.sent_count();
    (void)f.session.submit_input(aa::ipc::InputEvent{aa::ipc::KeyEvent{66, true}});
    ASSERT_EQ(f.session.input_sends(), 1U);
    ASSERT_GT(f.transport.sent_count(), before);

    auto frame = tr::decode_frame(f.transport.last_sent());
    ASSERT_TRUE(frame.has_value());
    EXPECT_EQ(frame.value().header.encryption, tr::Encryption::encrypted);
    auto opened = f.cipher->open(frame.value().payload);
    ASSERT_TRUE(opened.has_value());
    EXPECT_EQ(opened.value()[0], std::byte{0x80});
    EXPECT_EQ(opened.value()[1], std::byte{0x01});
}

TEST(WiredE2eAuth, AuthCompleteBeforeSslIsRefused) {
    AuthFixture f;
    f.transport.push_inbound(
        wiredtest::control_frame(proto::encode(proto::AuthComplete{0}).value()));
    auto refused = f.session.poll_transport();
    ASSERT_FALSE(refused.has_value());
    EXPECT_EQ(refused.error().code(), aa::ErrorCode::transport_unauthorized_peer);
    EXPECT_FALSE(f.session.authenticated());
    EXPECT_EQ(f.session.auth_completes(), 0U);
}

// ---- P1-A frame kind (AASDK parity) ---------------------------------------
// Pinned AASDK `ControlServiceChannel.cpp` sends ALL ten control-channel
// messages as `MessageType::SPECIFIC` (MessageType.hpp: SPECIFIC = 0,
// CONTROL = 1 << 2). Frames.hpp maps that to flags bit 2, so a plain BULK
// control-channel frame is 0x03 (SPECIFIC) and never 0x07 (CONTROL) - D19:
// the S24 phone framer ignores 0x07 while 0x03 draws responses.
constexpr std::uint8_t k_bulk_plain_specific = 0x03;
constexpr std::uint8_t k_bulk_encrypted_specific = 0x0B;

void expect_specific_control_frame(const std::vector<std::byte> &wire, bool encrypted) {
    ASSERT_GE(wire.size(), 2U);
    EXPECT_EQ(static_cast<std::uint8_t>(wire[1]),
              encrypted ? k_bulk_encrypted_specific : k_bulk_plain_specific);
    auto frame = tr::decode_frame(wire);
    ASSERT_TRUE(frame.has_value());
    EXPECT_EQ(frame.value().header.channel, 0);
    EXPECT_EQ(frame.value().header.type, tr::FrameType::bulk);
    EXPECT_EQ(frame.value().header.kind, tr::MessageKind::specific);
    EXPECT_EQ(frame.value().header.encryption,
              encrypted ? tr::Encryption::encrypted : tr::Encryption::plain);
}

TEST(WiredE2eAuth, ControlChannelFramesUseAasdkSpecificKind) {
    AuthFixture f;

    // VersionRequest (HU -> phone): already the pinned SPECIFIC form.
    ASSERT_TRUE(f.session.send_version_request(1, 6).has_value());
    expect_specific_control_frame(f.transport.last_sent(), false);

    // Inbound builders must emit the same kind as production (both directions
    // asserted against the AASDK truth, so neither side can drift to 0x07).
    expect_specific_control_frame(
        wiredtest::control_frame(proto::encode(proto::VersionResponse{1, 6, 0}).value()), false);
    expect_specific_control_frame(wiredtest::discovery_request_frame(), false);

    // Handshake chunk (encapsulated_ssl, HU -> phone).
    f.transport.push_inbound(
        wiredtest::control_frame(proto::encode(proto::VersionResponse{1, 6, 0}).value()));
    ASSERT_TRUE(f.session.poll_transport().has_value());
    expect_specific_control_frame(f.transport.last_sent(), false);

    // AuthComplete (HU -> phone at handshake completion).
    f.transport.push_inbound(
        wiredtest::control_frame(proto::encode(proto::EncapsulatedSsl{{std::byte{0x01}}}).value()));
    ASSERT_TRUE(f.session.poll_transport().has_value());
    ASSERT_TRUE(f.session.authenticated());
    expect_specific_control_frame(f.transport.last_sent(), false);

    // Post-auth encrypted control (ServiceDiscoveryResponse) keeps the kind.
    f.transport.push_inbound(wiredtest::discovery_request_frame());
    ASSERT_TRUE(f.session.poll_transport().has_value());
    expect_specific_control_frame(f.transport.last_sent(), true);
}

} // namespace
