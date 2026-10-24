// SPDX-License-Identifier: GPL-3.0-or-later
#include <aa/channels/Negotiator.hpp>
#include <aa/protocol/WifiProjection.hpp>
#include <gtest/gtest.h>

namespace {
namespace proto = aa::protocol;
namespace channels = aa::channels;

class WifiSink final : public proto::MessageSink {
public:
    aa::core::Result<void> on_message(const proto::MessageView& message) override {
        const auto request = proto::decode_wifi_credentials_request(message);
        if (!request) { return request.error(); }
        ++requests;
        return {};
    }
    int requests{};
};

TEST(WifiProjectionRouting, RoutesOnlyOpenedDynamicIdWhenRegistered) {
    // Given: Wi-Fi is explicitly configured at ID 73, not the SDK ordinal 18.
    auto support = channels::SupportConfiguration::create({{proto::ServiceKey{73},
        proto::ChannelRole::wifi_projection, proto::WifiProjectionConfiguration{}}});
    ASSERT_TRUE(support);
    channels::ServiceRegistry registry;
    WifiSink sink;
    ASSERT_TRUE(registry.register_sink(proto::ChannelRole::wifi_projection, sink));
    channels::Negotiator negotiator{std::move(support.value()), registry};
    const auto discovery = negotiator.start();
    ASSERT_EQ(discovery.services.size(), 1u);
    EXPECT_EQ(discovery.services.front().id, proto::ServiceKey{73});
    const std::vector<std::byte> payload{std::byte{0x80}, std::byte{0x01}};
    EXPECT_FALSE(negotiator.dispatch({73, 0, {}, payload}));
    const auto ordinal = negotiator.open({0, proto::ServiceKey{18}});
    ASSERT_TRUE(ordinal);
    EXPECT_EQ(ordinal.value().status, proto::ChannelOpenStatus::unsupported);
    const auto opened = negotiator.open({0, proto::ServiceKey{73}});
    ASSERT_TRUE(opened);
    ASSERT_EQ(opened.value().status, proto::ChannelOpenStatus::success);
    // When: the request arrives on the negotiated dynamic ID.
    ASSERT_TRUE(negotiator.dispatch({73, 0, {}, payload}));
    // Then: only that route reaches the Wi-Fi sink; ordinal, unknown and stale routes fail.
    EXPECT_EQ(sink.requests, 1);
    EXPECT_FALSE(negotiator.dispatch({18, 0, {}, payload}));
    EXPECT_FALSE(negotiator.dispatch({74, 0, {}, payload}));
    negotiator.reset();
    EXPECT_FALSE(negotiator.dispatch({73, 0, {}, payload}));
    EXPECT_EQ(sink.requests, 1);
}

TEST(WifiProjectionRouting, OmitsServiceWhenSupportOrSinkIsAbsent) {
    // Given: explicitly configured support but no Wi-Fi sink.
    auto support = channels::SupportConfiguration::create({{proto::ServiceKey{73},
        proto::ChannelRole::wifi_projection, proto::WifiProjectionConfiguration{}}});
    ASSERT_TRUE(support);
    channels::ServiceRegistry registry;
    channels::Negotiator missing_sink{std::move(support.value()), registry};
    WifiSink sink;
    channels::Negotiator missing_support{channels::SupportConfiguration{}, registry};
    // When: discovery starts without the complete explicit opt-in.
    EXPECT_TRUE(missing_sink.start().services.empty());
    ASSERT_TRUE(registry.register_sink(proto::ChannelRole::wifi_projection, sink));
    // Then: merely registering a Wi-Fi sink cannot change default/wired discovery.
    EXPECT_TRUE(missing_support.start().services.empty());
}
} // namespace
