// SPDX-License-Identifier: GPL-3.0-or-later
// Pre-28 wire-conformance tests (decision-oaa-aasdk-conformance.json action
// item 3): the independent raw-byte spec (conformance_wire_spec.hpp) is the
// truth; every adapter encode must reproduce those hand-derived bytes exactly,
// every decode must round-trip them, and every absent/strict path must fail
// closed. Fixture files (tests/replay/fixtures/conformance/, task-20
// aa-replay-fixture-v1) are hash-pinned with independent literals and must stay
// byte-identical to what the spec builds.

#include "conformance_fixture_builder.hpp"
#include "conformance_wire_spec.hpp"

#include <aap_protobuf/service/control/ControlMessageType.pb.h>
#include <aap_protobuf/service/media/shared/message/Config.pb.h>
#include <aap_protobuf/service/media/sink/MediaMessageId.pb.h>

#include <aa/protocol/Messages.hpp>
#include <aa/replay/Schema.hpp>
#include <aa/transport/Framing.hpp>

#include <fstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

namespace {
namespace proto = aa::protocol;
namespace replay = aa::replay;
namespace transport = aa::transport;
using namespace conformancewire;

// Independent SHA-256 literals for the committed fixture files (over the exact
// file bytes, as written by aa_conformance_fixture_write from the spec).
// Regenerating the files must reproduce these; a mismatch fails closed.
inline constexpr std::string_view kControlFixtureSha256 =
    "afdb66270a72823fee32fa141b76202f6b89ab560fb7fc2101a622bac693ad3e"; // PIN-CONTROL
inline constexpr std::string_view kMediaFixtureSha256 =
    "faafeeb072f63c83b5373e2b57c36312272f21fe9dde61c7100cbd2df4b34d13"; // PIN-MEDIA
inline constexpr std::string_view kUiConfigFixtureSha256 =
    "eadc5cfa18e598b96232e2fc355d04e795e351e25435b7e105bef9fd2968bc09"; // PIN-UICONFIG

std::vector<std::byte> read_file(const std::string& name) {
    std::ifstream input(std::string{AA_CONFORMANCE_FIXTURE_DIR} + "/" + name,
                        std::ios::binary);
    std::vector<std::byte> out;
    for (char byte = 0; input.get(byte);) {
        out.push_back(static_cast<std::byte>(byte));
    }
    return out;
}

std::vector<std::vector<std::byte>> transport_frames(const replay::Fixture& fixture) {
    std::vector<std::vector<std::byte>> frames;
    for (const auto& record : fixture.records) {
        if (const auto* body = std::get_if<replay::TransportRecord>(&record.body)) {
            frames.push_back(body->frame);
        }
    }
    return frames;
}

} // namespace

// ---------------------------------------------------------------------------
// Implemented messages: exact-byte + round-trip against the hand-derived spec.
// ---------------------------------------------------------------------------

TEST(ReplayConformance, VersionRequestEncodeMatchesHandDerivedBytes) {
    // Given: the OAA-observed v1.1 request (02-version-ssl-auth.md:83-94).
    const proto::VersionRequest request{1, 1};
    // When: the adapter encodes it.
    const auto payload = proto::encode(request);
    // Then: the bytes are exactly the spec literal.
    ASSERT_TRUE(payload.has_value());
    EXPECT_EQ(payload.value(), F_A_version_request_v1_1_payload);
}

TEST(ReplayConformance, VersionRequestDecodeRoundTripsSpecBytes) {
    // Given: the spec payload.
    const auto decoded = proto::decode_version_request(F_A_version_request_v1_1_payload);
    // Then: it decodes to the documented value.
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(decoded.value(), (proto::VersionRequest{1, 1}));
}

TEST(ReplayConformance, VersionRequestFailsClosedOnResponseAndForeignIds) {
    // Given: the 8-byte version RESPONSE (OAA 02:103-125) and a media payload.
    // Then: the request decoder rejects both (wrong discriminator / wrong body).
    const auto on_response = proto::decode_version_request(F_B_version_response_v1_7_payload);
    ASSERT_FALSE(on_response.has_value());
    EXPECT_EQ(on_response.error().code(), aa::ErrorCode::protocol_malformed_message);
    const auto on_media = proto::decode_version_request(F_K_update_ui_config_request_800A_payload);
    ASSERT_FALSE(on_media.has_value());
    EXPECT_EQ(on_media.error().code(), aa::ErrorCode::protocol_malformed_message);
    // And: a 6-byte body under the request id is rejected (body must be 4 bytes).
    const auto long_body = proto::decode_version_request(
        bytes({0x00, 0x01, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00}));
    ASSERT_FALSE(long_body.has_value());
    EXPECT_EQ(long_body.error().code(), aa::ErrorCode::protocol_malformed_message);
}

