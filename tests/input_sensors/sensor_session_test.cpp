// SPDX-License-Identifier: GPL-3.0-or-later
#define main sensor_test_receiver_main
#include "../../tools/wired-receiver/main.cpp" // NOLINT(bugprone-suspicious-include)
#undef main

#include "../wired_e2e/harness.hpp"

#include <aap_protobuf/service/control/message/ServiceDiscoveryResponse.pb.h>
#include <aap_protobuf/service/sensorsource/message/SensorBatch.pb.h>
#include <aap_protobuf/service/sensorsource/message/SensorResponse.pb.h>

#include <gtest/gtest.h>

namespace {
namespace proto = aa::protocol;
namespace sensor = aap_protobuf::service::sensorsource::message;

class SensorTrust final : public aa::trust::TrustStore {
public:
    aa::trust::Decision lookup(const aa::trust::PhoneIdentity&) const override {
        return aa::trust::Decision::approved;
    }
    aa::core::Result<void> approve_once(const aa::trust::PhoneIdentity&) override { return {}; }
    aa::core::Result<void> forget(const aa::trust::PhoneIdentity&) override { return {}; }
};

TEST(InputSensorsWireSession, NightModeOnlyIsMinimalPhoneRequiredSurfaceWithNoVehicleSensors) {
    const auto capability = aa::input::CapabilityDescriptor::bench_defaults();
    EXPECT_TRUE(capability.vehicle_sensors().empty());
    const auto services = service_descriptors(capability);
    ASSERT_EQ(services.size(), 7u);
    ASSERT_TRUE(aa::channels::SupportConfiguration::create(services));
    const auto encoded = proto::encode(proto::ServiceDiscoveryResponse{services});
    ASSERT_TRUE(encoded);
    aap_protobuf::service::control::message::ServiceDiscoveryResponse wire;
    ASSERT_TRUE(wire.ParseFromArray(encoded.value().data() + 2,
                                    static_cast<int>(encoded.value().size() - 2)));
    ASSERT_EQ(wire.channels_size(), 7);
    unsigned sensor_services = 0;
    unsigned microphone_services = 0;
    for (const auto& entry : wire.channels()) {
        if (entry.id() == 106) {
            ++microphone_services;
            ASSERT_TRUE(entry.has_media_source_service());
            const auto& source = entry.media_source_service();
            EXPECT_EQ(source.available_type(),
                      aap_protobuf::service::media::shared::message::MEDIA_CODEC_AUDIO_PCM);
            EXPECT_TRUE(source.has_available_while_in_call());
            EXPECT_FALSE(source.available_while_in_call());
            ASSERT_TRUE(source.has_audio_config());
            EXPECT_EQ(source.audio_config().sampling_rate(), 16000u);
            EXPECT_EQ(source.audio_config().number_of_bits(), 16u);
            EXPECT_EQ(source.audio_config().number_of_channels(), 1u);
        } else {
            EXPECT_FALSE(entry.has_media_source_service());
        }
        EXPECT_FALSE(entry.has_bluetooth_service());
        EXPECT_FALSE(entry.has_wifi_projection_service());
        if (!entry.has_sensor_source_service()) { continue; }
        ++sensor_services;
        EXPECT_EQ(entry.id(), 105);
        ASSERT_EQ(entry.sensor_source_service().sensors_size(), 1);
        EXPECT_EQ(entry.sensor_source_service().sensors(0).sensor_type(), sensor::SENSOR_NIGHT_MODE);
    }
    EXPECT_EQ(sensor_services, 1u);
    EXPECT_EQ(microphone_services, 1u);
    const auto decoded = proto::decode_service_discovery_response(encoded.value());
    ASSERT_TRUE(decoded);
    EXPECT_EQ(decoded.value().services, services);
    EXPECT_TRUE(capability.vehicle_sensors().empty());
}

TEST(InputSensorsWireSession, UnsupportedVehicleCapabilityIsNotSilentlyAdvertised) {
    const auto bench = aa::input::CapabilityDescriptor::bench_defaults();
    const aa::input::CapabilityDescriptor capability{bench.sources(), bench.keycodes(),
        bench.touch_viewport(), {aa::input::SensorChannel::speed}, bench.night_mode()};
    const auto services = service_descriptors(capability);
    const auto service = std::find_if(services.begin(), services.end(), [](const auto& entry) {
        return entry.id.value == 105 && entry.role == proto::ChannelRole::sensors;
    });
    ASSERT_NE(service, services.end());
    ASSERT_TRUE(std::holds_alternative<proto::SensorConfiguration>(service->configuration));
    const auto& config = std::get<proto::SensorConfiguration>(service->configuration);
    ASSERT_EQ(config.kinds.size(), 2u);
    EXPECT_EQ(static_cast<std::int32_t>(config.kinds[1]), 3);
    EXPECT_FALSE(aa::channels::SupportConfiguration::create(services));
}

TEST(InputSensorsWireSession, RequestAndToggleUseEncryptedNegotiatedServiceAndFailAfterLinkReset) {
    SensorTrust trust;
    aa::session::SteadyClock clock;
    wiredtest::CaptureSink capture;
    aa::diagnostics::Diagnostics diagnostics{capture};
    wiredtest::ScriptedTransport transport;
    aa::app::WiredSession session{{trust, clock, diagnostics}};
    aa::channels::SensorSink sink{aa::input::CapabilityDescriptor::bench_defaults(),
        [&session](std::vector<std::byte> payload) {
            return session.send_channel_payload(proto::ChannelRole::sensors, std::move(payload));
        }};
    ASSERT_TRUE(session.configure(service_descriptors(aa::input::CapabilityDescriptor::bench_defaults())));
    ASSERT_TRUE(session.register_sink(proto::ChannelRole::sensors, sink));
    session.attach_transport(&transport);
    EXPECT_FALSE(sink.set_night_mode(false));
    EXPECT_EQ(transport.sent_count(), 0u);
    auto cipher = std::make_unique<wiredtest::FakeHandshakeCipher>();
    auto* peer_cipher = cipher.get();
    session.attach_cipher(std::move(cipher));
    wiredtest::walk_auth(session, transport);
    ASSERT_TRUE(session.authenticated());
    transport.push_inbound(wiredtest::discovery_request_frame());
    ASSERT_TRUE(session.poll_transport());
    transport.push_inbound(wiredtest::open_request_frame(105));
    ASSERT_TRUE(session.poll_transport());

    aa::transport::FrameStreamParser parser;
    aa::transport::MessageReassembler reassembler{peer_cipher};
    auto decode_last = [&]() -> aa::core::Result<aa::transport::Message> {
        const auto frames = parser.feed(transport.last_sent());
        if (!frames) { return frames.error(); }
        if (frames.value().size() != 1) { return aa::Error{aa::ErrorCode::invalid_argument}; }
        auto message = reassembler.push(frames.value().front());
        if (!message) { return message.error(); }
        if (!message.value()) { return aa::Error{aa::ErrorCode::invalid_argument}; }
        return std::move(*message.value());
    };

    const auto before = transport.sent_count();
    const std::vector<std::byte> request{std::byte{0x80}, std::byte{0x01}, std::byte{0x08},
        std::byte{0x0a}, std::byte{0x10}, std::byte{0x00}};
    transport.push_inbound(wiredtest::service_frame(105, request));
    ASSERT_TRUE(session.poll_transport());
    ASSERT_EQ(transport.sent_count(), before + 2);
    EXPECT_EQ(transport.last_sent()[0], std::byte{105});
    EXPECT_EQ(transport.last_sent()[1] & std::byte{0x0c}, std::byte{0x08});
    auto message = decode_last();
    ASSERT_TRUE(message);
    ASSERT_EQ(message.value().channel, 105);
    EXPECT_EQ(message.value().kind, aa::transport::MessageKind::specific);
    const auto& payload = message.value().payload;
    ASSERT_GE(payload.size(), 2u);
    EXPECT_EQ(payload[0], std::byte{0x80});
    EXPECT_EQ(payload[1], std::byte{0x03});
    sensor::SensorBatch batch;
    ASSERT_TRUE(batch.ParseFromArray(payload.data() + 2, static_cast<int>(payload.size() - 2)));
    ASSERT_EQ(batch.night_mode_data_size(), 1);
    EXPECT_FALSE(batch.night_mode_data(0).night_mode());

    ASSERT_TRUE(sink.set_night_mode(true));
    ASSERT_EQ(transport.sent_count(), before + 3);
    message = decode_last();
    ASSERT_TRUE(message);
    EXPECT_EQ(transport.last_sent()[1] & std::byte{0x0c}, std::byte{0x08});
    EXPECT_EQ(message.value().channel, 105);
    EXPECT_EQ(message.value().kind, aa::transport::MessageKind::specific);
    const auto& toggle = message.value().payload;
    ASSERT_TRUE(batch.ParseFromArray(toggle.data() + 2, static_cast<int>(toggle.size() - 2)));
    ASSERT_EQ(batch.night_mode_data_size(), 1);
    EXPECT_TRUE(batch.night_mode_data(0).night_mode());
    session.attach_transport(&transport);
    EXPECT_FALSE(sink.set_night_mode(false));
    EXPECT_EQ(transport.sent_count(), before + 3);
}
class InBandSession : public ::testing::Test {
protected:
    SensorTrust trust;
    aa::session::SteadyClock clock;
    wiredtest::CaptureSink capture;
    aa::diagnostics::Diagnostics diagnostics{capture};
    wiredtest::ScriptedTransport transport;
    aa::app::WiredSession session{{trust, clock, diagnostics}};
    aa::channels::InputSink input_sink;
    aa::channels::SensorSink sensor_sink{aa::input::CapabilityDescriptor::bench_defaults(),
        [this](std::vector<std::byte> payload) {
            return session.send_channel_payload(proto::ChannelRole::sensors, std::move(payload));
        }};
    aa::transport::PayloadCipher* peer_cipher{nullptr};

