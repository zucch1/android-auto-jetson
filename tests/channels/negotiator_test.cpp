// SPDX-License-Identifier: GPL-3.0-or-later
#include "fixtures.hpp"
#include <aa/core/SessionThread.hpp>
#include <array>
#include <future>
#include <gtest/gtest.h>

namespace {
using namespace channeltest;
namespace channels = aa::channels;

TEST(ChannelServices, DefaultsOmitEveryWireRoleEvenWhenHandlersExist) {
    // Given: handlers but no qualified support configuration.
    channels::ServiceRegistry registry;
    Sink sink;
    for (const auto role : {proto::ChannelRole::video, proto::ChannelRole::media_audio,
         proto::ChannelRole::guidance_audio, proto::ChannelRole::system_audio,
         proto::ChannelRole::speech_audio, proto::ChannelRole::input,
         proto::ChannelRole::microphone, proto::ChannelRole::sensors,
         proto::ChannelRole::bluetooth_projection, proto::ChannelRole::wifi_projection,
         proto::ChannelRole::diagnostics}) {
        ASSERT_TRUE(registry.register_sink(role, sink));
    }
    channels::Negotiator negotiator{{}, registry};
    // When: a fresh session advertises.
    const auto advertisement = negotiator.start();
    // Then: registration alone fabricates no capability.
    EXPECT_TRUE(advertisement.services.empty());
}

TEST(ChannelServices, RoutesDynamic99OnSessionThreadOnlyAfterOpen) {
    // Given: a real serialized owner thread, configured video and a registered sink.
    aa::core::SessionThread owner;
    std::promise<bool> finished;
    auto result = finished.get_future();
    ASSERT_TRUE(owner.post([&] {
        channels::ServiceRegistry registry;
        Sink sink;
        auto config = channels::SupportConfiguration::create({video()});
        bool passed = owner.is_owner_thread() && config.has_value()
            && registry.register_sink(proto::ChannelRole::video, sink).has_value();
        if (!passed) { finished.set_value(false); return; }
        channels::Negotiator negotiator{std::move(config.value()), registry};
        passed = negotiator.start().services.size() == 1;
        const std::array payload{std::byte{0x18}, std::byte{0x99}};
        const proto::WireMessageView frame{99, 12, aa::core::Nanoseconds{42}, payload};
        passed = passed && !negotiator.dispatch(frame).has_value();
        // When: the phone opens negotiated ID 99 and sends its frame.
        const auto opened = negotiator.open({1, proto::ServiceKey{99}});
        passed = passed && opened.has_value()
            && opened.value().status == proto::ChannelOpenStatus::success
            && negotiator.dispatch(frame).has_value();
        // Then: semantic video, metadata and bytes arrive at the handler.
        passed = passed && sink.calls == 1 && sink.last.channel == proto::ChannelRole::video
            && sink.last.sequence == 12 && sink.last.timestamp == aa::core::Nanoseconds{42}
            && sink.bytes == std::vector<std::byte>(payload.begin(), payload.end());
        finished.set_value(passed);
    }));
    ASSERT_EQ(result.wait_for(std::chrono::seconds{5}), std::future_status::ready);
    EXPECT_TRUE(result.get());
    owner.request_stop();
    owner.join();
}

TEST(ChannelServices, StaticOrdinalControlUnknownAndUnopenedNeverDispatch) {
    // Given: video is advertised as 99, input as 100 but neither is open.
    channels::ServiceRegistry registry;
    Sink sink;
    ASSERT_TRUE(registry.register_sink(proto::ChannelRole::video, sink));
    ASSERT_TRUE(registry.register_sink(proto::ChannelRole::input, sink));
    auto config = channels::SupportConfiguration::create({video(),
        {proto::ServiceKey{100}, proto::ChannelRole::input, proto::ButtonConfiguration{{85}}}});
    ASSERT_TRUE(config);
    channels::Negotiator negotiator{std::move(config.value()), registry};
    ASSERT_EQ(negotiator.start().services.size(), 2u);
    ASSERT_TRUE(negotiator.open({1, proto::ServiceKey{99}}));
    // When: IDs other than the opened advertised service are dispatched.
    for (const std::uint8_t id : {std::uint8_t{0}, std::uint8_t{3}, std::uint8_t{7},
                                std::uint8_t{100}, std::uint8_t{255}}) {
        EXPECT_FALSE(negotiator.dispatch({id, 0, {}, {}}));
    }
    // Then: neither a static video ordinal nor a control discriminator reached a sink.
    EXPECT_EQ(sink.calls, 0);
}

TEST(ChannelServices, UnknownUnconfiguredAndRemovedServicesOpenUnsupported) {
    // Given: configured video with no registered handler and unsupported hardware handlers.
    channels::ServiceRegistry registry;
    Sink sink;
    ASSERT_TRUE(registry.register_sink(proto::ChannelRole::microphone, sink));
    ASSERT_TRUE(registry.register_sink(proto::ChannelRole::sensors, sink));
    auto config = channels::SupportConfiguration::create({video()});
    ASSERT_TRUE(config);
    channels::Negotiator negotiator{std::move(config.value()), registry};
    ASSERT_TRUE(negotiator.start().services.empty());
    // When: the peer requests missing video, microphone/sensor ordinals or an unknown ID.
    for (const auto id : {99, 7, 1, 255}) {
        const auto opened = negotiator.open({1, proto::ServiceKey{id}});
        // Then: every request deterministically returns unsupported.
        ASSERT_TRUE(opened);
        EXPECT_EQ(opened.value().status, proto::ChannelOpenStatus::unsupported);
    }
}

TEST(ChannelServices, ResetAndRestartRequireFreshOpen) {
    // Given: an opened video channel.
    channels::ServiceRegistry registry;
    Sink sink;
    ASSERT_TRUE(registry.register_sink(proto::ChannelRole::video, sink));
    auto config = channels::SupportConfiguration::create({video()});
    ASSERT_TRUE(config);
    channels::Negotiator negotiator{std::move(config.value()), registry};
    ASSERT_EQ(negotiator.start().services.size(), 1u);
    ASSERT_TRUE(negotiator.open({1, proto::ServiceKey{99}}));
    // When: teardown resets the session and a new session advertises.
    negotiator.reset();
    EXPECT_FALSE(negotiator.dispatch({99, 0, {}, {}}));
    ASSERT_EQ(negotiator.start().services.size(), 1u);
    // Then: advertisement alone does not restore the old open state.
    EXPECT_FALSE(negotiator.dispatch({99, 0, {}, {}}));
    EXPECT_EQ(sink.calls, 0);
}

TEST(ChannelServices, ReplacementSinkCannotInheritAdvertisementOrOpen) {
    // Given: opened video and a replacement handler.
    channels::ServiceRegistry registry;
    Sink original;
    Sink replacement;
    ASSERT_TRUE(registry.register_sink(proto::ChannelRole::video, original));
    auto config = channels::SupportConfiguration::create({video()});
    ASSERT_TRUE(config);
    channels::Negotiator negotiator{std::move(config.value()), registry};
    ASSERT_EQ(negotiator.start().services.size(), 1u);
    ASSERT_TRUE(negotiator.open({1, proto::ServiceKey{99}}));
    // When: registration is removed then replaced during the session.
    ASSERT_TRUE(registry.unregister_sink(proto::ChannelRole::video));
    ASSERT_TRUE(registry.register_sink(proto::ChannelRole::video, replacement));
    // Then: neither dispatch nor another open can reuse the stale advertisement.
    EXPECT_FALSE(negotiator.dispatch({99, 0, {}, {}}));
    const auto opened = negotiator.open({1, proto::ServiceKey{99}});
    ASSERT_TRUE(opened);
    EXPECT_EQ(opened.value().status, proto::ChannelOpenStatus::unsupported);
    EXPECT_EQ(replacement.calls, 0);
}

TEST(ChannelServices, AllocatorAvoidsExplicitIdsAndRejectsCollisionsBoundsAndRoles) {
    // Given: explicit ID 1 and an unset video ID.
    const auto button = proto::ServiceDescriptor{proto::ServiceKey{1}, proto::ChannelRole::input,
                                                proto::ButtonConfiguration{{85}}};
    // When: support is validated and allocated.
    const auto configured = channels::SupportConfiguration::create({video(0), button});
    // Then: allocation skips 1; invalid configurations never publish.
    ASSERT_TRUE(configured);
    EXPECT_EQ(configured.value().services()[0].id.value, 2);
    EXPECT_FALSE(channels::SupportConfiguration::create({video(1), button}));
    EXPECT_FALSE(channels::SupportConfiguration::create({video(99), video(100)}));
    EXPECT_FALSE(channels::SupportConfiguration::create({video(256)}));
    EXPECT_FALSE(channels::SupportConfiguration::create({video(-1)}));
    EXPECT_TRUE(channels::SupportConfiguration::create({video(255)}));
}

TEST(ChannelServices, DiagnosticsAreLocalAndDuplicateRegistrationIsRefused) {
    // Given: a local diagnostics sink.
    channels::ServiceRegistry registry;
    Sink sink;
    ASSERT_TRUE(registry.register_sink(proto::ChannelRole::diagnostics, sink));
    // When: local dispatch is used.
    const auto delivered = registry.dispatch_local({{proto::ChannelRole::diagnostics, 0, {}}, {}});
    // Then: no wire role bypass exists and duplicate registration fails.
    EXPECT_TRUE(delivered);
    EXPECT_EQ(sink.calls, 1);
    EXPECT_FALSE(registry.register_sink(proto::ChannelRole::diagnostics, sink));
    EXPECT_FALSE(registry.dispatch_local({{proto::ChannelRole::video, 0, {}}, {}}));
    EXPECT_FALSE(channels::SupportConfiguration::create(
        {{proto::ServiceKey{99}, proto::ChannelRole::diagnostics}}));
}

TEST(ChannelServices, EveryConfiguredRoleRoutesByDynamicIdIncludingOrdinalConflict) {
    // Given: input ID 3 conflicts with the SDK's static video ordinal.
    const std::array roles{proto::ChannelRole::video, proto::ChannelRole::media_audio,
        proto::ChannelRole::guidance_audio, proto::ChannelRole::system_audio,
        proto::ChannelRole::speech_audio, proto::ChannelRole::input};
    const std::array<std::uint8_t, 6> ids{99, 20, 21, 22, 23, 3};
    std::array<Sink, 6> handlers;
    channels::ServiceRegistry registry;
    std::vector<proto::ServiceDescriptor> services{video()};
    for (std::size_t index = 0; index < roles.size(); ++index) {
        ASSERT_TRUE(registry.register_sink(roles[index], handlers[index]));
        if (index > 0 && index < 5) { services.push_back(audio(roles[index], ids[index])); }
    }
    services.push_back({proto::ServiceKey{3}, proto::ChannelRole::input,
                        proto::ButtonConfiguration{{85}}});
    auto config = channels::SupportConfiguration::create(std::move(services));
    ASSERT_TRUE(config);
    channels::Negotiator negotiator{std::move(config.value()), registry};
    ASSERT_EQ(negotiator.start().services.size(), 6u);
    // When: every advertised service opens and sends a frame using its negotiated ID.
    for (const auto id : ids) {
        ASSERT_TRUE(negotiator.open({1, proto::ServiceKey{id}}));
        ASSERT_TRUE(negotiator.dispatch({id, 0, {}, {}}));
    }
    // Then: all six handlers receive exactly their own semantic role, including input on 3.
    for (std::size_t index = 0; index < roles.size(); ++index) {
        EXPECT_EQ(handlers[index].calls, 1);
        EXPECT_EQ(handlers[index].last.channel, roles[index]);
    }
}

TEST(ChannelServices, MalformedOpenIdsNeverChangeOpenState) {
    // Given: an advertised unopened service.
    channels::ServiceRegistry registry;
    Sink sink;
    ASSERT_TRUE(registry.register_sink(proto::ChannelRole::video, sink));
    auto config = channels::SupportConfiguration::create({video()});
    ASSERT_TRUE(config);
    channels::Negotiator negotiator{std::move(config.value()), registry};
    ASSERT_EQ(negotiator.start().services.size(), 1u);
    // When: an untrusted open ID cannot fit a non-control frame byte.
    for (const auto id : {-1, 0, 256}) {
        const auto result = negotiator.open({1, proto::ServiceKey{id}});
        // Then: a malformed error is returned and the advertised service remains closed.
        ASSERT_FALSE(result);
        EXPECT_EQ(result.error().code(), aa::ErrorCode::protocol_malformed_message);
    }
    EXPECT_FALSE(negotiator.dispatch({99, 0, {}, {}}));
}
} // namespace
