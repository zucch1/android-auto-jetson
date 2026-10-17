// SPDX-License-Identifier: GPL-3.0-or-later
#include "control_fixture.hpp"

#include <aap_protobuf/service/control/message/AudioFocusNotification.pb.h>
#include <aap_protobuf/service/control/message/ByeByeResponse.pb.h>
#include <aap_protobuf/service/control/message/NavFocusNotification.pb.h>
#include <gtest/gtest.h>

namespace {
namespace proto = aa::protocol;
namespace ctl = aap_protobuf::service::control::message;
namespace ss = aa::session;
namespace tr = aa::transport;
using wiredcontrol::WiredControl;

TEST_F(WiredControl, Unknown255ThenPingInSameReceiveContinues) {
    auto combined = wiredtest::control_frame({std::byte{0}, std::byte{255}});
    auto ping = proto::encode(proto::PingRequest{9876, false, {std::byte{0x77}}});
    ASSERT_TRUE(ping);
    const auto next = wiredtest::control_frame(std::move(ping).value());
    combined.insert(combined.end(), next.begin(), next.end());
    transport.push_inbound(std::move(combined));
    const auto before = transport.sent_count();
    ASSERT_TRUE(session.poll_transport());
    EXPECT_EQ(session.unhandled_controls(), 1u);
    ASSERT_EQ(transport.sent_count(), before + 1);
    const auto frame = tr::decode_frame(transport.last_sent());
    ASSERT_TRUE(frame);
    const auto reply = proto::decode_ping_response(frame.value().payload);
    ASSERT_TRUE(reply);
    EXPECT_EQ(reply.value().timestamp, 9876);
    EXPECT_EQ(reply.value().data, (std::vector<std::byte>{std::byte{0x77}}));
    EXPECT_FALSE(session.failure());
    expect_ping();
}

TEST_F(WiredControl, NotificationsAndUnknownIdsAreCountedWithoutRepliesAndResetPerLink) {
    const std::vector<std::uint16_t> ids{12, 14, 16, 17, 19, 20, 21, 22, 23, 24, 25,
                                        26, 255, 65535, 0x1234};
    const auto before = transport.sent_count();
    for (const auto id : ids) {
        SCOPED_TRACE(id);
        ASSERT_TRUE(receive({static_cast<std::byte>(id >> 8U), static_cast<std::byte>(id & 255U)}));
    }
    EXPECT_EQ(session.unhandled_controls(), ids.size());
    EXPECT_EQ(transport.sent_count(), before);
    expect_ping();
    session.attach_transport(&transport);
    EXPECT_EQ(session.unhandled_controls(), 0u);
}

TEST_F(WiredControl, NavFocusAndVoiceNotificationDoNotKillSession) {
    const auto before = transport.sent_count();
    ASSERT_TRUE(receive({std::byte{0}, std::byte{13}, std::byte{8}, std::byte{2}}));
    ctl::NavFocusNotification reply;
    expect_encrypted_reply(14, reply);
    EXPECT_EQ(reply.focus_type(), ctl::NAV_FOCUS_PROJECTED);
    ASSERT_TRUE(receive({std::byte{0}, std::byte{17}, std::byte{8}, std::byte{1}}));
    EXPECT_EQ(transport.sent_count(), before + 1);
    EXPECT_EQ(session.unhandled_controls(), 1u);
    expect_ping();
}

TEST_F(WiredControl, AudioFocusGainTransientDuckAndReleaseReplyThenPingSurvives) {
    for (const unsigned type : {1u, 2u, 3u, 4u}) {
        SCOPED_TRACE(type);
        const auto before = transport.sent_count();
        ASSERT_TRUE(receive({std::byte{0}, std::byte{18}, std::byte{8}, static_cast<std::byte>(type)}));
        ASSERT_EQ(transport.sent_count(), before + 1);
        ctl::AudioFocusNotification reply;
        expect_encrypted_reply(19, reply);
        EXPECT_EQ(reply.focus_state(), type == 4 ? ctl::AUDIO_FOCUS_STATE_LOSS
                                                : ctl::AUDIO_FOCUS_STATE_GAIN);
        EXPECT_FALSE(reply.unsolicited());
        EXPECT_EQ(session.unhandled_controls(), 0u);
        expect_ping();
    }
}

TEST_F(WiredControl, ByeByeRepliesBeforeGracefulStopAndDoesNotRouteFollowingPing) {
    ASSERT_TRUE(session.apply(ss::Event::negotiation_succeeded));
    auto combined = wiredtest::control_frame(
        {std::byte{0}, std::byte{15}, std::byte{8}, std::byte{1}});
    const auto ping = wiredtest::control_frame(proto::encode(proto::PingRequest{1, false, {}}).value());
    combined.insert(combined.end(), ping.begin(), ping.end());
    transport.push_inbound(std::move(combined));
    const auto before = transport.sent_count();
    ASSERT_TRUE(session.poll_transport());
    ASSERT_EQ(transport.sent_count(), before + 1);
    ctl::ByeByeResponse reply;
    expect_encrypted_reply(16, reply);
    EXPECT_EQ(reply.ByteSizeLong(), 0u);
    EXPECT_TRUE(session.token().stop_requested());
    EXPECT_EQ(session.state(), ss::State::stopping);
    EXPECT_FALSE(session.failure());
    EXPECT_EQ(session.pings_answered(), 0u);
    ASSERT_TRUE(session.apply(ss::Event::cleanup_finished));
    EXPECT_EQ(session.state(), ss::State::disconnected);
    EXPECT_FALSE(session.failure());
    EXPECT_FALSE(transport.is_open());
}

TEST_F(WiredControl, FailedByeByeReplyIsTerminalAndCannotReconnect) {
    ASSERT_TRUE(receive({std::byte{0}, std::byte{255}}));
    transport.fail_send_at(transport.sent_count());
    const auto result = receive({std::byte{0}, std::byte{15}, std::byte{8}, std::byte{1}});
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code(), aa::ErrorCode::transport_io);
    ASSERT_TRUE(session.failure());
    EXPECT_EQ(session.failure()->code(), aa::ErrorCode::transport_io);
    EXPECT_EQ(session.state(), ss::State::failed);
    EXPECT_TRUE(session.token().stop_requested());
    EXPECT_FALSE(session.apply(ss::Event::reconnect));
}

