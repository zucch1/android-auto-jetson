// SPDX-License-Identifier: GPL-3.0-or-later
// Module contract smoke (task 14): session transitions, channel dispatch and
// capability defaults, IPC single-consumer admission and wire names, trust
// fail-closed admission, config boundary validation and the framed transport
// interface are usable exactly as documented.
#include <aa/channels/Services.hpp>
#include <aa/config/Config.hpp>
#include <aa/core/Result.hpp>
#include <aa/ipc/Control.hpp>
#include <aa/session/Session.hpp>
#include <aa/trust/Trust.hpp>
#include <aa/transport/Transport.hpp>

#include <array>
#include <cstddef>
#include <span>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

namespace {

struct CountingSink final : aa::protocol::MessageSink {
    int calls{0};
    aa::core::Result<void> on_message(const aa::protocol::MessageView&) override {
        ++calls;
        return {};
    }
};

struct LoopbackTransport final : aa::transport::Transport {
    aa::transport::Kind kind() const noexcept override { return aa::transport::Kind::tcp; }
    aa::core::Result<void> open(aa::core::CancellationToken) override {
        open_ = true;
        return {};
    }
    aa::core::Result<void> send(std::span<const std::byte> frame) override {
        if (!open_) {
            return aa::Error{aa::ErrorCode::transport_closed};
        }
        stored_.assign(frame.begin(), frame.end());
        return {};
    }
    aa::core::Result<std::vector<std::byte>> receive() override {
        if (!open_) {
            return aa::Error{aa::ErrorCode::transport_closed};
        }
        return stored_;
    }
    void close() noexcept override { open_ = false; }

    bool open_{false};
    std::vector<std::byte> stored_{};
};

TEST(SessionState, AcceptsLegalTransition) {
    // Given: a fresh state machine.
    aa::session::StateMachine machine;
    // When: a table-legal edge is applied.
    const auto result = machine.transition(aa::session::State::discovering);
    // Then: the state moves and is reported.
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value(), aa::session::State::discovering);
    EXPECT_EQ(machine.state(), aa::session::State::discovering);
}

TEST(SessionState, RejectsIllegalTransitionWithTypedError) {
    // Given: a machine still disconnected.
    aa::session::StateMachine machine;
    // When: a direct jump to active is attempted.
    const auto result = machine.transition(aa::session::State::active);
    // Then: the typed error names the illegal transition and state is unchanged.
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code(), aa::ErrorCode::session_illegal_transition);
    EXPECT_EQ(machine.state(), aa::session::State::disconnected);
}

TEST(Channels, BenchDefaultsOmitUnsupportedCapabilities) {
    // Given: the capability profile advertised on the bench.
    const auto profile = aa::channels::CapabilityProfile::bench_defaults();
    // When/Then: no backend has been supplied, so nothing is advertised.
    EXPECT_FALSE(profile.advertises(aa::protocol::ChannelRole::video));
    EXPECT_FALSE(profile.advertises(aa::protocol::ChannelRole::media_audio));
    EXPECT_FALSE(profile.advertises(aa::protocol::ChannelRole::input));
    EXPECT_FALSE(profile.advertises(aa::protocol::ChannelRole::microphone));
    EXPECT_FALSE(profile.advertises(aa::protocol::ChannelRole::sensors));
    EXPECT_EQ(profile.primary, (aa::protocol::VideoProfile{1280, 720, 30}));
    EXPECT_EQ(profile.fallback, (aa::protocol::VideoProfile{800, 480, 30}));
}

TEST(Channels, LocalDiagnosticsRoutesWithoutExposingWireDispatch) {
    // Given: a registry with one registered local diagnostics sink.
    aa::channels::ServiceRegistry registry;
    CountingSink sink;
    ASSERT_TRUE(registry.register_sink(aa::protocol::ChannelRole::diagnostics, sink).has_value());
    const std::array<std::byte, 2> payload{std::byte{0x01}, std::byte{0x02}};
    const aa::protocol::MessageView message{
        {aa::protocol::ChannelRole::diagnostics, 3, aa::core::Nanoseconds{125}}, payload};
    // When: the message is dispatched and then an unregistered channel is used.
    const auto delivered = registry.dispatch_local(message);
    const auto missing = registry.dispatch_local({{aa::protocol::ChannelRole::input, 0,
                                             aa::core::Nanoseconds{}},
                                            {}});
    // Then: the sink saw it and the unregistered channel is a typed error.
    ASSERT_TRUE(delivered.has_value());
    EXPECT_EQ(sink.calls, 1);
    ASSERT_FALSE(missing.has_value());
    EXPECT_EQ(missing.error().code(), aa::ErrorCode::protocol_unsupported_channel);
}

