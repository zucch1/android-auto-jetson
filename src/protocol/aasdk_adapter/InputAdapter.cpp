// SPDX-License-Identifier: GPL-3.0-or-later
// Input-source adapter (finding F2): binds the project-owned normalized input
// shape to the pinned AASDK InputReport protobuf so a phone receives the real
// AAP input message (message id 0x8001 + InputReport body), not the local IPC
// record. Every conversion is explicit; the message id is framed big-endian
// exactly as InputSourceService::sendInputReport writes it.
#include "Framing.hpp"

#include <aap_protobuf/service/inputsource/InputMessageId.pb.h>
#include <aap_protobuf/service/inputsource/message/InputReport.pb.h>
#include <aap_protobuf/service/inputsource/message/KeyEvent.pb.h>
#include <aap_protobuf/service/inputsource/message/TouchEvent.pb.h>

#include <aa/protocol/InputReport.hpp>

#include <cstdint>
#include <type_traits>
#include <variant>
#include <vector>

namespace aa::protocol {
namespace {

namespace isrc = aap_protobuf::service::inputsource;
namespace isrcmsg = aap_protobuf::service::inputsource::message;

[[nodiscard]] isrcmsg::PointerAction pointer_action(ipc::TouchAction action) noexcept {
    switch (action) {
    case ipc::TouchAction::down:
        return isrcmsg::ACTION_DOWN;
    case ipc::TouchAction::up:
        return isrcmsg::ACTION_UP;
    case ipc::TouchAction::move:
        return isrcmsg::ACTION_MOVED;
    }
    return isrcmsg::ACTION_DOWN;
}

} // namespace

core::Result<std::vector<std::byte>> encode_input_report(const input::NormalizedInput &event,
                                                         std::uint64_t timestamp_ns) {
    isrcmsg::InputReport wire;
    wire.set_timestamp(timestamp_ns);
    std::visit(
        [&wire](const auto &normalized) {
            using T = std::decay_t<decltype(normalized)>;
            if constexpr (std::is_same_v<T, input::NormalizedKey>) {
                auto *key = wire.mutable_key_event()->add_keys();
                key->set_keycode(normalized.keycode);
                key->set_down(normalized.down);
                key->set_metastate(0);
            } else {
                auto *touch = wire.mutable_touch_event();
                auto *pointer = touch->add_pointer_data();
                pointer->set_x(normalized.x);
                pointer->set_y(normalized.y);
                pointer->set_pointer_id(normalized.pointer_id);
                touch->set_action_index(0);
                touch->set_action(pointer_action(normalized.action));
            }
        },
        event);
    return aasdk_adapter::framed_id(
        static_cast<std::uint16_t>(isrc::InputMessageId::INPUT_MESSAGE_INPUT_REPORT), wire);
}

} // namespace aa::protocol