TEST_F(WiredControl, IgnoringUnknownDoesNotRelaxStrictParsingOrPhaseGuards) {
    ASSERT_TRUE(receive({std::byte{0}, std::byte{255}}));
    const std::vector<std::pair<std::vector<std::byte>, aa::ErrorCode>> cases{
        {{std::byte{0}}, aa::ErrorCode::invalid_argument},
        {{std::byte{0}, std::byte{4}}, aa::ErrorCode::transport_unauthorized_peer},
        {{std::byte{0}, std::byte{2}}, aa::ErrorCode::transport_unauthorized_peer},
        {{std::byte{0}, std::byte{11}}, aa::ErrorCode::protocol_malformed_message},
        {{std::byte{0}, std::byte{11}, std::byte{0xff}}, aa::ErrorCode::protocol_malformed_message},
        {{std::byte{0}, std::byte{7}}, aa::ErrorCode::protocol_malformed_message},
        {{std::byte{0}, std::byte{18}}, aa::ErrorCode::protocol_malformed_message},
        {{std::byte{0}, std::byte{18}, std::byte{8}, std::byte{9}}, aa::ErrorCode::protocol_malformed_message},
        {{std::byte{0}, std::byte{15}}, aa::ErrorCode::protocol_malformed_message},
        {{std::byte{0}, std::byte{15}, std::byte{8}, std::byte{0}}, aa::ErrorCode::protocol_malformed_message},
        {{std::byte{0}, std::byte{13}, std::byte{0xff}}, aa::ErrorCode::protocol_malformed_message},
        {{std::byte{0}, std::byte{13}, std::byte{8}, std::byte{3}}, aa::ErrorCode::protocol_malformed_message},
    };
    const auto before = transport.sent_count();
    for (const auto& [payload, expected] : cases) {
        const auto result = receive(payload);
        ASSERT_FALSE(result);
        EXPECT_EQ(result.error().code(), expected);
    }
    EXPECT_EQ(transport.sent_count(), before);
    EXPECT_EQ(session.unhandled_controls(), 1u);
    expect_ping();
    session.attach_transport(&transport);
    transport.push_inbound(wiredtest::control_frame({std::byte{0}, std::byte{2}, std::byte{1}}));
    const auto malformed_version = session.poll_transport();
    ASSERT_FALSE(malformed_version);
    EXPECT_EQ(malformed_version.error().code(), aa::ErrorCode::protocol_malformed_message);
    transport.push_inbound(wiredtest::control_frame({std::byte{0}, std::byte{3}, std::byte{1}}));
    const auto ssl_before_version = session.poll_transport();
    ASSERT_FALSE(ssl_before_version);
    EXPECT_EQ(ssl_before_version.error().code(), aa::ErrorCode::transport_unauthorized_peer);
}