TEST(Ipc, SingleConsumerRuleFailsClosedForEveryWrongShape) {
    // Given: an active consumer that owns the registered name.
    const aa::ipc::ConsumerIdentity active{aa::core::ConsumerId{1}, 1000U};
    // When/Then: every wrong shape of caller is denied by default.
    EXPECT_EQ(aa::ipc::authorize({{aa::core::ConsumerId{1}, 1001U}, active, true}),
              aa::ipc::Authorization::wrong_uid);
    EXPECT_EQ(aa::ipc::authorize({{aa::core::ConsumerId{1}, 1000U}, active, false}),
              aa::ipc::Authorization::unowned_name);
    EXPECT_EQ(aa::ipc::authorize({{aa::core::ConsumerId{2}, 1000U}, active, true}),
              aa::ipc::Authorization::not_active_consumer);
    EXPECT_EQ(aa::ipc::authorize({active, active, true}),
              aa::ipc::Authorization::allowed);
}

TEST(Ipc, WireNamesAreStable) {
    // Given/When/Then: the D-Bus method names are the frozen wire contract.
    EXPECT_EQ(aa::ipc::wire_name(aa::ipc::ControlMethod::register_consumer),
              "RegisterConsumer");
    EXPECT_EQ(aa::ipc::wire_name(aa::ipc::ControlMethod::request_add_phone),
              "RequestAddPhone");
}

TEST(Trust, UnknownPhoneFailsClosed) {
    // Given/When/Then: only an approved phone may hold a session.
    EXPECT_FALSE(aa::trust::allows_session(aa::trust::Decision::unknown));
    EXPECT_FALSE(aa::trust::allows_session(aa::trust::Decision::rejected));
    EXPECT_TRUE(aa::trust::allows_session(aa::trust::Decision::approved));
}

TEST(Config, UnconfirmedJurisdictionFailsClosed) {
    // Given: a config whose jurisdiction was never install-time confirmed.
    aa::config::Config candidate{};
    candidate.credential_path = "/etc/aa/hu.pem";
    candidate.wireless.jurisdiction = "US";
    // When: the boundary validates it.
    const auto result = aa::config::validate(std::move(candidate));
    // Then: wireless startup stays closed with the typed error.
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code(), aa::ErrorCode::config_jurisdiction_unconfirmed);
}

TEST(Config, ConfirmedBenchConfigValidates) {
    // Given: the bench config with confirmed jurisdiction and disabled extras.
    aa::config::Config candidate{};
    candidate.credential_path = "/etc/aa/hu.pem";
    candidate.wireless.jurisdiction = "US";
    candidate.wireless.jurisdiction_confirmed = true;
    // When: the boundary validates it.
    const auto result = aa::config::validate(std::move(candidate));
    // Then: the typed config is returned with conservative defaults.
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value().wireless.channel, 36);
    EXPECT_FALSE(result.value().microphone_enabled);
    EXPECT_FALSE(result.value().sensors_enabled);
}

TEST(Transport, FramedInterfaceIsImplementableAndTyped) {
    // Given: a loopback implementation of the framed-message contract.
    LoopbackTransport transport;
    const std::array<std::byte, 3> frame{std::byte{0x01}, std::byte{0x02}, std::byte{0x03}};
    // When: a frame round-trips and the channel is then closed.
    ASSERT_TRUE(transport.open({}).has_value());
    ASSERT_TRUE(transport.send(frame).has_value());
    const auto received = transport.receive();
    ASSERT_TRUE(received.has_value());
    EXPECT_EQ(received.value().size(), std::size_t{3});
    transport.close();
    // Then: a closed channel reports the typed error instead of data.
    const auto closed = transport.receive();
    ASSERT_FALSE(closed.has_value());
    EXPECT_EQ(closed.error().code(), aa::ErrorCode::transport_closed);
}

} // namespace