    void SetUp() override {
        ASSERT_TRUE(session.configure(service_descriptors(aa::input::CapabilityDescriptor::bench_defaults())));
        ASSERT_TRUE(session.register_sink(proto::ChannelRole::sensors, sensor_sink));
        ASSERT_TRUE(session.register_sink(proto::ChannelRole::input, input_sink));
        session.attach_transport(&transport);
        auto cipher = std::make_unique<wiredtest::FakeHandshakeCipher>();
        peer_cipher = cipher.get();
        session.attach_cipher(std::move(cipher));
        wiredtest::walk_auth(session, transport);
        ASSERT_TRUE(session.authenticated());
        transport.push_inbound(wiredtest::discovery_request_frame());
        ASSERT_TRUE(session.poll_transport());
    }

    aa::core::Result<bool> receive(std::uint8_t channel, std::vector<std::byte> payload) {
        const auto frames = aa::transport::encode_message(
            {channel, aa::transport::MessageKind::specific, std::move(payload)},
            aa::transport::Encryption::encrypted, peer_cipher);
        if (!frames) { return frames.error(); }
        for (const auto& frame : frames.value()) { transport.push_inbound(frame); }
        return session.poll_transport();
    }

    aa::core::Result<aa::transport::Message> last_message() {
        aa::transport::FrameStreamParser parser;
        aa::transport::MessageReassembler reassembler{peer_cipher};
        const auto frames = parser.feed(transport.last_sent());
        if (!frames) { return frames.error(); }
        if (frames.value().size() != 1) { return aa::Error{aa::ErrorCode::invalid_argument}; }
        auto message = reassembler.push(frames.value().front());
        if (!message) { return message.error(); }
        if (!message.value()) { return aa::Error{aa::ErrorCode::invalid_argument}; }
        return std::move(*message.value());
    }