TEST_F(WiredControl, ReplyEncodersRejectOutOfRangeEnums) {
    ASSERT_TRUE(receive({std::byte{0}, std::byte{255}}));
    const auto audio = proto::encode(proto::AudioFocusNotification{
        static_cast<proto::AudioFocusState>(99), false});
    ASSERT_FALSE(audio);
    EXPECT_EQ(audio.error().code(), aa::ErrorCode::protocol_malformed_message);
    const auto nav = proto::encode(proto::NavFocusNotification{static_cast<proto::NavFocusType>(99)});
    ASSERT_FALSE(nav);
    EXPECT_EQ(nav.error().code(), aa::ErrorCode::protocol_malformed_message);
}

TEST_F(WiredControl, UnknownBeforeAuthenticationDoesNotAdvanceAuthentication) {
    session.attach_transport(&transport);
    const auto before = transport.sent_count();
    const auto state = session.state();
    transport.push_inbound(wiredtest::control_frame({std::byte{0x12}, std::byte{0x34}}));
    ASSERT_TRUE(session.poll_transport());
    EXPECT_FALSE(session.authenticated());
    EXPECT_EQ(session.state(), state);
    EXPECT_EQ(session.version_responses(), 0u);
    EXPECT_EQ(session.auth_completes(), 0u);
    EXPECT_EQ(session.unhandled_controls(), 1u);
    EXPECT_EQ(transport.sent_count(), before);
}

TEST_F(WiredControl, UnknownDiagnosticsContainMetadataAndAreBoundedPerLink) {
    const auto before = capture.jsons.size();
    for (unsigned index = 0; index < 16; ++index) {
        ASSERT_TRUE(receive({std::byte{0xff}, std::byte{0xff}, std::byte{0xab}, std::byte{0xcd}}));
    }
    ASSERT_EQ(capture.jsons.size(), before + 8);
    for (std::size_t index = before; index < capture.jsons.size(); ++index) {
        EXPECT_NE(capture.jsons[index].find("\"msg_id\":65535"), std::string::npos);
        EXPECT_NE(capture.jsons[index].find("\"auth_phase\":2"), std::string::npos);
        EXPECT_NE(capture.jsons[index].find("\"payload_bytes\":4"), std::string::npos);
    }
    EXPECT_EQ(session.unhandled_controls(), 16u);
}

TEST_F(WiredControl, NativeNavigationReleaseIsAcknowledgedAsNative) {
    ASSERT_TRUE(receive({std::byte{0}, std::byte{13}, std::byte{8}, std::byte{1}}));
    ctl::NavFocusNotification reply;
    expect_encrypted_reply(14, reply);
    EXPECT_EQ(reply.focus_type(), ctl::NAV_FOCUS_NATIVE);
}

TEST_F(WiredControl, AbsentNavigationSelectionDefaultsToProjected) {
    ASSERT_TRUE(receive({std::byte{0}, std::byte{13}}));
    ctl::NavFocusNotification reply;
    expect_encrypted_reply(14, reply);
    EXPECT_EQ(reply.focus_type(), ctl::NAV_FOCUS_PROJECTED);
}

TEST_F(WiredControl, WirelessDisabledByeByeIsAcknowledged) { expect_modern_byebye(6); }
TEST_F(WiredControl, PowerDownByeByeIsAcknowledged) { expect_modern_byebye(7); }
TEST_F(WiredControl, UserProfileSwitchByeByeIsAcknowledged) { expect_modern_byebye(8); }

TEST_F(WiredControl, WrongTypeAlongsideValidFocusOrByeByeValueIsRejected) {
    for (const unsigned id : {13u, 15u, 18u}) {
        const auto result = receive({std::byte{0}, static_cast<std::byte>(id),
            std::byte{8}, std::byte{1}, std::byte{0x0a}, std::byte{0}});
        EXPECT_FALSE(result) << id;
        if (!result) { EXPECT_EQ(result.error().code(), aa::ErrorCode::protocol_malformed_message); }
    }
}

TEST_F(WiredControl, UnsupportedEnumAlongsideValidFocusOrByeByeValueIsRejected) {
    for (const unsigned id : {13u, 15u, 18u}) {
        const auto result = receive({std::byte{0}, static_cast<std::byte>(id),
            std::byte{8}, std::byte{1}, std::byte{8}, std::byte{99}});
        EXPECT_FALSE(result) << id;
        if (!result) { EXPECT_EQ(result.error().code(), aa::ErrorCode::protocol_malformed_message); }
    }
}
} // namespace
