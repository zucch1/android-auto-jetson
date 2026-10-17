// SPDX-License-Identifier: GPL-3.0-or-later
#include "Framing.hpp"
#include "UnknownFields.hpp"

#include <aa/protocol/Microphone.hpp>

#include <aap_protobuf/service/media/shared/message/Setup.pb.h>
#include <aap_protobuf/service/media/shared/message/Config.pb.h>
#include <aap_protobuf/service/media/sink/MediaMessageId.pb.h>
#include <aap_protobuf/service/media/source/message/MicrophoneRequest.pb.h>
#include <aap_protobuf/service/media/source/message/MicrophoneResponse.pb.h>
#include <aap_protobuf/service/media/source/message/Ack.pb.h>
#include <aap_protobuf/shared/MessageStatus.pb.h>

namespace aa::protocol {
namespace media = aap_protobuf::service::media;
namespace shared = media::shared::message;
namespace source = media::source::message;
namespace ids = media::sink;
static_assert(ids::MEDIA_MESSAGE_SETUP == 0x8000);
static_assert(ids::MEDIA_MESSAGE_CONFIG == 0x8003);
static_assert(ids::MEDIA_MESSAGE_ACK == 0x8004);
static_assert(ids::MEDIA_MESSAGE_MICROPHONE_REQUEST == 0x8005);
static_assert(ids::MEDIA_MESSAGE_MICROPHONE_RESPONSE == 0x8006);
static_assert(shared::Config::STATUS_READY == 2);
static_assert(aap_protobuf::shared::STATUS_COMMAND_NOT_SUPPORTED == kMicrophoneCaptureUnsupported);

namespace {
template <typename Wire>
core::Result<void> parse(Wire& wire, std::span<const std::byte> payload,
                         std::uint16_t id, std::span<const int> tags) {
    const auto body = aasdk_adapter::control_body(payload, static_cast<ControlMessageId>(id));
    if (!body) { return body.error(); }
    if (const auto parsed = aasdk_adapter::parse_body(wire, body.value()); !parsed) {
        return parsed.error();
    }
    if (aasdk_adapter::classify_unknown_fields(wire, tags)
            .rejected_under(aasdk_adapter::kStrictExtensionPolicy)) {
        return aasdk_adapter::malformed();
    }
    return {};
}
} // namespace

core::Result<MicrophoneSetup> decode_microphone_setup(std::span<const std::byte> payload) {
    shared::Setup wire;
    constexpr int tags[] = {1};
    if (auto parsed = parse(wire, payload, ids::MEDIA_MESSAGE_SETUP, tags); !parsed) {
        return parsed.error();
    }
    if (wire.type() != shared::MEDIA_CODEC_AUDIO_PCM) { return aasdk_adapter::malformed(); }
    return MicrophoneSetup{};
}

core::Result<MicrophoneRequest> decode_microphone_request(std::span<const std::byte> payload) {
    source::MicrophoneRequest wire;
    constexpr int tags[] = {1, 2, 3, 4};
    if (auto parsed = parse(wire, payload, ids::MEDIA_MESSAGE_MICROPHONE_REQUEST, tags); !parsed) {
        return parsed.error();
    }
    MicrophoneRequest request{wire.open()};
    if (wire.has_anc_enabled()) { request.anc_enabled = wire.anc_enabled(); }
    if (wire.has_ec_enabled()) { request.ec_enabled = wire.ec_enabled(); }
    if (wire.has_max_unacked()) { request.max_unacked = wire.max_unacked(); }
    return request;
}

core::Result<MicrophoneResponse> decode_microphone_response(std::span<const std::byte> payload) {
    source::MicrophoneResponse wire;
    constexpr int tags[] = {1, 2};
    if (auto parsed = parse(wire, payload, ids::MEDIA_MESSAGE_MICROPHONE_RESPONSE, tags); !parsed) {
        return parsed.error();
    }
    if (wire.has_session_id()) { return aasdk_adapter::malformed(); }
    return MicrophoneResponse{wire.status()};
}

core::Result<void> decode_microphone_ack(std::span<const std::byte> payload) {
    source::Ack wire;
    constexpr int tags[] = {1, 2, 3};
    // No capture sessions exist: validate the pinned Ack, then ignore it.
    return parse(wire, payload, ids::MEDIA_MESSAGE_ACK, tags);
}

core::Result<std::vector<std::byte>> encode(const MicrophoneSetup&) {
    shared::Setup wire;
    wire.set_type(shared::MEDIA_CODEC_AUDIO_PCM);
    return aasdk_adapter::framed_id(ids::MEDIA_MESSAGE_SETUP, wire);
}

core::Result<std::vector<std::byte>> encode(const MicrophoneRequest& request) {
    source::MicrophoneRequest wire;
    wire.set_open(request.open);
    if (request.anc_enabled) { wire.set_anc_enabled(*request.anc_enabled); }
    if (request.ec_enabled) { wire.set_ec_enabled(*request.ec_enabled); }
    if (request.max_unacked) { wire.set_max_unacked(*request.max_unacked); }
    return aasdk_adapter::framed_id(ids::MEDIA_MESSAGE_MICROPHONE_REQUEST, wire);
}

core::Result<std::vector<std::byte>> encode(const MicrophoneResponse& response) {
    source::MicrophoneResponse wire;
    // Generated int32 serialization sign-extends negative statuses to ten bytes.
    wire.set_status(response.status);
    return aasdk_adapter::framed_id(ids::MEDIA_MESSAGE_MICROPHONE_RESPONSE, wire);
}

core::Result<std::vector<std::byte>> encode_microphone_ready() {
    shared::Config wire;
    wire.set_status(shared::Config::STATUS_READY);
    wire.set_max_unacked(1);
    wire.add_configuration_indices(0);
    return aasdk_adapter::framed_id(ids::MEDIA_MESSAGE_CONFIG, wire);
}
} // namespace aa::protocol
