// SPDX-License-Identifier: GPL-3.0-or-later
#include "Framing.hpp"
#include "UnknownFields.hpp"

#include <aa/protocol/AvSink.hpp>
#include <aa/transport/Frames.hpp>

#include <aap_protobuf/service/media/shared/message/Setup.pb.h>
#include <aap_protobuf/service/media/shared/message/Start.pb.h>
#include <aap_protobuf/service/media/shared/message/Stop.pb.h>
#include <aap_protobuf/service/media/shared/message/Config.pb.h>
#include <aap_protobuf/service/media/sink/MediaMessageId.pb.h>
#include <aap_protobuf/service/media/source/message/Ack.pb.h>
#include <aap_protobuf/service/media/video/message/VideoFocusRequestNotification.pb.h>
#include <aap_protobuf/service/media/video/message/VideoFocusNotification.pb.h>

namespace aa::protocol {
namespace media = aap_protobuf::service::media;
namespace shared = media::shared::message;
namespace video = media::video::message;
namespace ids = media::sink;
static_assert(ids::MEDIA_MESSAGE_SETUP == 0x8000);
static_assert(ids::MEDIA_MESSAGE_START == 0x8001);
static_assert(ids::MEDIA_MESSAGE_STOP == 0x8002);
static_assert(ids::MEDIA_MESSAGE_CONFIG == 0x8003);
static_assert(ids::MEDIA_MESSAGE_ACK == 0x8004);
static_assert(ids::MEDIA_MESSAGE_VIDEO_FOCUS_REQUEST == 0x8007);
static_assert(ids::MEDIA_MESSAGE_VIDEO_FOCUS_NOTIFICATION == 0x8008);
static_assert(ids::MEDIA_MESSAGE_DATA == 0x0000);
static_assert(ids::MEDIA_MESSAGE_CODEC_CONFIG == 0x0001);
static_assert(shared::Config::STATUS_READY == 2);
static_assert(static_cast<int>(AvSinkCodec::pcm) == shared::MEDIA_CODEC_AUDIO_PCM);
static_assert(static_cast<int>(AvSinkCodec::h264) == shared::MEDIA_CODEC_VIDEO_H264_BP);
static_assert(static_cast<int>(VideoFocusMode::projected) == video::VIDEO_FOCUS_PROJECTED);
static_assert(static_cast<int>(VideoFocusMode::native) == video::VIDEO_FOCUS_NATIVE);
static_assert(static_cast<int>(VideoFocusMode::native_transient) == video::VIDEO_FOCUS_NATIVE_TRANSIENT);
static_assert(static_cast<int>(VideoFocusMode::projected_no_input_focus) == video::VIDEO_FOCUS_PROJECTED_NO_INPUT_FOCUS);

namespace {
template <typename Wire>
core::Result<void> parse(Wire& wire, std::span<const std::byte> payload,
                         std::uint16_t id, std::span<const int> tags) {
    const auto body = aasdk_adapter::control_body(payload, static_cast<ControlMessageId>(id));
    if (!body) { return body.error(); }
    if (auto parsed = aasdk_adapter::parse_body(wire, body.value()); !parsed) {
        return parsed.error();
    }
    if (aasdk_adapter::classify_unknown_fields(wire, tags)
            .rejected_under(aasdk_adapter::kStrictExtensionPolicy)) {
        return aasdk_adapter::malformed();
    }
    return {};
}

bool valid_focus(VideoFocusMode mode) {
    switch (mode) {
    case VideoFocusMode::projected:
    case VideoFocusMode::native:
    case VideoFocusMode::native_transient:
    case VideoFocusMode::projected_no_input_focus: return true;
    }
    return false;
}

core::Result<std::span<const std::byte>> media_body(std::span<const std::byte> payload,
                                                  std::uint16_t id) {
    if (payload.size() > transport::kMaxMessageBytes) { return aasdk_adapter::malformed(); }
    return aasdk_adapter::control_body(payload, static_cast<ControlMessageId>(id));
}
} // namespace

core::Result<AvSinkSetup> decode_av_sink_setup(std::span<const std::byte> payload) {
    shared::Setup wire;
    constexpr int tags[] = {1};
    if (auto parsed = parse(wire, payload, ids::MEDIA_MESSAGE_SETUP, tags); !parsed) {
        return parsed.error();
    }
    switch (wire.type()) {
    case shared::MEDIA_CODEC_AUDIO_PCM: return AvSinkSetup{AvSinkCodec::pcm};
    case shared::MEDIA_CODEC_VIDEO_H264_BP: return AvSinkSetup{AvSinkCodec::h264};
    default: return aasdk_adapter::malformed();
    }
}

core::Result<AvSinkStart> decode_av_sink_start(std::span<const std::byte> payload) {
    shared::Start wire;
    constexpr int tags[] = {1, 2};
    if (auto parsed = parse(wire, payload, ids::MEDIA_MESSAGE_START, tags); !parsed) {
        return parsed.error();
    }
    return AvSinkStart{wire.session_id(), wire.configuration_index()};
}

core::Result<AvSinkStop> decode_av_sink_stop(std::span<const std::byte> payload) {
    shared::Stop wire;
    if (auto parsed = parse(wire, payload, ids::MEDIA_MESSAGE_STOP, {}); !parsed) {
        return parsed.error();
    }
    return AvSinkStop{};
}

core::Result<VideoFocusRequest> decode_video_focus_request(std::span<const std::byte> payload) {
    video::VideoFocusRequestNotification wire;
    constexpr int tags[] = {1, 2, 3};
    if (auto parsed = parse(wire, payload, ids::MEDIA_MESSAGE_VIDEO_FOCUS_REQUEST, tags); !parsed) {
        return parsed.error();
    }
    VideoFocusRequest request;
    if (wire.has_mode()) { request.mode = static_cast<VideoFocusMode>(wire.mode()); }
    return request;
}

core::Result<AvSinkData> decode_av_sink_data(std::span<const std::byte> payload) {
    const auto body = media_body(payload, ids::MEDIA_MESSAGE_DATA);
    if (!body) { return body.error(); }
    constexpr std::size_t timestamp_bytes = 8;
    if (body.value().size() <= timestamp_bytes) { return aasdk_adapter::malformed(); }
    std::uint64_t timestamp = 0;
    for (const auto byte : body.value().first(timestamp_bytes)) {
        timestamp = (timestamp << 8U) | static_cast<std::uint64_t>(byte);
    }
    return AvSinkData{timestamp, body.value().subspan(timestamp_bytes)};
}

core::Result<std::span<const std::byte>> decode_av_sink_codec_config(std::span<const std::byte> payload) {
    return media_body(payload, ids::MEDIA_MESSAGE_CODEC_CONFIG);
}

core::Result<std::vector<std::byte>> encode(const AvSinkSetup& setup) {
    shared::Setup wire;
    switch (setup.codec) {
    case AvSinkCodec::pcm: wire.set_type(shared::MEDIA_CODEC_AUDIO_PCM); break;
    case AvSinkCodec::h264: wire.set_type(shared::MEDIA_CODEC_VIDEO_H264_BP); break;
    default: return aasdk_adapter::malformed();
    }
    return aasdk_adapter::framed_id(ids::MEDIA_MESSAGE_SETUP, wire);
}

core::Result<std::vector<std::byte>> encode(const AvSinkStart& start) {
    shared::Start wire;
    wire.set_session_id(start.session_id);
    wire.set_configuration_index(start.configuration_index);
    return aasdk_adapter::framed_id(ids::MEDIA_MESSAGE_START, wire);
}

core::Result<std::vector<std::byte>> encode(const AvSinkStop&) {
    return aasdk_adapter::framed_id(ids::MEDIA_MESSAGE_STOP, shared::Stop{});
}

core::Result<std::vector<std::byte>> encode(const VideoFocusRequest& request) {
    video::VideoFocusRequestNotification wire;
    if (request.mode) {
        if (!valid_focus(*request.mode)) { return aasdk_adapter::malformed(); }
        wire.set_mode(static_cast<video::VideoFocusMode>(*request.mode));
    }
    return aasdk_adapter::framed_id(ids::MEDIA_MESSAGE_VIDEO_FOCUS_REQUEST, wire);
}

core::Result<std::vector<std::byte>> encode(const VideoFocusNotification& notification) {
    video::VideoFocusNotification wire;
    if (notification.focus) {
        if (!valid_focus(*notification.focus)) { return aasdk_adapter::malformed(); }
        wire.set_focus(static_cast<video::VideoFocusMode>(*notification.focus));
    }
    if (notification.unsolicited) { wire.set_unsolicited(*notification.unsolicited); }
    return aasdk_adapter::framed_id(ids::MEDIA_MESSAGE_VIDEO_FOCUS_NOTIFICATION, wire);
}

core::Result<std::vector<std::byte>> encode_av_sink_ready() {
    shared::Config wire;
    wire.set_status(shared::Config::STATUS_READY);
    wire.set_max_unacked(1);
    wire.add_configuration_indices(0);
    return aasdk_adapter::framed_id(ids::MEDIA_MESSAGE_CONFIG, wire);
}

core::Result<std::vector<std::byte>> encode_av_sink_ack(std::int32_t session_id) {
    media::source::message::Ack wire;
    wire.set_session_id(session_id);
    wire.set_ack(1);
    return aasdk_adapter::framed_id(ids::MEDIA_MESSAGE_ACK, wire);
}
} // namespace aa::protocol
