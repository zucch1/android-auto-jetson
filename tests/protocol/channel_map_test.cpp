// SPDX-License-Identifier: GPL-3.0-or-later
// Channel compatibility (task 15): the static frame-channel ordinals of the
// pinned SDK map to and from project channel roles, the three id spaces stay
// distinct, and out-of-range ordinals fail closed. The SDK's own ChannelId
// layout is the reference under test.
#include <aasdk/Messenger/ChannelId.hpp>

#include <aa/protocol/Messages.hpp>

#include <cstdint>
#include <type_traits>

#include <gtest/gtest.h>

namespace {

namespace proto = aa::protocol;
namespace sdk = aasdk::messenger;

struct MappingCase final {
    sdk::ChannelId channel;
    bool mapped;
    proto::ChannelRole role;
};

constexpr MappingCase kMappingTable[] = {
    {sdk::ChannelId::CONTROL, false, {}},
    {sdk::ChannelId::SENSOR, true, proto::ChannelRole::sensors},
    {sdk::ChannelId::MEDIA_SINK, false, {}},
    {sdk::ChannelId::MEDIA_SINK_VIDEO, true, proto::ChannelRole::video},
    {sdk::ChannelId::MEDIA_SINK_MEDIA_AUDIO, true, proto::ChannelRole::media_audio},
    {sdk::ChannelId::MEDIA_SINK_GUIDANCE_AUDIO, true, proto::ChannelRole::guidance_audio},
    {sdk::ChannelId::MEDIA_SINK_SYSTEM_AUDIO, true, proto::ChannelRole::system_audio},
    {sdk::ChannelId::MEDIA_SINK_TELEPHONY_AUDIO, true, proto::ChannelRole::speech_audio},
    {sdk::ChannelId::INPUT_SOURCE, true, proto::ChannelRole::input},
    {sdk::ChannelId::MEDIA_SOURCE_MICROPHONE, true, proto::ChannelRole::microphone},
    {sdk::ChannelId::BLUETOOTH, true, proto::ChannelRole::bluetooth_projection},
    {sdk::ChannelId::RADIO, false, {}},
    {sdk::ChannelId::NAVIGATION_STATUS, false, {}},
    {sdk::ChannelId::MEDIA_PLAYBACK_STATUS, false, {}},
    {sdk::ChannelId::PHONE_STATUS, false, {}},
    {sdk::ChannelId::MEDIA_BROWSER, false, {}},
    {sdk::ChannelId::VENDOR_EXTENSION, false, {}},
    {sdk::ChannelId::GENERIC_NOTIFICATION, false, {}},
    {sdk::ChannelId::WIFI_PROJECTION, true, proto::ChannelRole::wifi_projection},
    {sdk::ChannelId::NONE, false, {}},
};

} // namespace

TEST(ChannelMapping, EverySdkOrdinalFollowsThePinnedTable) {
    // Given: the pinned SDK's static ChannelId ordinals.
    // When: each ordinal crosses the adapter.
    // Then: mapped channels carry their documented role.
    for (const MappingCase& entry : kMappingTable) {
        if (!entry.mapped) {
            continue;
        }
        const auto ordinal = static_cast<std::uint8_t>(entry.channel);
        const auto result = proto::channel_role_from_wire(ordinal);
        ASSERT_TRUE(result.has_value()) << "ordinal " << unsigned{ordinal};
        EXPECT_EQ(result.value(), entry.role) << "ordinal " << unsigned{ordinal};
    }
}

TEST(ChannelMapping, UnmappedChannelsAreTypedErrors) {
    // Given: channel bytes with no dispatch role (control plane, parent sink,
    // radio/navigation/status/vendor services and the none sentinel).
    for (const MappingCase& entry : kMappingTable) {
        if (entry.mapped) {
            continue;
        }
        // When: the adapter maps them.
        const auto ordinal = static_cast<std::uint8_t>(entry.channel);
        const auto result = proto::channel_role_from_wire(ordinal);
        // Then: the typed error names the unsupported channel.
        ASSERT_FALSE(result.has_value()) << "ordinal " << unsigned{ordinal};
        EXPECT_EQ(result.error().code(), aa::ErrorCode::protocol_unsupported_channel);
    }
}

TEST(ChannelMapping, OutOfRangeOrdinalsFailClosed) {
    // Given: channel bytes that are not ChannelId values at all.
    for (const auto ordinal : {std::uint8_t{19}, std::uint8_t{42}, std::uint8_t{254}}) {
        // When: the adapter maps them.
        const auto result = proto::channel_role_from_wire(ordinal);
        // Then: they are rejected, never silently mapped.
        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code(), aa::ErrorCode::protocol_unsupported_channel);
    }
}

TEST(ChannelMapping, RoleToOrdinalRoundTripsIdentity) {
    // Given: every dispatch role with a static wire channel.
    for (const MappingCase& entry : kMappingTable) {
        if (!entry.mapped) {
            continue;
        }
        // When: role -> ordinal -> role.
        const auto ordinal = proto::wire_from_channel_role(entry.role);
        ASSERT_TRUE(ordinal.has_value());
        const auto role = proto::channel_role_from_wire(ordinal.value());
        // Then: the mapping is the identity.
        ASSERT_TRUE(role.has_value());
        EXPECT_EQ(role.value(), entry.role);
    }
}

TEST(ChannelMapping, DiagnosticsRoleHasNoStaticWireChannel) {
    // Given: the local-only diagnostics role.
    // When: a wire ordinal is requested.
    const auto result = proto::wire_from_channel_role(proto::ChannelRole::diagnostics);
    // Then: the adapter refuses instead of inventing a channel byte.
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code(), aa::ErrorCode::protocol_unsupported_channel);
}

TEST(ChannelMapping, ThreeIdSpacesDoNotCollide) {
    // Given: the numeric value 1 is the SENSOR ordinal in the static space.
    EXPECT_EQ(static_cast<std::uint8_t>(sdk::ChannelId::SENSOR), 1);
    const auto role = proto::channel_role_from_wire(1);
    ASSERT_TRUE(role.has_value());
    EXPECT_EQ(role.value(), proto::ChannelRole::sensors);
    // When: the same number occupies the dynamic Service.id space.
    const proto::ServiceKey dynamic{1};
    EXPECT_TRUE(static_cast<bool>(dynamic));
    // And: the same number is the version_request control message id.
    EXPECT_EQ(static_cast<std::uint16_t>(proto::ControlMessageId::version_request), 1);
    // Then: the spaces are distinct types with no implicit conversion.
    static_assert(!std::is_convertible_v<proto::ServiceKey, std::uint8_t>);
    static_assert(!std::is_convertible_v<proto::ControlMessageId, std::uint8_t>);
    static_assert(!std::is_convertible_v<proto::ControlMessageId, proto::ServiceKey>);
}

TEST(ChannelMapping, ZeroIsNotAServiceIdentity) {
    // Given: the unset service id.
    const proto::ServiceKey unset{0};
    // When/Then: it is rejected by the id space contract.
    EXPECT_FALSE(static_cast<bool>(unset));
    EXPECT_TRUE(static_cast<bool>(proto::ServiceKey{1}));
}