    void expect_open(std::uint8_t channel, proto::ChannelOpenStatus status) {
        EXPECT_EQ(transport.last_sent()[0], static_cast<std::byte>(channel));
        EXPECT_EQ(transport.last_sent()[1] & std::byte{0x0c}, std::byte{0x0c});
        const auto message = last_message();
        ASSERT_TRUE(message);
        EXPECT_EQ(message.value().channel, channel);
        EXPECT_EQ(message.value().kind, aa::transport::MessageKind::control);
        const auto response = proto::decode_channel_open_response(message.value().payload);
        ASSERT_TRUE(response);
        EXPECT_EQ(response.value().status, status);
        EXPECT_FALSE(session.failure());
    }

    const std::vector<std::byte> sensor_request{std::byte{0x80}, std::byte{0x01}, std::byte{0x08},
        std::byte{0x0a}, std::byte{0x10}, std::byte{0x00}};
};

TEST_F(InBandSession, OpenOnSensorServiceRepliesOnSameEncryptedControlChannelThenDispatches) {
    ASSERT_TRUE(receive(105, proto::encode(proto::ChannelOpenRequest{1, {105}}).value()));
    expect_open(105, proto::ChannelOpenStatus::success);
    EXPECT_EQ(session.channel_opens(), 1u);
    const auto before = transport.sent_count();
    ASSERT_TRUE(receive(105, sensor_request));
    EXPECT_EQ(transport.sent_count(), before + 2);
    EXPECT_EQ(session.channel_opens(), 1u);
    EXPECT_EQ(transport.last_sent()[1] & std::byte{0x0c}, std::byte{0x08});
    const auto data = last_message();
    ASSERT_TRUE(data);
    EXPECT_EQ(data.value().channel, 105);
    EXPECT_EQ(data.value().kind, aa::transport::MessageKind::specific);
    ASSERT_GE(data.value().payload.size(), 2u);
    EXPECT_EQ(data.value().payload[0], std::byte{0x80});
    EXPECT_EQ(data.value().payload[1], std::byte{0x03});
    const auto before_unsupported = transport.sent_count();
    ASSERT_TRUE(receive(105, {std::byte{0x80}, std::byte{0x01}, std::byte{0x08},
                              std::byte{0x03}, std::byte{0x10}, std::byte{0x00}}));
    ASSERT_EQ(transport.sent_count(), before_unsupported + 1);
    EXPECT_EQ(transport.last_sent()[1] & std::byte{0x0c}, std::byte{0x08});
    const auto response = last_message();
    ASSERT_TRUE(response);
    EXPECT_EQ(response.value().channel, 105);
    EXPECT_EQ(response.value().kind, aa::transport::MessageKind::specific);
    ASSERT_GE(response.value().payload.size(), 2u);
    EXPECT_EQ(response.value().payload[0], std::byte{0x80});
    EXPECT_EQ(response.value().payload[1], std::byte{0x02});
}

TEST_F(InBandSession, UnknownOpenIsUnsupportedAndDoesNotFailSession) {
    ASSERT_TRUE(receive(250, proto::encode(proto::ChannelOpenRequest{1, {250}}).value()));
    expect_open(250, proto::ChannelOpenStatus::unsupported);
    EXPECT_EQ(session.channel_opens(), 0u);
    ASSERT_TRUE(receive(105, sensor_request));
    EXPECT_EQ(session.channel_opens(), 1u);
}

TEST_F(InBandSession, MismatchedBodyDoesNotOpenEitherAdvertisedRoute) {
    ASSERT_TRUE(receive(105, proto::encode(proto::ChannelOpenRequest{1, {104}}).value()));
    expect_open(105, proto::ChannelOpenStatus::unsupported);
    EXPECT_EQ(session.channel_opens(), 0u);
    ASSERT_TRUE(receive(105, sensor_request));
    EXPECT_EQ(session.channel_opens(), 1u);
    ASSERT_TRUE(receive(104, {std::byte{0x80}, std::byte{0x04}}));
    EXPECT_EQ(session.channel_opens(), 2u);
}

TEST_F(InBandSession, PriorityOptionalInBandOpenUsesPartialParseButControlDecoderStaysStrict) {
    const std::vector<std::byte> payload{std::byte{0}, std::byte{7}, std::byte{0x10}, std::byte{105}};
    EXPECT_FALSE(proto::decode_channel_open_request(payload));
    const auto partial = proto::decode_in_band_channel_open_request(payload);
    ASSERT_TRUE(partial);
    EXPECT_EQ(partial.value().priority, 0);
    ASSERT_TRUE(receive(105, payload));
    expect_open(105, proto::ChannelOpenStatus::success);
    EXPECT_EQ(session.channel_opens(), 1u);
}

TEST_F(InBandSession, AbsentInvalidAndMalformedBodyServiceIdsUseFrameAddress) {
    for (const auto& payload : std::vector<std::vector<std::byte>>{
             {std::byte{0}, std::byte{7}},
             {std::byte{0}, std::byte{7}, std::byte{0x10}, std::byte{0}},
             {std::byte{0}, std::byte{7}, std::byte{0xff}}}) {
        ASSERT_TRUE(receive(105, payload));
        expect_open(105, proto::ChannelOpenStatus::success);
    }
    EXPECT_EQ(session.channel_opens(), 3u);
}

TEST_F(InBandSession, SensorRequestImplicitlyOpensAdvertisedUnopenedRouteExactlyOnce) {
    const auto before = transport.sent_count();
    ASSERT_TRUE(receive(105, sensor_request));
    EXPECT_EQ(transport.sent_count(), before + 2);
    EXPECT_EQ(session.channel_opens(), 1u);
    ASSERT_TRUE(receive(105, sensor_request));
    EXPECT_EQ(session.channel_opens(), 1u);
    EXPECT_FALSE(session.failure());
}

TEST_F(InBandSession, RegisteredInputAppearsAs104InputSourceServiceInDiscoveryBytes) {
    const auto message = last_message();
    ASSERT_TRUE(message);
    EXPECT_EQ(transport.last_sent()[1] & std::byte{0x0c}, std::byte{0x08});
    EXPECT_EQ(message.value().channel, 0);
    EXPECT_EQ(message.value().kind, aa::transport::MessageKind::specific);
    aap_protobuf::service::control::message::ServiceDiscoveryResponse wire;
    const auto& payload = message.value().payload;
    ASSERT_TRUE(wire.ParseFromArray(payload.data() + 2, static_cast<int>(payload.size() - 2)));
    ASSERT_EQ(wire.channels_size(), 2);
    bool found = false;
    for (const auto& service : wire.channels()) {
        if (service.id() != 104) { continue; }
        found = true;
        EXPECT_TRUE(service.has_input_source_service());
    }
    EXPECT_TRUE(found);
}

TEST_F(InBandSession, CloseNotificationsAreCountedAndNeverFailEvenForUnknownChannels) {
    const auto before = transport.sent_count();
    ASSERT_TRUE(receive(105, {std::byte{0}, std::byte{9}}));
    ASSERT_TRUE(receive(250, {std::byte{0}, std::byte{9}}));
    EXPECT_EQ(session.channel_closes(), 2u);
    EXPECT_EQ(session.channel_opens(), 0u);
    EXPECT_EQ(transport.sent_count(), before);
    EXPECT_FALSE(session.failure());
}

TEST_F(InBandSession, UnknownDataRejectReachesGetterAndExistingPollErrorLog) {
    const auto received = receive(250, sensor_request);
    ASSERT_FALSE(received);
    EXPECT_EQ(received.error().code(), aa::ErrorCode::channel_not_registered);
    const auto reject = session.last_reject();
    ASSERT_TRUE(reject);
    EXPECT_EQ(reject->channel, 250);
    EXPECT_EQ(reject->msg_id, 0x8001);
    testing::internal::CaptureStdout();
    log_line(poll_error_json(received.error().code(), reject));
    const auto line = testing::internal::GetCapturedStdout();
    EXPECT_EQ(line, "{\"event\":\"err\",\"detail\":\"poll\",\"code\":\"channel_not_registered\","
                    "\"reject_channel\":250,\"reject_msg_id\":32769}\n");
    ASSERT_TRUE(receive(105, sensor_request));
    EXPECT_FALSE(session.last_reject());
    session.attach_transport(&transport);
    EXPECT_FALSE(session.last_reject());
    EXPECT_EQ(session.channel_closes(), 0u);
}

TEST_F(InBandSession, PollErrorsWithoutRejectedFrameKeepOriginalJsonShape) {
    EXPECT_EQ(poll_error_json(aa::ErrorCode::transport_closed, std::nullopt),
              "{\"event\":\"err\",\"detail\":\"poll\",\"code\":\"transport_closed\"}");
}
} // namespace