TEST(ReplayConformance, ControlFrameLayoutMatchesArchitectureFlags) {
    // Given: the OAA architecture flags table (PLAIN|CONTROL|BULK = 0x07,
    // PLAIN|SPECIFIC|BULK = 0x03).
    // When: the project-owned framer wraps the spec payloads.
    auto control = transport::encode_message(
        transport::Message{std::uint8_t{0}, transport::MessageKind::control,
                           F_A_version_request_v1_1_payload},
        transport::Encryption::plain, nullptr);
    ASSERT_TRUE(control.has_value());
    ASSERT_EQ(control.value().size(), 1u);
    // Then: the frame bytes equal the hand-derived frame literal.
    EXPECT_EQ(control.value().front(), F_A_version_request_v1_1_frame);

    auto specific = transport::encode_message(
        transport::Message{kDynamicVideoServiceByte, transport::MessageKind::specific,
                           F_N_av_setup_request_8000_payload},
        transport::Encryption::plain, nullptr);
    ASSERT_TRUE(specific.has_value());
    ASSERT_EQ(specific.value().size(), 1u);
    EXPECT_EQ(specific.value().front(), F_N_av_setup_request_8000_frame);
}

TEST(ReplayConformance, ServiceDiscoveryResponseMatchesHandDerivedBytes) {
    // Given: one video service (id 99, 1280x720@30 H264_BP) exactly as derived
    // from the rulebook field tags in the spec header.
    const proto::ServiceDiscoveryResponse response{
        {{proto::ServiceKey{99}, proto::ChannelRole::video,
          proto::VideoConfiguration{{{1280, 720, 30}}}}}};
    // When: the adapter encodes it.
    const auto payload = proto::encode(response);
    // Then: the bytes are exactly the spec literal (protobuf field order and
    // enum values verified against OAA independently).
    ASSERT_TRUE(payload.has_value());
    EXPECT_EQ(payload.value(), F_E_service_discovery_response_payload);
    // And: the spec bytes decode back to the same value.
    const auto decoded = proto::decode_service_discovery_response(
        F_E_service_discovery_response_payload);
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(decoded.value(), response);
}

TEST(ReplayConformance, ChannelOpenAndPingMatchHandDerivedBytes) {
    // Given: the spec payloads for open/ping (zigzag priority pinned).
    const proto::ChannelOpenRequest open{1000, proto::ServiceKey{99}};
    const auto open_bytes = proto::encode(open);
    ASSERT_TRUE(open_bytes.has_value());
    EXPECT_EQ(open_bytes.value(), F_F_channel_open_request_payload);
    const auto open_back = proto::decode_channel_open_request(F_F_channel_open_request_payload);
    ASSERT_TRUE(open_back.has_value());
    EXPECT_EQ(open_back.value(), open);

    const proto::ChannelOpenResponse ok{proto::ChannelOpenStatus::success};
    const auto ok_bytes = proto::encode(ok);
    ASSERT_TRUE(ok_bytes.has_value());
    EXPECT_EQ(ok_bytes.value(), F_G_channel_open_response_success_payload);
    const auto ok_back =
        proto::decode_channel_open_response(F_G_channel_open_response_success_payload);
    ASSERT_TRUE(ok_back.has_value());
    EXPECT_EQ(ok_back.value(), ok);

    // The -250 sign-extended 10-byte varint is part of the wire contract.
    const proto::ChannelOpenResponse unsupported{proto::ChannelOpenStatus::unsupported};
    const auto no_bytes = proto::encode(unsupported);
    ASSERT_TRUE(no_bytes.has_value());
    EXPECT_EQ(no_bytes.value(), F_H_channel_open_response_unsupported_payload);
    const auto no_back =
        proto::decode_channel_open_response(F_H_channel_open_response_unsupported_payload);
    ASSERT_TRUE(no_back.has_value());
    EXPECT_EQ(no_back.value(), unsupported);

    const proto::PingRequest ping{42, true, {std::byte{0xDE}, std::byte{0xAD}}};
    const auto ping_bytes = proto::encode(ping);
    ASSERT_TRUE(ping_bytes.has_value());
    EXPECT_EQ(ping_bytes.value(), F_I_ping_request_payload);
    const auto ping_back = proto::decode_ping_request(F_I_ping_request_payload);
    ASSERT_TRUE(ping_back.has_value());
    EXPECT_EQ(ping_back.value(), ping);

    // F-J: the spec-minimal form (data ABSENT) decodes to the value.
    const proto::PingResponse pong{42, {}};
    const auto pong_back = proto::decode_ping_response(F_J_ping_response_payload);
    ASSERT_TRUE(pong_back.has_value());
    EXPECT_EQ(pong_back.value(), pong);
    // F-J2: our encoder form (explicit-empty data) is pinned exactly and is
    // bytewise distinct from the minimal form (discrepancy D24, PROPOSED
    // canonicalization - accepted encoder behavior is not changed here).
    const auto pong_bytes = proto::encode(pong);
    ASSERT_TRUE(pong_bytes.has_value());
    EXPECT_EQ(pong_bytes.value(), F_J2_ping_response_encoder_form_payload);
    EXPECT_NE(F_J2_ping_response_encoder_form_payload, F_J_ping_response_payload);
    const auto pong_back_explicit =
        proto::decode_ping_response(F_J2_ping_response_encoder_form_payload);
    ASSERT_TRUE(pong_back_explicit.has_value());
    EXPECT_EQ(pong_back_explicit.value(), pong);
}

