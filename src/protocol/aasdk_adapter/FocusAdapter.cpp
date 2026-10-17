// SPDX-License-Identifier: GPL-3.0-or-later
#include "Framing.hpp"
#include "UnknownFields.hpp"

#include <aap_protobuf/service/control/ControlMessageType.pb.h>
#include <aap_protobuf/service/control/message/AudioFocusNotification.pb.h>
#include <aap_protobuf/service/control/message/AudioFocusRequest.pb.h>
#include <aap_protobuf/service/control/message/ByeByeRequest.pb.h>
#include <aap_protobuf/service/control/message/ByeByeResponse.pb.h>
#include <aap_protobuf/service/control/message/NavFocusNotification.pb.h>
#include <aap_protobuf/service/control/message/NavFocusRequestNotification.pb.h>

namespace aa::protocol {
namespace ctl = aap_protobuf::service::control::message;

static_assert(static_cast<std::uint16_t>(ControlMessageId::audio_focus_request) ==
              ctl::MESSAGE_AUDIO_FOCUS_REQUEST);
static_assert(static_cast<std::uint16_t>(ControlMessageId::audio_focus_notification) ==
              ctl::MESSAGE_AUDIO_FOCUS_NOTIFICATION);
static_assert(static_cast<std::uint16_t>(ControlMessageId::byebye_request) ==
              ctl::MESSAGE_BYEBYE_REQUEST);
static_assert(static_cast<std::uint16_t>(ControlMessageId::byebye_response) ==
              ctl::MESSAGE_BYEBYE_RESPONSE);
static_assert(static_cast<std::uint16_t>(ControlMessageId::nav_focus_request) ==
              ctl::MESSAGE_NAV_FOCUS_REQUEST);
static_assert(static_cast<std::uint16_t>(ControlMessageId::nav_focus_notification) ==
              ctl::MESSAGE_NAV_FOCUS_NOTIFICATION);

core::Result<AudioFocusRequest> decode_audio_focus_request(std::span<const std::byte> payload) {
    const auto body = aasdk_adapter::control_body(payload, ControlMessageId::audio_focus_request);
    if (!body) { return body.error(); }
    ctl::AudioFocusRequest wire;
    if (const auto parsed = aasdk_adapter::parse_body(wire, body.value()); !parsed) {
        return parsed.error();
    }
    constexpr int known_tags[]{1};
    if (aasdk_adapter::classify_unknown_fields(wire, known_tags).malformed()) {
        return aasdk_adapter::malformed();
    }
    switch (wire.audio_focus_type()) {
    case ctl::AUDIO_FOCUS_GAIN: return AudioFocusRequest{AudioFocusRequestType::gain};
    case ctl::AUDIO_FOCUS_GAIN_TRANSIENT:
        return AudioFocusRequest{AudioFocusRequestType::gain_transient};
    case ctl::AUDIO_FOCUS_GAIN_TRANSIENT_MAY_DUCK:
        return AudioFocusRequest{AudioFocusRequestType::gain_transient_may_duck};
    case ctl::AUDIO_FOCUS_RELEASE: return AudioFocusRequest{AudioFocusRequestType::release};
    default: return aasdk_adapter::malformed();
    }
}

core::Result<std::vector<std::byte>> encode(const AudioFocusNotification &message) {
    ctl::AudioFocusNotification wire;
    switch (message.state) {
    case AudioFocusState::invalid: wire.set_focus_state(ctl::AUDIO_FOCUS_STATE_INVALID); break;
    case AudioFocusState::gain: wire.set_focus_state(ctl::AUDIO_FOCUS_STATE_GAIN); break;
    case AudioFocusState::gain_transient:
        wire.set_focus_state(ctl::AUDIO_FOCUS_STATE_GAIN_TRANSIENT); break;
    case AudioFocusState::loss: wire.set_focus_state(ctl::AUDIO_FOCUS_STATE_LOSS); break;
    case AudioFocusState::loss_transient_can_duck:
        wire.set_focus_state(ctl::AUDIO_FOCUS_STATE_LOSS_TRANSIENT_CAN_DUCK); break;
    case AudioFocusState::loss_transient:
        wire.set_focus_state(ctl::AUDIO_FOCUS_STATE_LOSS_TRANSIENT); break;
    case AudioFocusState::gain_media_only:
        wire.set_focus_state(ctl::AUDIO_FOCUS_STATE_GAIN_MEDIA_ONLY); break;
    case AudioFocusState::gain_transient_guidance_only:
        wire.set_focus_state(ctl::AUDIO_FOCUS_STATE_GAIN_TRANSIENT_GUIDANCE_ONLY); break;
    default: return aasdk_adapter::malformed();
    }
    wire.set_unsolicited(message.unsolicited);
    return aasdk_adapter::framed(ControlMessageId::audio_focus_notification, wire);
}

core::Result<ByeByeRequest> decode_byebye_request(std::span<const std::byte> payload) {
    const auto body = aasdk_adapter::control_body(payload, ControlMessageId::byebye_request);
    if (!body) { return body.error(); }
    ctl::ByeByeRequest wire;
    // The pinned proto2 enum ends at 5; OAA reasons 6-8 land in unknown fields.
    if (body.value().size() > aasdk_adapter::kMaxProtoBodyBytes ||
        !wire.ParsePartialFromArray(body.value().data(), static_cast<int>(body.value().size()))) {
        return aasdk_adapter::malformed();
    }
    auto reason = wire.has_reason() ? static_cast<std::int32_t>(wire.reason()) : 0;
    bool has_reason = wire.has_reason();
    const auto& unknown = wire.GetReflection()->GetUnknownFields(wire);
    for (int index = 0; index < unknown.field_count(); ++index) {
        const auto& field = unknown.field(index);
        if (field.number() != 1) { continue; }
        if (has_reason || field.type() != google::protobuf::UnknownField::TYPE_VARINT ||
            field.varint() < 6 || field.varint() > 8) {
            return aasdk_adapter::malformed();
        }
        reason = static_cast<std::int32_t>(field.varint());
        has_reason = true;
    }
    if (!has_reason) { return aasdk_adapter::malformed(); }
    switch (reason) {
    case 1: return ByeByeRequest{ByeByeReason::user_selection};
    case 2: return ByeByeRequest{ByeByeReason::device_switch};
    case 3: return ByeByeRequest{ByeByeReason::not_supported};
    case 4: return ByeByeRequest{ByeByeReason::not_currently_supported};
    case 5: return ByeByeRequest{ByeByeReason::probe_supported};
    case 6: return ByeByeRequest{ByeByeReason::wireless_projection_disabled};
    case 7: return ByeByeRequest{ByeByeReason::power_down};
    case 8: return ByeByeRequest{ByeByeReason::user_profile_switch};
    default: return aasdk_adapter::malformed();
    }
}

core::Result<std::vector<std::byte>> encode(const ByeByeResponse &) {
    return aasdk_adapter::framed(ControlMessageId::byebye_response, ctl::ByeByeResponse{});
}

core::Result<NavFocusRequest> decode_nav_focus_request(std::span<const std::byte> payload) {
    const auto body = aasdk_adapter::control_body(payload, ControlMessageId::nav_focus_request);
    if (!body) { return body.error(); }
    ctl::NavFocusRequestNotification wire;
    if (const auto parsed = aasdk_adapter::parse_body(wire, body.value()); !parsed) {
        return parsed.error();
    }
    constexpr int known_tags[]{1};
    if (aasdk_adapter::classify_unknown_fields(wire, known_tags).malformed()) {
        return aasdk_adapter::malformed();
    }
    if (!wire.has_focus_type()) { return NavFocusRequest{}; }
    switch (wire.focus_type()) {
    case ctl::NAV_FOCUS_NATIVE: return NavFocusRequest{NavFocusType::native};
    case ctl::NAV_FOCUS_PROJECTED: return NavFocusRequest{NavFocusType::projected};
    default: return aasdk_adapter::malformed();
    }
}

core::Result<std::vector<std::byte>> encode(const NavFocusNotification &message) {
    ctl::NavFocusNotification wire;
    switch (message.type) {
    case NavFocusType::native: wire.set_focus_type(ctl::NAV_FOCUS_NATIVE); break;
    case NavFocusType::projected: wire.set_focus_type(ctl::NAV_FOCUS_PROJECTED); break;
    default: return aasdk_adapter::malformed();
    }
    return aasdk_adapter::framed(ControlMessageId::nav_focus_notification, wire);
}
} // namespace aa::protocol
