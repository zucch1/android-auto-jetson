// SPDX-License-Identifier: GPL-3.0-or-later
// Service-discovery adapter (task 15): binds ServiceDiscoveryResponse to the
// pinned AASDK protobuf code. Each advertised entry carries the dynamic
// per-session Service.id (never a frame-channel ordinal) plus exactly one
// channel-bearing service field; classification failures are typed errors.
#include "Framing.hpp"
#include "MediaConfiguration.hpp"

#include <aap_protobuf/service/Service.pb.h>
#include <aap_protobuf/service/control/message/ServiceDiscoveryResponse.pb.h>
#include <aap_protobuf/service/media/shared/message/MediaCodecType.pb.h>
#include <aap_protobuf/service/media/sink/message/AudioStreamType.pb.h>

#include <aa/protocol/Messages.hpp>

#include <cstddef>
#include <span>
#include <set>
#include <vector>

namespace aa::protocol {
namespace {

namespace ctl = aap_protobuf::service::control::message;
namespace media = aap_protobuf::service::media;
using WireService = aap_protobuf::service::Service;

[[nodiscard]] Error unsupported_channel() noexcept {
    return Error{ErrorCode::protocol_unsupported_channel};
}

// One Service entry is exactly one service kind; zero or several present
// fields make the entry unclassifiable.
[[nodiscard]] int present_service_fields(const WireService& entry) noexcept {
    return static_cast<int>(entry.has_sensor_source_service())
         + static_cast<int>(entry.has_media_sink_service())
         + static_cast<int>(entry.has_input_source_service())
         + static_cast<int>(entry.has_media_source_service())
         + static_cast<int>(entry.has_bluetooth_service())
         + static_cast<int>(entry.has_radio_service())
         + static_cast<int>(entry.has_navigation_status_service())
         + static_cast<int>(entry.has_media_playback_service())
         + static_cast<int>(entry.has_phone_status_service())
         + static_cast<int>(entry.has_media_browser_service())
         + static_cast<int>(entry.has_vendor_extension_service())
         + static_cast<int>(entry.has_generic_notification_service())
         + static_cast<int>(entry.has_wifi_projection_service());
}

// Media-sink entries name their stream either by audio_type or by carrying
// video configurations; both or neither is ambiguous and rejected.
[[nodiscard]] core::Result<ChannelRole>
classify_media_sink(const aap_protobuf::service::media::sink::MediaSinkService& sink) {
    const bool audio = sink.has_audio_type();
    const bool video = sink.video_configs_size() > 0;
    if (audio == video) {
        return aasdk_adapter::malformed();
    }
    if (video) {
        return ChannelRole::video;
    }
    switch (sink.audio_type()) {
    case media::sink::message::AUDIO_STREAM_MEDIA:
        return ChannelRole::media_audio;
    case media::sink::message::AUDIO_STREAM_GUIDANCE:
        return ChannelRole::guidance_audio;
    case media::sink::message::AUDIO_STREAM_SYSTEM_AUDIO:
        return ChannelRole::system_audio;
    case media::sink::message::AUDIO_STREAM_TELEPHONY:
        return ChannelRole::speech_audio;
    }
    return aasdk_adapter::malformed();
}

[[nodiscard]] core::Result<ChannelRole> classify_service(const WireService& entry) {
    if (present_service_fields(entry) != 1) {
        return aasdk_adapter::malformed();
    }
    if (entry.has_media_sink_service()) {
        return classify_media_sink(entry.media_sink_service());
    }
    if (entry.has_sensor_source_service()) {
        return ChannelRole::sensors;
    }
    if (entry.has_input_source_service()) {
        return ChannelRole::input;
    }
    if (entry.has_media_source_service()) {
        return ChannelRole::microphone;
    }
    if (entry.has_bluetooth_service()) {
        if (!entry.bluetooth_service().has_car_address()) {
            return aasdk_adapter::malformed();
        }
        return ChannelRole::bluetooth_projection;
    }
    if (entry.has_wifi_projection_service()) {
        return ChannelRole::wifi_projection;
    }
    return unsupported_channel();
}

} // namespace

core::Result<ServiceDiscoveryResponse>
decode_service_discovery_response(std::span<const std::byte> payload) {
    const auto body =
        aasdk_adapter::control_body(payload, ControlMessageId::service_discovery_response);
    if (!body) {
        return body.error();
    }
    ctl::ServiceDiscoveryResponse wire;
    if (const auto parsed = aasdk_adapter::parse_body(wire, body.value()); !parsed) {
        return parsed.error();
    }
    if (wire.channels_size() > static_cast<int>(kMaxDiscoveredServices)) {
        return aasdk_adapter::malformed();
    }
    ServiceDiscoveryResponse message;
    std::set<std::int32_t> ids;
    std::set<ChannelRole> roles;
    message.services.reserve(static_cast<std::size_t>(wire.channels_size()));
    for (const WireService& entry : wire.channels()) {
        // proto2 required field on every nested entry, checked explicitly.
        if (!entry.has_id()) {
            return aasdk_adapter::malformed();
        }
        ServiceDescriptor descriptor;
        descriptor.id = ServiceKey{entry.id()};
        if (!descriptor.id || !ids.insert(descriptor.id.value).second) {
            return aasdk_adapter::malformed();
        }
        const auto role = classify_service(entry);
        if (!role) {
            return role.error();
        }
        descriptor.role = role.value();
        if (!roles.insert(descriptor.role).second) { return aasdk_adapter::malformed(); }
        auto config = aasdk_adapter::read_configuration(entry, descriptor.role);
        if (!config) { return config.error(); }
        descriptor.configuration = std::move(config.value());
        if (const auto valid = validate_service(descriptor); !valid) { return valid.error(); }
        message.services.push_back(std::move(descriptor));
    }
    return message;
}

core::Result<std::vector<std::byte>> encode(const ServiceDiscoveryResponse& message) {
    if (message.services.size() > kMaxDiscoveredServices) {
        return aasdk_adapter::malformed();
    }
    ctl::ServiceDiscoveryResponse wire;
    std::set<std::int32_t> ids;
    std::set<ChannelRole> roles;
    for (const ServiceDescriptor& descriptor : message.services) {
        if (!descriptor.id || !ids.insert(descriptor.id.value).second
            || !roles.insert(descriptor.role).second) {
            return aasdk_adapter::malformed();
        }
        WireService* entry = wire.add_channels();
        entry->set_id(descriptor.id.value);
        if (const auto placed = aasdk_adapter::put_configuration(*entry, descriptor); !placed) {
            return placed.error();
        }
    }
    return aasdk_adapter::framed(ControlMessageId::service_discovery_response, wire);
}

} // namespace aa::protocol
