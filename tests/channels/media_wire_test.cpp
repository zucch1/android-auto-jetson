// SPDX-License-Identifier: GPL-3.0-or-later
#include "fixtures.hpp"
#include <aap_protobuf/service/control/message/ChannelOpenResponse.pb.h>
#include <aap_protobuf/service/control/message/ServiceDiscoveryResponse.pb.h>
#include <array>
#include <gtest/gtest.h>

namespace {
using namespace channeltest;
namespace ctl = aap_protobuf::service::control::message;
namespace sink = aap_protobuf::service::media::sink::message;
namespace shared = aap_protobuf::service::media::shared::message;

TEST(MediaWire, ConfiguredProfilesProduceActualPinnedVideoFields) {
    // Given: explicitly supported primary and fallback, registered video handler.
    aa::channels::ServiceRegistry registry;
    Sink handler;
    ASSERT_TRUE(registry.register_sink(proto::ChannelRole::video, handler));
    auto config = aa::channels::SupportConfiguration::create({video()});
    ASSERT_TRUE(config);
    aa::channels::Negotiator negotiator{std::move(config.value()), registry};
    // When: discovery is serialized by the real adapter.
    const auto encoded = proto::encode(negotiator.start());
    ASSERT_TRUE(encoded);
    ctl::ServiceDiscoveryResponse wire;
    ASSERT_TRUE(wire.ParseFromArray(encoded.value().data() + 2,
                                   static_cast<int>(encoded.value().size() - 2)));
    // Then: pinned fields, not a dummy codec-only entry, describe both profiles.
    ASSERT_EQ(wire.channels_size(), 1);
    EXPECT_EQ(wire.channels(0).id(), 99);
    const auto& media = wire.channels(0).media_sink_service();
    EXPECT_EQ(media.available_type(), shared::MEDIA_CODEC_VIDEO_H264_BP);
    ASSERT_EQ(media.video_configs_size(), 2);
    EXPECT_EQ(media.video_configs(0).codec_resolution(), sink::VIDEO_1280x720);
    EXPECT_EQ(media.video_configs(1).codec_resolution(), sink::VIDEO_800x480);
    for (const auto& profile : media.video_configs()) {
        EXPECT_TRUE(profile.has_codec_resolution());
        EXPECT_TRUE(profile.has_frame_rate());
        EXPECT_EQ(profile.frame_rate(), sink::VIDEO_FPS_30);
        EXPECT_EQ(profile.video_codec_type(), shared::MEDIA_CODEC_VIDEO_H264_BP);
    }
    const auto decoded = proto::decode_service_discovery_response(encoded.value());
    ASSERT_TRUE(decoded);
    EXPECT_EQ(decoded.value().services, std::vector<proto::ServiceDescriptor>{video()});
}

TEST(MediaWire, SelectsPrimaryOrFallbackFromBackendProfilesNotPeerOrder) {
    // Given: decoded wire-supported profiles, an opened service and reversed peer preference.
    const auto encoded = proto::encode(proto::ServiceDiscoveryResponse{{video()}});
    ASSERT_TRUE(encoded);
    const auto decoded = proto::decode_service_discovery_response(encoded.value());
    ASSERT_TRUE(decoded);
    auto config = aa::channels::SupportConfiguration::create(decoded.value().services);
    ASSERT_TRUE(config);
    aa::channels::ServiceRegistry registry;
    Sink handler;
    ASSERT_TRUE(registry.register_sink(proto::ChannelRole::video, handler));
    aa::channels::Negotiator negotiator{std::move(config.value()), registry};
    ASSERT_EQ(negotiator.start().services.size(), 1u);
    ASSERT_TRUE(negotiator.open({1, proto::ServiceKey{99}}));
    const std::array both{proto::VideoProfile{800, 480, 30}, proto::VideoProfile{1280, 720, 30}};
    // When: the peer offers profiles.
    const auto selected = negotiator.select_video(proto::ServiceKey{99}, both);
    // Then: backend primary wins, while a fallback-only offer selects 800x480.
    ASSERT_TRUE(selected);
    EXPECT_EQ(selected.value(), (proto::VideoProfile{1280, 720, 30}));
    const auto fallback = negotiator.select_video(proto::ServiceKey{99}, std::span(both).first(1));
    ASSERT_TRUE(fallback);
    EXPECT_EQ(fallback.value(), (proto::VideoProfile{800, 480, 30}));
}

TEST(MediaWire, AudioRolesAndPcmConfigurationsAreAccurate) {
    // Given: four distinct audio roles with configured PCM support.
    const std::array roles{proto::ChannelRole::media_audio, proto::ChannelRole::guidance_audio,
                           proto::ChannelRole::system_audio, proto::ChannelRole::speech_audio};
    const std::array wire_roles{sink::AUDIO_STREAM_MEDIA, sink::AUDIO_STREAM_GUIDANCE,
                                sink::AUDIO_STREAM_SYSTEM_AUDIO, sink::AUDIO_STREAM_TELEPHONY};
    proto::ServiceDiscoveryResponse response;
    for (std::size_t index = 0; index < roles.size(); ++index) {
        response.services.push_back(audio(roles[index], static_cast<std::int32_t>(index) + 20));
    }
    // When: the adapter emits the actual pinned descriptors.
    const auto encoded = proto::encode(response);
    ASSERT_TRUE(encoded);
    ctl::ServiceDiscoveryResponse wire;
    ASSERT_TRUE(wire.ParseFromArray(encoded.value().data() + 2,
                                   static_cast<int>(encoded.value().size() - 2)));
    // Then: stream types and PCM fields are preserved for every role.
    ASSERT_EQ(wire.channels_size(), 4);
    for (std::size_t index = 0; index < roles.size(); ++index) {
        const auto& media = wire.channels(static_cast<int>(index)).media_sink_service();
        EXPECT_EQ(media.audio_type(), wire_roles[index]);
        EXPECT_EQ(media.available_type(), shared::MEDIA_CODEC_AUDIO_PCM);
        ASSERT_EQ(media.audio_configs_size(), 1);
        EXPECT_EQ(media.audio_configs(0).sampling_rate(), 48000u);
        EXPECT_EQ(media.audio_configs(0).number_of_bits(), 16u);
        EXPECT_EQ(media.audio_configs(0).number_of_channels(), 2u);
    }
    const auto decoded = proto::decode_service_discovery_response(encoded.value());
    ASSERT_TRUE(decoded);
    EXPECT_EQ(decoded.value(), response);
}

TEST(MediaWire, AudioSelectionRequiresSupportedConfigurationAndOpen) {
    // Given: a configured PCM handler with a stereo-only profile.
    aa::channels::ServiceRegistry registry;
    Sink handler;
    ASSERT_TRUE(registry.register_sink(proto::ChannelRole::media_audio, handler));
    auto config = aa::channels::SupportConfiguration::create({audio(proto::ChannelRole::media_audio, 21)});
    ASSERT_TRUE(config);
    aa::channels::Negotiator negotiator{std::move(config.value()), registry};
    ASSERT_EQ(negotiator.start().services.size(), 1u);
    ASSERT_TRUE(negotiator.open({1, proto::ServiceKey{21}}));
    const std::array offered{proto::AudioProfile{16000, 16, 1}, proto::AudioProfile{48000, 16, 2}};
    // When: compatible and incompatible peer audio offers are selected.
    const auto selected = negotiator.select_audio(proto::ServiceKey{21}, offered);
    // Then: only the configured profile succeeds.
    ASSERT_TRUE(selected);
    EXPECT_EQ(selected.value(), (proto::AudioProfile{48000, 16, 2}));
    EXPECT_FALSE(negotiator.select_audio(proto::ServiceKey{21}, std::span(offered).first(1)));
    negotiator.reset();
    EXPECT_FALSE(negotiator.select_audio(proto::ServiceKey{21}, offered));
}

TEST(MediaWire, ButtonsAreActualAndNeverTouch) {
    // Given: an explicitly configured non-touch button list.
    const proto::ServiceDiscoveryResponse response{{{proto::ServiceKey{75}, proto::ChannelRole::input,
                                                    proto::ButtonConfiguration{{85, 87, 88}}}}};
    // When: the discovery payload is serialized.
    const auto encoded = proto::encode(response);
    ASSERT_TRUE(encoded);
    ctl::ServiceDiscoveryResponse wire;
    ASSERT_TRUE(wire.ParseFromArray(encoded.value().data() + 2,
                                   static_cast<int>(encoded.value().size() - 2)));
    // Then: only those buttons appear, without fabricated touch surfaces.
    const auto& input = wire.channels(0).input_source_service();
    ASSERT_EQ(input.keycodes_supported_size(), 3);
    EXPECT_EQ(input.keycodes_supported(0), 85);
    EXPECT_EQ(input.keycodes_supported(1), 87);
    EXPECT_EQ(input.keycodes_supported(2), 88);
    EXPECT_EQ(input.touchscreen_size(), 0);
    EXPECT_EQ(input.touchpad_size(), 0);
    const auto decoded = proto::decode_service_discovery_response(encoded.value());
    ASSERT_TRUE(decoded);
    EXPECT_EQ(decoded.value(), response);
}

TEST(MediaWire, OpenResponsesUsePinnedSuccessAndCommandNotSupported) {
    // Given: project success and unsupported outcomes.
    for (const auto status : {proto::ChannelOpenStatus::success, proto::ChannelOpenStatus::unsupported}) {
        // When: the real adapter serializes a channel-open response.
        const auto encoded = proto::encode(proto::ChannelOpenResponse{status});
        ASSERT_TRUE(encoded);
        ctl::ChannelOpenResponse wire;
        ASSERT_TRUE(wire.ParseFromArray(encoded.value().data() + 2,
                                       static_cast<int>(encoded.value().size() - 2)));
        // Then: discriminator 8 and the pinned enum status are accurate.
        EXPECT_EQ(encoded.value()[0], std::byte{0});
        EXPECT_EQ(encoded.value()[1], std::byte{8});
        EXPECT_EQ(wire.status(), status == proto::ChannelOpenStatus::success
            ? aap_protobuf::shared::STATUS_SUCCESS : aap_protobuf::shared::STATUS_COMMAND_NOT_SUPPORTED);
        const auto decoded = proto::decode_channel_open_response(encoded.value());
        ASSERT_TRUE(decoded);
        EXPECT_EQ(decoded.value().status, status);
    }
}

TEST(MediaWire, HostNegotiationDrivesWireOpenResponseAndHandlerDelivery) {
    // Given: an actual adapter advertisement decoded as a phone would see it.
    aa::channels::ServiceRegistry registry;
    Sink handler;
    ASSERT_TRUE(registry.register_sink(proto::ChannelRole::video, handler));
    auto config = aa::channels::SupportConfiguration::create({video()});
    ASSERT_TRUE(config);
    aa::channels::Negotiator negotiator{std::move(config.value()), registry};
    const auto discovery = proto::encode(negotiator.start());
    ASSERT_TRUE(discovery);
    const auto peer = proto::decode_service_discovery_response(discovery.value());
    ASSERT_TRUE(peer);
    ASSERT_EQ(peer.value().services.size(), 1u);
    const auto service = peer.value().services[0].id;
    const auto request = proto::encode(proto::ChannelOpenRequest{1, service});
    ASSERT_TRUE(request);
    const auto inbound = proto::decode_channel_open_request(request.value());
    ASSERT_TRUE(inbound);
    // When: the receiver accepts the actual decoded open and responds on the wire.
    const auto opened = negotiator.open(inbound.value());
    ASSERT_TRUE(opened);
    const auto response = proto::encode(opened.value());
    ASSERT_TRUE(response);
    const auto peer_open = proto::decode_channel_open_response(response.value());
    // Then: success permits the payload only on the discovered dynamic frame ID.
    ASSERT_TRUE(peer_open);
    EXPECT_EQ(peer_open.value().status, proto::ChannelOpenStatus::success);
    const std::array bytes{std::byte{0x00}, std::byte{0x00}, std::byte{0x01}, std::byte{0x65}};
    ASSERT_TRUE(negotiator.dispatch({static_cast<std::uint8_t>(service.value), 1, {}, bytes}));
    EXPECT_EQ(handler.bytes, std::vector<std::byte>(bytes.begin(), bytes.end()));
    EXPECT_EQ(handler.last.channel, proto::ChannelRole::video);
    const auto unsupported_request = proto::encode(proto::ChannelOpenRequest{1, proto::ServiceKey{7}});
    ASSERT_TRUE(unsupported_request);
    const auto unsupported_inbound = proto::decode_channel_open_request(unsupported_request.value());
    ASSERT_TRUE(unsupported_inbound);
    const auto unsupported = negotiator.open(unsupported_inbound.value());
    ASSERT_TRUE(unsupported);
    const auto unsupported_wire = proto::encode(unsupported.value());
    ASSERT_TRUE(unsupported_wire);
    const auto refused = proto::decode_channel_open_response(unsupported_wire.value());
    ASSERT_TRUE(refused);
    EXPECT_EQ(refused.value().status, proto::ChannelOpenStatus::unsupported);
}
} // namespace
