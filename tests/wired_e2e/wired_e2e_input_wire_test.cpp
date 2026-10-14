// SPDX-License-Identifier: GPL-3.0-or-later
//
// Input wire-compatibility (finding F2): the phone-facing input payload must be
// the real AAP InputReport protobuf (message id 0x8001 + body), byte-for-byte
// identical to the pinned AASDK schema - NOT the local 24-byte IPC record. This
// builds the reference InputReport from the aap_protobuf schema (the same types
// InputSourceService::sendInputReport serializes) and asserts the adapter's
// output matches it exactly.

#include <aa/protocol/InputReport.hpp>

#include <aap_protobuf/service/inputsource/InputMessageId.pb.h>
#include <aap_protobuf/service/inputsource/message/InputReport.pb.h>
#include <aap_protobuf/service/inputsource/message/KeyEvent.pb.h>
#include <aap_protobuf/service/inputsource/message/TouchEvent.pb.h>

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace {

namespace input = aa::input;
namespace ipc = aa::ipc;
namespace proto = aa::protocol;
namespace isrc = aap_protobuf::service::inputsource;
namespace isrcmsg = aap_protobuf::service::inputsource::message;

std::vector<std::byte> reference(std::uint16_t message_id, const isrcmsg::InputReport &wire) {
    const std::string body = wire.SerializeAsString();
    std::vector<std::byte> payload;
    payload.push_back(static_cast<std::byte>((message_id >> 8U) & 0xFFU));
    payload.push_back(static_cast<std::byte>(message_id & 0xFFU));
    for (const char byte : body) {
        payload.push_back(static_cast<std::byte>(byte));
    }
    return payload;
}

std::vector<std::byte> expected_body(const std::vector<std::byte> &payload) {
    return std::vector<std::byte>(payload.begin() + 2, payload.end());
}

std::vector<std::byte> expected_key(std::uint32_t keycode, bool down, std::uint64_t ts) {
    isrcmsg::InputReport wire;
    wire.set_timestamp(ts);
    auto *key = wire.mutable_key_event()->add_keys();
    key->set_keycode(keycode);
    key->set_down(down);
    key->set_metastate(0);
    return reference(static_cast<std::uint16_t>(isrc::InputMessageId::INPUT_MESSAGE_INPUT_REPORT),
                     wire);
}

std::vector<std::byte> expected_touch(const input::NormalizedTouch &touch,
                                      isrcmsg::PointerAction action, std::uint64_t ts) {
    isrcmsg::InputReport wire;
    wire.set_timestamp(ts);
    auto *event = wire.mutable_touch_event();
    auto *pointer = event->add_pointer_data();
    pointer->set_x(touch.x);
    pointer->set_y(touch.y);
    pointer->set_pointer_id(touch.pointer_id);
    event->set_action_index(0);
    event->set_action(action);
    return reference(static_cast<std::uint16_t>(isrc::InputMessageId::INPUT_MESSAGE_INPUT_REPORT),
                     wire);
}

TEST(WiredE2eInputWire, TouchMatchesAapInputReportBytes) {
    const std::uint64_t ts = 123456789ULL;
    const input::NormalizedTouch touch{10, 20, 0, ipc::TouchAction::down};
    auto encoded = proto::encode_input_report(input::NormalizedInput{touch}, ts);
    ASSERT_TRUE(encoded.has_value());
    const auto payload = encoded.value();
    ASSERT_EQ(payload.size(), expected_touch(touch, isrcmsg::ACTION_DOWN, ts).size());
    EXPECT_EQ(payload, expected_touch(touch, isrcmsg::ACTION_DOWN, ts));
    EXPECT_EQ(payload[0], std::byte{0x80});
    EXPECT_EQ(payload[1], std::byte{0x01});

    isrcmsg::InputReport decoded;
    const auto body = expected_body(payload);
    ASSERT_TRUE(decoded.ParseFromArray(body.data(), static_cast<int>(body.size())));
    ASSERT_TRUE(decoded.has_touch_event());
    ASSERT_EQ(decoded.touch_event().pointer_data_size(), 1);
    EXPECT_EQ(decoded.touch_event().pointer_data(0).x(), 10U);
    EXPECT_EQ(decoded.touch_event().pointer_data(0).y(), 20U);
    EXPECT_EQ(decoded.touch_event().pointer_data(0).pointer_id(), 0U);
    EXPECT_EQ(decoded.touch_event().action(), isrcmsg::ACTION_DOWN);
    EXPECT_FALSE(decoded.has_key_event());
}

TEST(WiredE2eInputWire, KeyMatchesAapInputReportBytes) {
    const std::uint64_t ts = 987654321ULL;
    auto encoded =
        proto::encode_input_report(input::NormalizedInput{input::NormalizedKey{66, true}}, ts);
    ASSERT_TRUE(encoded.has_value());
    const auto payload = encoded.value();
    EXPECT_EQ(payload, expected_key(66, true, ts));
    EXPECT_EQ(payload[0], std::byte{0x80});
    EXPECT_EQ(payload[1], std::byte{0x01});

    isrcmsg::InputReport decoded;
    const auto body = expected_body(payload);
    ASSERT_TRUE(decoded.ParseFromArray(body.data(), static_cast<int>(body.size())));
    ASSERT_TRUE(decoded.has_key_event());
    ASSERT_EQ(decoded.key_event().keys_size(), 1);
    EXPECT_EQ(decoded.key_event().keys(0).keycode(), 66U);
    EXPECT_TRUE(decoded.key_event().keys(0).down());
    EXPECT_EQ(decoded.key_event().keys(0).metastate(), 0U);
    EXPECT_FALSE(decoded.has_touch_event());
}

TEST(WiredE2eInputWire, TouchActionMappingMatchesPointerAction) {
    const std::uint64_t ts = 1;
    struct Case {
        ipc::TouchAction action;
        isrcmsg::PointerAction expected;
    };
    for (const Case &c : {Case{ipc::TouchAction::down, isrcmsg::ACTION_DOWN},
                          Case{ipc::TouchAction::up, isrcmsg::ACTION_UP},
                          Case{ipc::TouchAction::move, isrcmsg::ACTION_MOVED}}) {
        auto encoded = proto::encode_input_report(
            input::NormalizedInput{input::NormalizedTouch{1, 2, 0, c.action}}, ts);
        ASSERT_TRUE(encoded.has_value());
        isrcmsg::InputReport decoded;
        const auto body = expected_body(encoded.value());
        ASSERT_TRUE(decoded.ParseFromArray(body.data(), static_cast<int>(body.size())));
        EXPECT_EQ(decoded.touch_event().action(), c.expected);
    }
}

} // namespace