// ---------------------------------------------------------------------------
// Fail-closed anchors: absent messages must never be silently consumed.
// ---------------------------------------------------------------------------

TEST(ReplayConformance, MediaAndUiConfigPayloadsAreNeverDecodedAsControl) {
    // Given: media-channel payloads (0x800A anchor, 0x8012 reply, nav focus).
    // Then: every control decoder rejects them (wrong discriminator).
    for (const auto* payload : {&F_K_update_ui_config_request_800A_payload,
                                &F_L_update_hu_ui_config_response_8012_payload,
                                &F_T_nav_focus_request_000D_payload}) {
        const auto as_sdp = proto::decode_service_discovery_response(*payload);
        ASSERT_FALSE(as_sdp.has_value());
        EXPECT_EQ(as_sdp.error().code(), aa::ErrorCode::protocol_malformed_message);
        const auto as_open = proto::decode_channel_open_response(*payload);
        ASSERT_FALSE(as_open.has_value());
        EXPECT_EQ(as_open.error().code(), aa::ErrorCode::protocol_malformed_message);
    }
}

TEST(ReplayConformance, UiConfigAnchorIdsAndDirectionsArePinned) {
    // Given: the AASDK history values (matrix rows R27-R30).
    // Then: the pinned schema's 0x800A really is named ..._REPLY (the mislabel,
    // discrepancy D1) and carries the SAME body as the 0x8009 direction.
    EXPECT_EQ(static_cast<int>(
                  aap_protobuf::service::media::sink::MediaMessageId::
                      MEDIA_MESSAGE_UPDATE_UI_CONFIG_REQUEST),
              0x8009);
    EXPECT_EQ(static_cast<int>(
                  aap_protobuf::service::media::sink::MediaMessageId::
                      MEDIA_MESSAGE_UPDATE_UI_CONFIG_REPLY),
              0x800A);
    EXPECT_EQ(F_K_update_ui_config_request_800A_payload[0], std::byte{0x80});
    EXPECT_EQ(F_K_update_ui_config_request_800A_payload[1], std::byte{0x0A});
    EXPECT_EQ(F_M_update_ui_config_request_8009_payload[1], std::byte{0x09});
    // Same message body in both directions (OAA: one request, no reply implied).
    EXPECT_EQ(std::vector<std::byte>(F_K_update_ui_config_request_800A_payload.begin() + 2,
                                     F_K_update_ui_config_request_800A_payload.end()),
              std::vector<std::byte>(F_M_update_ui_config_request_8009_payload.begin() + 2,
                                     F_M_update_ui_config_request_8009_payload.end()));
    // And: the true reply id 0x8012 (32786) is ABSENT from the pinned enum,
    // which stops at 0x800B (discrepancy D2).
    EXPECT_EQ(static_cast<int>(
                  aap_protobuf::service::media::sink::MediaMessageId::
                      MEDIA_MESSAGE_AUDIO_UNDERFLOW_NOTIFICATION),
              0x800B);
    EXPECT_LT(static_cast<int>(
                  aap_protobuf::service::media::sink::MediaMessageId::
                      MEDIA_MESSAGE_AUDIO_UNDERFLOW_NOTIFICATION),
              0x8012);
    EXPECT_EQ(F_L_update_hu_ui_config_response_8012_payload[1], std::byte{0x12});
}

TEST(ReplayConformance, ContestedSetupStatusBytesArePinnedDistinct) {
    // Given: the contested 0x8003 status=1 bytes (discrepancy D4: WAIT vs FAIL).
    // Then: the literal is pinned and distinct from the safe status=2 form.
    EXPECT_EQ(F_O2_av_setup_response_8003_contested_payload,
              bytes({0x80, 0x03, 0x08, 0x01}));
    EXPECT_NE(F_O2_av_setup_response_8003_contested_payload,
              F_O_av_setup_response_8003_ok_payload);
    // And: the AASDK reading of the value is pinned (STATUS_WAIT=1, STATUS_READY=2)
    // so the conflict with OAA (FAIL=1 OK=2) cannot be silently normalized away.
    EXPECT_EQ(static_cast<int>(
                  aap_protobuf::service::media::shared::message::Config::STATUS_WAIT),
              1);
    EXPECT_EQ(static_cast<int>(
                  aap_protobuf::service::media::shared::message::Config::STATUS_READY),
              2);
}

