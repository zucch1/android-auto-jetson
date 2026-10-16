// SPDX-License-Identifier: GPL-3.0-or-later
#define main sensor_test_receiver_main
#include "../../tools/wired-receiver/main.cpp"
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
    for (const auto& entry : wire.channels()) {
        EXPECT_FALSE(entry.has_media_source_service());
        EXPECT_FALSE(entry.has_bluetooth_service());
        EXPECT_FALSE(entry.has_wifi_projection_service());
        if (!entry.has_sensor_source_service()) { continue; }
        ++sensor_services;
        EXPECT_EQ(entry.id(), 105);
        ASSERT_EQ(entry.sensor_source_service().sensors_size(), 1);
        EXPECT_EQ(entry.sensor_source_service().sensors(0).sensor_type(), sensor::SENSOR_NIGHT_MODE);
    }
    EXPECT_EQ(sensor_services, 1u);
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
    const auto& config = std::get<proto::SensorConfiguration>(services.back().configuration);
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
    EXPECT_EQ(transport.last_sent()[1] & std::byte{0x08}, std::byte{0x08});
    auto message = decode_last();
    ASSERT_TRUE(message);
    ASSERT_EQ(message.value().channel, 105);
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
    const auto& toggle = message.value().payload;
    ASSERT_TRUE(batch.ParseFromArray(toggle.data() + 2, static_cast<int>(toggle.size() - 2)));
    ASSERT_EQ(batch.night_mode_data_size(), 1);
    EXPECT_TRUE(batch.night_mode_data(0).night_mode());
    session.attach_transport(&transport);
    EXPECT_FALSE(sink.set_night_mode(false));
    EXPECT_EQ(transport.sent_count(), before + 3);
}
} // namespace
