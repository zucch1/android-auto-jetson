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
        EXPECT_TRUE(profile.has_density());
        EXPECT_EQ(profile.density(), 160u);
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

namespace {
TEST(MediaWire, DefaultScreenExactlyMatchesLegacyAnnouncement) {
    // Golden captured independently of the screen encoder: the video Service
    // entry main announced (1280x720 then 800x480, H.264 BP, 30 fps, density
    // 160, no other field), from `protoc --encode=aap_protobuf.service.Service`.
    // The receiver's own descriptor path is checked in wired_e2e.
    const auto encoded = aa::protocol::encode(aa::protocol::ServiceDiscoveryResponse{{channeltest::video()}});
    ASSERT_TRUE(encoded);
    ctl::ServiceDiscoveryResponse actual;
    ASSERT_TRUE(actual.ParseFromArray(encoded.value().data() + 2,
                                     static_cast<int>(encoded.value().size() - 2)));
    static constexpr unsigned char golden[] = {
        0x08, 0x63, 0x1a, 0x18, 0x08, 0x03, 0x22, 0x09, 0x08, 0x02, 0x10, 0x02, 0x28, 0xa0, 0x01, 0x50,
        0x03, 0x22, 0x09, 0x08, 0x01, 0x10, 0x02, 0x28, 0xa0, 0x01, 0x50, 0x03};
    EXPECT_EQ(actual.channels(0).SerializeAsString(),
              std::string(reinterpret_cast<const char*>(golden), sizeof golden));
}

TEST(MediaWire, ScreenOptionsReachDiscoveryScaledPerProfileAndRoundtrip) {
    auto descriptor = channeltest::video();
    auto& screen = std::get<proto::VideoConfiguration>(descriptor.configuration).screen;
    screen.density = 240;
    screen.real_density = 210;
    screen.width_margin = 160;  // 1280x720 pixels
    screen.height_margin = 120;
    screen.pixel_aspect_ratio_e4 = 10000;
    screen.decoder_additional_depth = 2;
    screen.theme = 2;
    screen.margins = aa::config::ScreenInsets{60, 90, 128, 64};          // top, bottom, left, right
    screen.content_insets = aa::config::ScreenInsets{3, 6, 3, 6};        // half-up rounding
    screen.stable_content_insets = aa::config::ScreenInsets{12, 18, 16, 32};
    const proto::ServiceDiscoveryResponse response{{descriptor}};
    const auto encoded = proto::encode(response);
    ASSERT_TRUE(encoded);
    ctl::ServiceDiscoveryResponse wire;
    ASSERT_TRUE(wire.ParseFromArray(encoded.value().data() + 2,
                                   static_cast<int>(encoded.value().size() - 2)));
    const auto& configs = wire.channels(0).media_sink_service().video_configs();
    ASSERT_EQ(configs.size(), 2);
    for (const auto& v : configs) {  // panel properties are not scaled
        EXPECT_EQ(v.density(), 240u);
        EXPECT_EQ(v.real_density(), 210u);
        EXPECT_EQ(v.pixel_aspect_ratio_e4(), 10000u);
        EXPECT_EQ(v.decoder_additional_depth(), 2u);
        EXPECT_EQ(v.ui_config().ui_theme(), shared::UI_THEME_DARK);
    }
    // Primary 1280x720: exactly as configured.
    const auto& primary = configs[0];
    ASSERT_EQ(primary.codec_resolution(), sink::VIDEO_1280x720);
    EXPECT_EQ(primary.width_margin(), 160u);
    EXPECT_EQ(primary.height_margin(), 120u);
    EXPECT_EQ(primary.ui_config().margins().top(), 60u);
    EXPECT_EQ(primary.ui_config().margins().bottom(), 90u);
    EXPECT_EQ(primary.ui_config().margins().left(), 128u);
    EXPECT_EQ(primary.ui_config().margins().right(), 64u);
    // Fallback 800x480: x scaled by 800/1280, y by 480/720, round half up.
    const auto& fallback = configs[1];
    ASSERT_EQ(fallback.codec_resolution(), sink::VIDEO_800x480);
    EXPECT_EQ(fallback.width_margin(), 100u);
    EXPECT_EQ(fallback.height_margin(), 80u);
    EXPECT_EQ(fallback.ui_config().margins().top(), 40u);
    EXPECT_EQ(fallback.ui_config().margins().bottom(), 60u);
    EXPECT_EQ(fallback.ui_config().margins().left(), 80u);
    EXPECT_EQ(fallback.ui_config().margins().right(), 40u);
    EXPECT_EQ(fallback.ui_config().content_insets().top(), 2u);   // 3 * 2/3 = 2
    EXPECT_EQ(fallback.ui_config().content_insets().bottom(), 4u);
    EXPECT_EQ(fallback.ui_config().content_insets().left(), 2u);  // 1.875 -> 2
    EXPECT_EQ(fallback.ui_config().content_insets().right(), 4u); // 3.75 -> 4
    EXPECT_EQ(fallback.ui_config().stable_content_insets().left(), 10u);
    EXPECT_EQ(fallback.ui_config().stable_content_insets().right(), 20u);
    const auto decoded = proto::decode_service_discovery_response(encoded.value());
    ASSERT_TRUE(decoded);
    EXPECT_EQ(decoded.value(), response);
}

TEST(MediaWire, ScalingIsPerAxisHalfUpAndLeavesPanelPropertiesAlone) {
    aa::config::Screen screen;
    screen.density = 320;
    screen.real_density = 300;
    screen.width_margin = 1;   // 0.625 -> 1
    screen.height_margin = 1;  // 0.667 -> 1
    screen.margins = aa::config::ScreenInsets{3, 1, 5, 2};
    const aa::config::ScreenExtent primary{1280, 720}, fallback{800, 480};
    const auto scaled = aa::config::scale_screen(screen, primary, fallback);
    EXPECT_EQ(scaled.density, 320u);
    EXPECT_EQ(scaled.real_density, screen.real_density);
    EXPECT_EQ(scaled.width_margin, 1u);
    EXPECT_EQ(scaled.height_margin, 1u);
    EXPECT_EQ(*scaled.margins, (aa::config::ScreenInsets{2, 1, 3, 1}));  // 3*2/3=2, 5*.625=3.125, 2*.625=1.25
    EXPECT_FALSE(scaled.content_insets);
    EXPECT_EQ(aa::config::scale_screen(screen, primary, primary), screen);
    EXPECT_EQ(aa::config::scale_screen(aa::config::scale_screen(screen, primary, primary), primary, fallback), scaled);
}

TEST(MediaWire, ScreenIsValidatedAgainstEveryOfferedProfile) {
    const std::array<aa::config::ScreenExtent, 2> profiles{{{1280, 720}, {800, 480}}};
    aa::config::Screen screen;
    EXPECT_TRUE(aa::config::invalid_screen_field(screen, profiles).empty());
    EXPECT_EQ(aa::config::invalid_screen_field(screen, std::span<const aa::config::ScreenExtent>{}), "profiles");
    // 1275 + 4 < 1280 fits the primary profile, but half-up scaling gives
    // 797 + 3 = 800 in the fallback: rejected by the fallback only.
    screen.margins = aa::config::ScreenInsets{0, 0, 1275, 4};
    EXPECT_TRUE(aa::config::invalid_screen_field(screen, 1280, 720).empty());
    EXPECT_EQ(aa::config::invalid_screen_field(screen, profiles), "margins");
    // 1279 in the primary extent is the last width margin that still fits both.
    screen = {};
    screen.width_margin = 1279;
    EXPECT_TRUE(aa::config::invalid_screen_field(screen, profiles).empty());
    screen.width_margin = 1280;
    EXPECT_EQ(aa::config::invalid_screen_field(screen, profiles), "width-margin");
    screen = {};
    screen.height_margin = 719;
    EXPECT_TRUE(aa::config::invalid_screen_field(screen, profiles).empty());
    screen.height_margin = 720;
    EXPECT_EQ(aa::config::invalid_screen_field(screen, profiles), "height-margin");
    // The encoder refuses it too: nothing unsafe reaches the phone.
    auto descriptor = channeltest::video();
    std::get<proto::VideoConfiguration>(descriptor.configuration).screen =
        aa::config::Screen{.margins = aa::config::ScreenInsets{0, 0, 1275, 4}};
    EXPECT_FALSE(proto::encode(proto::ServiceDiscoveryResponse{{descriptor}}));
}

TEST(MediaWire, OnlyUnimplementedGeometryNeedsTheExperimentalFlag) {
    EXPECT_TRUE(aa::config::experimental_screen_field({}).empty());
    aa::config::Screen screen;
    screen.density = 240;
    screen.real_density = 210;
    screen.theme = 1;
    screen.decoder_additional_depth = 1;
    screen.pixel_aspect_ratio_e4 = 10000;
    screen.width_margin = 0;
    screen.margins = aa::config::ScreenInsets{};
    screen.content_insets = aa::config::ScreenInsets{4, 4, 4, 4};  // phone-side layout only
    screen.stable_content_insets = aa::config::ScreenInsets{4, 4, 4, 4};
    EXPECT_TRUE(aa::config::experimental_screen_field(screen).empty());
    for (const auto& [field, apply] : std::vector<std::pair<std::string_view, void (*)(aa::config::Screen&)>>{
             {"width-margin", [](aa::config::Screen& s) { s.width_margin = 1; }},
             {"height-margin", [](aa::config::Screen& s) { s.height_margin = 1; }},
             {"margins", [](aa::config::Screen& s) { s.margins = aa::config::ScreenInsets{0, 0, 0, 1}; }},
             {"pixel-aspect-ratio-e4", [](aa::config::Screen& s) { s.pixel_aspect_ratio_e4 = 9999; }}}) {
        auto changed = screen;
        apply(changed);
        EXPECT_EQ(aa::config::experimental_screen_field(changed), field);
    }
}

TEST(MediaWire, LegacyViewingDistanceIsIgnoredOnDecode) {
    // proto field 7 is obsolete (present through AA 16.4): a legacy sender may
    // still set it, and decoding must keep accepting and ignoring it.
    const auto encoded = proto::encode(proto::ServiceDiscoveryResponse{{channeltest::video()}});
    ASSERT_TRUE(encoded);
    ctl::ServiceDiscoveryResponse wire;
    ASSERT_TRUE(wire.ParseFromArray(encoded.value().data() + 2,
                                   static_cast<int>(encoded.value().size() - 2)));
    for (auto& v : *wire.mutable_channels(0)->mutable_media_sink_service()->mutable_video_configs()) {
        v.set_viewing_distance(100);
    }
    std::vector<std::byte> payload(encoded.value().begin(), encoded.value().begin() + 2);
    const auto body = wire.SerializeAsString();
    for (const char c : body) { payload.push_back(static_cast<std::byte>(c)); }
    const auto decoded = proto::decode_service_discovery_response(payload);
    ASSERT_TRUE(decoded);
    EXPECT_EQ(decoded.value().services.front(), channeltest::video());
}

TEST(MediaWire, InsetEdgesAreCanonicalizedToExplicitZeros) {
    // Only one edge configured: the other three go out as explicit zeros (the
    // proto2 default value). Per-edge presence is not preserved or reported.
    auto descriptor = channeltest::video();
    std::get<proto::VideoConfiguration>(descriptor.configuration).screen.content_insets =
        aa::config::ScreenInsets{0, 0, 12, 0};
    const auto encoded = proto::encode(proto::ServiceDiscoveryResponse{{descriptor}});
    ASSERT_TRUE(encoded);
    ctl::ServiceDiscoveryResponse wire;
    ASSERT_TRUE(wire.ParseFromArray(encoded.value().data() + 2,
                                   static_cast<int>(encoded.value().size() - 2)));
    const auto& insets = wire.channels(0).media_sink_service().video_configs(0).ui_config().content_insets();
    EXPECT_TRUE(insets.has_top() && insets.has_bottom() && insets.has_right());
    EXPECT_EQ(insets.left(), 12u);
    EXPECT_EQ(insets.top() + insets.bottom() + insets.right(), 0u);
    auto absent = wire;
    for (auto& v : *absent.mutable_channels(0)->mutable_media_sink_service()->mutable_video_configs()) {
        auto* i = v.mutable_ui_config()->mutable_content_insets();
        i->clear_top(); i->clear_bottom(); i->clear_right();  // a sender that left the edges absent
    }
    std::vector<std::byte> payload(encoded.value().begin(), encoded.value().begin() + 2);
    for (const char c : absent.SerializeAsString()) { payload.push_back(static_cast<std::byte>(c)); }
    const auto decoded = proto::decode_service_discovery_response(payload);
    ASSERT_TRUE(decoded);
    EXPECT_EQ(decoded.value().services.front(), descriptor);
}

TEST(MediaWire, ScreenParserRejectsInvalidNumbersAndUnknownOptions) {
    for (const auto value : {"", "-1", "+1", "1x", " 1", "1 ", "4294967296"}) {
        aa::config::Screen screen;
        EXPECT_FALSE(aa::config::parse_screen_option(screen, "density", value)) << value;
        EXPECT_EQ(screen, aa::config::Screen{});
    }
    aa::config::Screen screen;
    EXPECT_FALSE(aa::config::parse_screen_option(screen, "unknown", "1"));
    EXPECT_FALSE(aa::config::parse_screen_option(screen, "margins-unknown", "1"));
    EXPECT_EQ(screen, aa::config::Screen{});
}

TEST(MediaWire, ScreenBoundsNameEveryInvalidField) {
    struct Case { const char* field; const char* good; const char* bad; };
    for (const auto& c : {Case{"density", "640", "161"},
         Case{"real-density", "1000", "1001"}, Case{"real-density", "40", "39"},
         Case{"pixel-aspect-ratio-e4", "5000", "4999"},
         Case{"pixel-aspect-ratio-e4", "20000", "20001"},
         Case{"decoder-additional-depth", "8", "9"}, Case{"theme", "2", "3"},
         Case{"width-margin", "799", "800"}, Case{"height-margin", "479", "480"}}) {
        aa::config::Screen screen;
        ASSERT_TRUE(aa::config::parse_screen_option(screen, c.field, c.good));
        EXPECT_TRUE(aa::config::invalid_screen_field(screen, 800, 480).empty()) << c.field;
        ASSERT_TRUE(aa::config::parse_screen_option(screen, c.field, c.bad));
        EXPECT_EQ(aa::config::invalid_screen_field(screen, 800, 480), c.field);
    }
    for (const auto prefix : {"margins", "content-insets", "stable-content-insets"}) {
        for (const auto side : {"top", "bottom", "left", "right"}) {
            aa::config::Screen screen;
            const auto field = std::string{prefix} + "-" + side;
            ASSERT_TRUE(aa::config::parse_screen_option(screen, field, "10"));
            EXPECT_TRUE(aa::config::invalid_screen_field(screen, 800, 480).empty());
            ASSERT_TRUE(aa::config::parse_screen_option(screen, field, "800"));
            EXPECT_EQ(aa::config::invalid_screen_field(screen, 800, 480), prefix);
        }
        aa::config::Screen screen;
        ASSERT_TRUE(aa::config::parse_screen_option(screen, std::string{prefix} + "-left", "400"));
        ASSERT_TRUE(aa::config::parse_screen_option(screen, std::string{prefix} + "-right", "400"));
        EXPECT_EQ(aa::config::invalid_screen_field(screen, 800, 480), prefix);
    }
}

TEST(MediaWire, UnsafeScreenCannotBeAnnounced) {
    auto descriptor = channeltest::video();
    auto& screen = std::get<proto::VideoConfiguration>(descriptor.configuration).screen;
    screen.density = 999;
    EXPECT_FALSE(proto::encode(proto::ServiceDiscoveryResponse{{descriptor}}));
    screen = {};
    screen.height_margin = 720; // not below the primary profile's height
    EXPECT_FALSE(proto::encode(proto::ServiceDiscoveryResponse{{descriptor}}));
    screen.height_margin = 719; // still below it, and below the scaled 800x480 height
    EXPECT_TRUE(proto::encode(proto::ServiceDiscoveryResponse{{descriptor}}));
}
} // namespace
