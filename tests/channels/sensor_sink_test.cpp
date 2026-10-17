// SPDX-License-Identifier: GPL-3.0-or-later
#include <aa/channels/Negotiator.hpp>
#include <aa/channels/SensorSink.hpp>

#include <aap_protobuf/service/control/message/ServiceDiscoveryResponse.pb.h>
#include <aap_protobuf/service/sensorsource/message/SensorBatch.pb.h>
#include <aap_protobuf/service/sensorsource/message/SensorRequest.pb.h>
#include <aap_protobuf/service/sensorsource/message/SensorResponse.pb.h>

#include <gtest/gtest.h>

namespace {
namespace proto = aa::protocol;
namespace sensor = aap_protobuf::service::sensorsource::message;

std::vector<std::byte> request(sensor::SensorType type, std::int64_t period = 0) {
    sensor::SensorRequest wire;
    wire.set_type(type);
    wire.set_min_update_period(period);
    const auto body = wire.SerializeAsString();
    std::vector<std::byte> payload{std::byte{0x80}, std::byte{0x01}};
    for (const char byte : body) { payload.push_back(static_cast<std::byte>(byte)); }
    return payload;
}

class SensorWire : public ::testing::Test {
protected:
    std::vector<std::vector<std::byte>> sent;
    aa::channels::SensorSink sink{aa::input::CapabilityDescriptor::bench_defaults(),
        [this](std::vector<std::byte> payload) -> aa::core::Result<void> {
            sent.push_back(std::move(payload));
            return {};
        }};

    aa::core::Result<void> submit(const std::vector<std::byte>& payload) {
        return sink.on_message({{proto::ChannelRole::sensors, 1, {}}, payload});
    }

    void expect_response(bool accepted) {
        ASSERT_FALSE(sent.empty());
        ASSERT_GE(sent.front().size(), 2u);
        EXPECT_EQ(sent.front()[0], std::byte{0x80});
        EXPECT_EQ(sent.front()[1], std::byte{0x02});
        sensor::SensorResponse response;
        ASSERT_TRUE(response.ParseFromArray(sent.front().data() + 2,
                                            static_cast<int>(sent.front().size() - 2)));
        EXPECT_EQ(response.status(), accepted ? aap_protobuf::shared::STATUS_SUCCESS
                                             : aap_protobuf::shared::STATUS_INVALID_SENSOR);
    }