// ---------------------------------------------------------------------------
// Fixture files: hash-pinned, spec-identical, loadable through the task-20 gate.
// ---------------------------------------------------------------------------

TEST(ReplayConformance, ControlFixtureFileIsHashPinnedAndSpecIdentical) {
    // Given: the committed control fixture file and a fresh spec build.
    const auto file = read_file("conformance-control.json");
    ASSERT_FALSE(file.empty());
    auto built = build(&fill_control);
    ASSERT_TRUE(built.has_value());
    // Then: byte-identical to the spec-built export and hash-pinned.
    EXPECT_EQ(file, built.value().file_bytes);
    EXPECT_EQ(built.value().sha256_hex, kControlFixtureSha256);
    // And: it passes the task-20 loader (SHA-256 + canonical + privacy gates).
    const auto loaded = replay::load_fixture(file);
    ASSERT_TRUE(loaded.has_value());
    // And: every transport record frame is a spec literal, in spec order.
    const auto frames = transport_frames(loaded.value());
    const std::vector<std::vector<std::byte>> expected = {
        F_A_version_request_v1_1_frame,         F_B_version_response_v1_7_frame,
        F_E_service_discovery_response_frame,   F_D_service_discovery_request_frame,
        F_F_channel_open_request_frame,         F_G_channel_open_response_success_frame,
        F_I_ping_request_frame,                 F_J_ping_response_frame};
    EXPECT_EQ(frames, expected);
}

TEST(ReplayConformance, MediaFixtureFileIsHashPinnedAndSpecIdentical) {
    const auto file = read_file("conformance-media.json");
    ASSERT_FALSE(file.empty());
    auto built = build(&fill_media);
    ASSERT_TRUE(built.has_value());
    EXPECT_EQ(file, built.value().file_bytes);
    EXPECT_EQ(built.value().sha256_hex, kMediaFixtureSha256);
    const auto loaded = replay::load_fixture(file);
    ASSERT_TRUE(loaded.has_value());
    const auto frames = transport_frames(loaded.value());
    const std::vector<std::vector<std::byte>> expected = {
        F_N_av_setup_request_8000_frame,        F_O_av_setup_response_8003_ok_frame,
        F_P_av_start_indication_8001_frame,     F_Q_av_stop_indication_8002_frame,
        F_R_video_focus_request_8007_frame,     F_S_video_focus_indication_8008_frame,
        F_O2_av_setup_response_8003_contested_frame};
    EXPECT_EQ(frames, expected);
}

TEST(ReplayConformance, UiConfigFixtureFileIsHashPinnedAndSpecIdentical) {
    const auto file = read_file("conformance-uiconfig.json");
    ASSERT_FALSE(file.empty());
    auto built = build(&fill_uiconfig);
    ASSERT_TRUE(built.has_value());
    EXPECT_EQ(file, built.value().file_bytes);
    EXPECT_EQ(built.value().sha256_hex, kUiConfigFixtureSha256);
    const auto loaded = replay::load_fixture(file);
    ASSERT_TRUE(loaded.has_value());
    const auto frames = transport_frames(loaded.value());
    const std::vector<std::vector<std::byte>> expected = {
        F_M_update_ui_config_request_8009_frame, F_K_update_ui_config_request_800A_frame,
        F_L_update_hu_ui_config_response_8012_frame};
    EXPECT_EQ(frames, expected);
}

TEST(ReplayConformance, TamperedFixtureFileIsRejectedByHashGate) {
    // Given: the committed control fixture with one byte flipped in its hash.
    auto file = read_file("conformance-control.json");
    ASSERT_FALSE(file.empty());
    const auto marker = std::string_view{"\"sha256\":\""};
    std::size_t pos = 0;
    std::string text(file.size(), '\0');
    for (std::size_t i = 0; i < file.size(); ++i) {
        text[i] = static_cast<char>(file[i]);
    }
    pos = text.find(marker);
    ASSERT_NE(pos, std::string::npos);
    const std::size_t hash_start = pos + marker.size();
    text[hash_start] = (text[hash_start] == 'a') ? 'b' : 'a';
    std::vector<std::byte> tampered;
    tampered.reserve(text.size());
    for (const char byte : text) {
        tampered.push_back(static_cast<std::byte>(byte));
    }
    // Then: the loader fails closed.
    const auto loaded = replay::load_fixture(tampered);
    ASSERT_FALSE(loaded.has_value());
}
