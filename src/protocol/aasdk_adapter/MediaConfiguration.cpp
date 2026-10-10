// SPDX-License-Identifier: GPL-3.0-or-later
#include "MediaConfiguration.hpp"
#include "Framing.hpp"
#include "UnknownFields.hpp"

#include <aap_protobuf/service/media/shared/message/MediaCodecType.pb.h>
#include <aap_protobuf/service/media/sink/message/AudioStreamType.pb.h>

namespace aa::protocol::aasdk_adapter {
namespace media = aap_protobuf::service::media;
namespace sink = media::sink::message;
namespace shared = media::shared::message;

namespace {

// Pinned-schema tag sets for the unknown-field separation (pre-28 conformance):
// entries carrying a tag from these sets could not be interpreted (unknown
// enum value or wire-type mismatch) and are malformed; entries with any other
// tag are extension fields (see UnknownFields.hpp). The strict profile rejects
// both classes - today's behavior - so this split is behavior-preserving until
// the extension policy is explicitly flipped on phone evidence.
constexpr int kServiceTags[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14};
constexpr int kMediaSinkServiceTags[] = {1, 2, 3, 4, 5, 6, 7, 8};
constexpr int kInputSourceServiceTags[] = {1, 2, 3, 4, 5};
constexpr int kVideoConfigurationTags[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};
constexpr int kAudioConfigurationTags[] = {1, 2, 3};

[[nodiscard]] bool rejects_unknown(const google::protobuf::Message& message,
                                  std::span<const int> known_tags) {
    return classify_unknown_fields(message, known_tags)
        .rejected_under(kStrictExtensionPolicy);
}

} // namespace

core::Result<ServiceConfiguration>
read_configuration(const aap_protobuf::service::Service& entry, ChannelRole role) {
    // Proto2 stores unknown optional enums as unknown fields and returns the
    // default enum value. That silent-default fallback is the malformed class
    // and is rejected here; unseen tags are the extension class and follow the
    // explicit strict policy (UnknownFields.hpp). Both rejections are the
    // pre-28 behavior, now typed instead of conflated.
    if (rejects_unknown(entry, kServiceTags)
        || (entry.has_media_sink_service()
            && rejects_unknown(entry.media_sink_service(), kMediaSinkServiceTags))) {
        return malformed();
    }
    switch (role) {
    case ChannelRole::input: {
        const auto& input = entry.input_source_service();
        if (rejects_unknown(input, kInputSourceServiceTags)
            || input.touchscreen_size() != 0 || input.touchpad_size() != 0
            || input.keycodes_supported_size() > static_cast<int>(kMaxButtonKeycodes)) {
            return malformed();
        }
        ButtonConfiguration config;
        for (const auto keycode : input.keycodes_supported()) { config.keycodes.push_back(keycode); }
        return ServiceConfiguration{std::move(config)};
    }
    case ChannelRole::video: {
        const auto& wire = entry.media_sink_service();
        if (wire.audio_configs_size() != 0 || wire.has_audio_type()
            || wire.available_type() != shared::MEDIA_CODEC_VIDEO_H264_BP
            || wire.video_configs_size() > static_cast<int>(kMaxMediaProfiles)) {
            return malformed();
        }
        VideoConfiguration config;
        for (const auto& video : wire.video_configs()) {
            if (rejects_unknown(video, kVideoConfigurationTags)
                || !video.has_codec_resolution() || !video.has_frame_rate()
                || !video.has_video_codec_type()
                || video.video_codec_type() != shared::MEDIA_CODEC_VIDEO_H264_BP
                || video.frame_rate() != sink::VIDEO_FPS_30
                || video.width_margin() != 0 || video.height_margin() != 0) {
                return malformed();
            }
            switch (video.codec_resolution()) {
            case sink::VIDEO_1280x720: config.profiles.push_back({1280, 720, 30}); break;
            case sink::VIDEO_800x480: config.profiles.push_back({800, 480, 30}); break;
            default: return malformed();
            }
        }
        return ServiceConfiguration{std::move(config)};
    }
    case ChannelRole::media_audio:
    case ChannelRole::guidance_audio:
    case ChannelRole::system_audio:
    case ChannelRole::speech_audio: {
        const auto& wire = entry.media_sink_service();
        if (wire.video_configs_size() != 0
            || wire.available_type() != shared::MEDIA_CODEC_AUDIO_PCM
            || wire.audio_configs_size() > static_cast<int>(kMaxMediaProfiles)) {
            return malformed();
        }
        AudioConfiguration config;
        for (const auto& audio : wire.audio_configs()) {
            if (rejects_unknown(audio, kAudioConfigurationTags)
                || audio.number_of_bits() > 255 || audio.number_of_channels() > 255) {
                return malformed();
            }
            config.profiles.push_back({audio.sampling_rate(),
                                      static_cast<std::uint8_t>(audio.number_of_bits()),
                                      static_cast<std::uint8_t>(audio.number_of_channels())});
        }
        return ServiceConfiguration{std::move(config)};
    }
    case ChannelRole::microphone:
    case ChannelRole::sensors:
    case ChannelRole::bluetooth_projection:
    case ChannelRole::wifi_projection:
    case ChannelRole::diagnostics:
        return Error{ErrorCode::protocol_unsupported_channel};
    }
    return malformed();
}

core::Result<void> put_configuration(aap_protobuf::service::Service& entry,
                                    const ServiceDescriptor& descriptor) {
    if (const auto valid = validate_service(descriptor); !valid) { return valid.error(); }
    switch (descriptor.role) {
    case ChannelRole::video: {
        auto* wire = entry.mutable_media_sink_service();
        wire->set_available_type(shared::MEDIA_CODEC_VIDEO_H264_BP);
        for (const auto profile : std::get<VideoConfiguration>(descriptor.configuration).profiles) {
            auto* video = wire->add_video_configs();
            video->set_codec_resolution(profile.width == 1280 ? sink::VIDEO_1280x720
                                                             : sink::VIDEO_800x480);
            video->set_frame_rate(sink::VIDEO_FPS_30);
            video->set_video_codec_type(shared::MEDIA_CODEC_VIDEO_H264_BP);
        }
        return {};
    }
    case ChannelRole::media_audio:
    case ChannelRole::guidance_audio:
    case ChannelRole::system_audio:
    case ChannelRole::speech_audio: {
        auto* wire = entry.mutable_media_sink_service();
        wire->set_available_type(shared::MEDIA_CODEC_AUDIO_PCM);
        switch (descriptor.role) {
        case ChannelRole::media_audio: wire->set_audio_type(sink::AUDIO_STREAM_MEDIA); break;
        case ChannelRole::guidance_audio: wire->set_audio_type(sink::AUDIO_STREAM_GUIDANCE); break;
        case ChannelRole::system_audio: wire->set_audio_type(sink::AUDIO_STREAM_SYSTEM_AUDIO); break;
        case ChannelRole::speech_audio: wire->set_audio_type(sink::AUDIO_STREAM_TELEPHONY); break;
        default: return malformed();
        }
        for (const auto profile : std::get<AudioConfiguration>(descriptor.configuration).profiles) {
            auto* audio = wire->add_audio_configs();
            audio->set_sampling_rate(profile.sampling_rate);
            audio->set_number_of_bits(profile.bits);
            audio->set_number_of_channels(profile.channels);
        }
        return {};
    }
    case ChannelRole::input:
        for (const auto keycode : std::get<ButtonConfiguration>(descriptor.configuration).keycodes) {
            entry.mutable_input_source_service()->add_keycodes_supported(keycode);
        }
        return {};
    case ChannelRole::microphone:
    case ChannelRole::sensors:
    case ChannelRole::bluetooth_projection:
    case ChannelRole::wifi_projection:
    case ChannelRole::diagnostics:
        return Error{ErrorCode::protocol_unsupported_channel};
    }
    return malformed();
}
} // namespace aa::protocol::aasdk_adapter