    void expect_batch(std::size_t index, bool night) {
        ASSERT_LT(index, sent.size());
        const auto& payload = sent[index];
        ASSERT_GE(payload.size(), 2u);
        EXPECT_EQ(payload[0], std::byte{0x80});
        EXPECT_EQ(payload[1], std::byte{0x03});
        sensor::SensorBatch batch;
        ASSERT_TRUE(batch.ParseFromArray(payload.data() + 2, static_cast<int>(payload.size() - 2)));
        ASSERT_EQ(batch.night_mode_data_size(), 1);
        EXPECT_TRUE(batch.night_mode_data(0).has_night_mode());
        EXPECT_EQ(batch.night_mode_data(0).night_mode(), night);
        std::vector<const google::protobuf::FieldDescriptor*> fields;
        batch.GetReflection()->ListFields(batch, &fields);
        ASSERT_EQ(fields.size(), 1u);
        EXPECT_EQ(fields.front()->number(), 10);
    }
};

TEST_F(SensorWire, NightModeRequestAnswersSuccessThenOneCurrentManualStateBatch) {
    ASSERT_TRUE(submit(request(sensor::SENSOR_NIGHT_MODE, 1000)));
    ASSERT_EQ(sent.size(), 2u);
    expect_response(true);
    expect_batch(1, false);
}

TEST_F(SensorWire, EveryOtherPinnedSensorAnswersInvalidSensorWithoutBatch) {
    for (int type = 1; type <= 22; ++type) {
        if (type == 10) { continue; }
        sent.clear();
        ASSERT_TRUE(submit(request(static_cast<sensor::SensorType>(type))));
        ASSERT_EQ(sent.size(), 1u) << type;
        expect_response(false);
    }
}

TEST_F(SensorWire, UnknownSensorAnswersInvalidSensorWithoutBatch) {
    const std::vector<std::byte> payload{std::byte{0x80}, std::byte{0x01}, std::byte{0x08},
        std::byte{0x7f}, std::byte{0x10}, std::byte{0x00}};
    ASSERT_TRUE(submit(payload));
    ASSERT_EQ(sent.size(), 1u);
    expect_response(false);
}

TEST_F(SensorWire, ManualToggleEmitsOneBatchAndLaterRequestUsesCurrentState) {
    ASSERT_TRUE(sink.set_night_mode(true));
    ASSERT_EQ(sent.size(), 1u);
    expect_batch(0, true);
    sent.clear();
    ASSERT_TRUE(submit(request(sensor::SENSOR_NIGHT_MODE)));
    ASSERT_EQ(sent.size(), 2u);
    expect_response(true);
    expect_batch(1, true);
    sent.clear();
    ASSERT_TRUE(sink.set_night_mode(false));
    ASSERT_EQ(sent.size(), 1u);
    expect_batch(0, false);
}

TEST_F(SensorWire, MalformedRequestsAndWrongRoleEmitNothing) {
    const std::vector<std::vector<std::byte>> malformed{
        {}, {std::byte{0x80}}, {std::byte{0x80}, std::byte{0x02}},
        {std::byte{0x80}, std::byte{0x01}, std::byte{0x08}, std::byte{0x0a}},
        {std::byte{0x80}, std::byte{0x01}, std::byte{0x10}, std::byte{0x00}},
        request(sensor::SENSOR_NIGHT_MODE, -1)};
    for (const auto& payload : malformed) { EXPECT_FALSE(submit(payload)); }
    const auto payload = request(sensor::SENSOR_NIGHT_MODE);
    EXPECT_FALSE(sink.on_message({{proto::ChannelRole::input, 1, {}}, payload}));
    EXPECT_TRUE(sent.empty());
}

TEST_F(SensorWire, SendFailuresPropagateWithoutSendingBatchAfterFailedResponse) {
    unsigned attempts = 0;
    aa::channels::SensorSink sink{aa::input::CapabilityDescriptor::bench_defaults(),
        [&attempts](const std::vector<std::byte>&) -> aa::core::Result<void> {
            ++attempts;
            return aa::Error{aa::ErrorCode::transport_io};
        }};
    const auto payload = request(sensor::SENSOR_NIGHT_MODE);
    const auto received = sink.on_message({{proto::ChannelRole::sensors, 1, {}}, payload});
    ASSERT_FALSE(received);
    EXPECT_EQ(received.error().code(), aa::ErrorCode::transport_io);
    EXPECT_EQ(attempts, 1u);
    const auto toggle = sink.set_night_mode(true);
    ASSERT_FALSE(toggle);
    EXPECT_EQ(toggle.error().code(), aa::ErrorCode::transport_io);
    EXPECT_EQ(attempts, 2u);
}

TEST_F(SensorWire, NegotiatedDynamicSensorIdRoutesOnlyAfterOpenAndIsReset) {
    aa::channels::ServiceRegistry registry;
    ASSERT_TRUE(registry.register_sink(proto::ChannelRole::sensors, sink));
    auto support = aa::channels::SupportConfiguration::create({
        {proto::ServiceKey{105}, proto::ChannelRole::sensors,
         proto::SensorConfiguration{{proto::SensorKind::night_mode}}}});
    ASSERT_TRUE(support);
    aa::channels::Negotiator negotiator{std::move(support).value(), registry};
    ASSERT_EQ(negotiator.start().services.size(), 1u);
    const auto payload = request(sensor::SENSOR_NIGHT_MODE);
    EXPECT_FALSE(negotiator.dispatch({105, 1, {}, payload}));
    ASSERT_TRUE(negotiator.open({1, proto::ServiceKey{105}}));
    ASSERT_TRUE(negotiator.dispatch({105, 1, {}, payload}));
    ASSERT_EQ(sent.size(), 2u);
    negotiator.reset();
    EXPECT_FALSE(negotiator.dispatch({105, 1, {}, payload}));
}
} // namespace
