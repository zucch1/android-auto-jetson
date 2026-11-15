// SPDX-License-Identifier: GPL-3.0-or-later
// The wireless receiver offers the video profiles it was configured with: the
// service-99 discovery entry must carry the configured list (--video-profiles),
// not a fixed 720p pair, and must default to the shared default when omitted.
#include "../../tools/wireless-receiver/ServiceDescription.hpp"

#include <gtest/gtest.h>
#include <aa/protocol/Messages.hpp>
#include <algorithm>
#include <variant>
#include <vector>

namespace {

const aa::protocol::ServiceDescriptor* video_service(
    const std::vector<aa::protocol::ServiceDescriptor>& services) {
    const auto found = std::find_if(services.begin(), services.end(), [](const auto& service) {
        return service.role == aa::protocol::ChannelRole::video;
    });
    return found == services.end() ? nullptr : &*found;
}

} // namespace

TEST(WirelessVideoProfiles, NonDefaultProfileReachesTheOfferedVideoService) {
    // Given: a receiver configured for one non-default profile.
    const auto capabilities = aa::input::CapabilityDescriptor::bench_defaults();
    const std::vector<aa::protocol::VideoProfile> configured{{1920, 1080, 60,
                                                              aa::protocol::VideoCodec::h264}};
    // When: the wireless service description is built.
    const auto services = aa::wireless_receiver::service_descriptors(capabilities, configured);
    // Then: the video service offers exactly that profile.
    const auto* video = video_service(services);
    ASSERT_NE(video, nullptr);
    const auto* configuration =
        std::get_if<aa::protocol::VideoConfiguration>(&video->configuration);
    ASSERT_NE(configuration, nullptr);
    EXPECT_EQ(configuration->profiles, configured);
    EXPECT_EQ(configuration->profiles.front(),
              (aa::protocol::VideoProfile{1920, 1080, 60, aa::protocol::VideoCodec::h264}));
}

TEST(WirelessVideoProfiles, OmittedProfilesDefaultToTheSharedDefault) {
    // Given: no configured profile list.
    const auto capabilities = aa::input::CapabilityDescriptor::bench_defaults();
    // When: the service description is built.
    const auto services = aa::wireless_receiver::service_descriptors(capabilities);
    // Then: the offer is the shared default the wired receiver also uses.
    const auto* video = video_service(services);
    ASSERT_NE(video, nullptr);
    const auto* configuration =
        std::get_if<aa::protocol::VideoConfiguration>(&video->configuration);
    ASSERT_NE(configuration, nullptr);
    EXPECT_EQ(configuration->profiles, aa::config::default_video_profiles());
}

TEST(WirelessVideoProfiles, NonVideoServicesKeepTheirFixedShapes) {
    // Given: a non-default video profile list.
    const auto capabilities = aa::input::CapabilityDescriptor::bench_defaults();
    const std::vector<aa::protocol::VideoProfile> configured{{1920, 1080, 60,
                                                              aa::protocol::VideoCodec::h264}};
    // When: the service description is built.
    const auto services = aa::wireless_receiver::service_descriptors(capabilities, configured);
    // Then: the audio and microphone services are unchanged.
    const auto audio = std::find_if(services.begin(), services.end(), [](const auto& service) {
        return service.role == aa::protocol::ChannelRole::media_audio;
    });
    ASSERT_NE(audio, services.end());
    const auto* audio_configuration =
        std::get_if<aa::protocol::AudioConfiguration>(&audio->configuration);
    ASSERT_NE(audio_configuration, nullptr);
    EXPECT_EQ(audio_configuration->profiles,
              (std::vector<aa::protocol::AudioProfile>{{48000, 16, 2}}));
    const auto microphone = std::find_if(services.begin(), services.end(), [](const auto& service) {
        return service.role == aa::protocol::ChannelRole::microphone;
    });
    ASSERT_NE(microphone, services.end());
    const auto* microphone_configuration =
        std::get_if<aa::protocol::MicrophoneConfiguration>(&microphone->configuration);
    ASSERT_NE(microphone_configuration, nullptr);
    EXPECT_EQ(microphone_configuration->profile, (aa::protocol::AudioProfile{16000, 16, 1}));
}
