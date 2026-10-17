// SPDX-License-Identifier: GPL-3.0-or-later
#include <aa/channels/MicrophoneSink.hpp>
#include <aa/channels/Negotiator.hpp>
#include <aa/protocol/Microphone.hpp>
#include <aa/audio/Microphone.hpp>

#include <aap_protobuf/service/media/shared/message/Config.pb.h>
#include <aap_protobuf/service/media/shared/message/Setup.pb.h>
#include <aap_protobuf/service/media/source/message/MicrophoneRequest.pb.h>
#include <aap_protobuf/service/media/source/message/MicrophoneResponse.pb.h>
#include <aap_protobuf/service/media/source/message/Ack.pb.h>
#include <gtest/gtest.h>

namespace {
namespace proto = aa::protocol;
namespace shared = aap_protobuf::service::media::shared::message;
namespace source = aap_protobuf::service::media::source::message;

template<class Wire>
std::vector<std::byte> payload(unsigned id, const Wire& wire) {
    std::vector<std::byte> bytes{static_cast<std::byte>(id >> 8U),
                               static_cast<std::byte>(id & 255U)};
    for (const char byte : wire.SerializePartialAsString()) {
        bytes.push_back(static_cast<std::byte>(byte));
    }
    return bytes;
}

class MicrophoneWire : public ::testing::Test {
protected:
    std::vector<std::vector<std::byte>> sent;
    aa::channels::MicrophoneSink sink{[this](std::vector<std::byte> bytes) -> aa::core::Result<void> {
        sent.push_back(std::move(bytes));
        return {};
    }};
    aa::core::Result<void> submit(const std::vector<std::byte>& bytes) {
        return sink.on_message({{proto::ChannelRole::microphone, 1, {}}, bytes});
    }
};

TEST_F(MicrophoneWire, PcmSetupUsesConfig8003ReadyTwoAndSingleConfigurationZero) {
    shared::Setup setup;
    setup.set_type(shared::MEDIA_CODEC_AUDIO_PCM);
    const auto bytes = payload(0x8000, setup);
    EXPECT_EQ(proto::encode(proto::MicrophoneSetup{}).value(), bytes);
    ASSERT_TRUE(submit(bytes));
    ASSERT_EQ(sent.size(), 1u);
    EXPECT_EQ(sent[0][0], std::byte{0x80});
    EXPECT_EQ(sent[0][1], std::byte{3});
    shared::Config config;
    ASSERT_TRUE(config.ParseFromArray(sent[0].data() + 2, static_cast<int>(sent[0].size() - 2)));
    EXPECT_EQ(config.status(), shared::Config::STATUS_READY);
    EXPECT_EQ(config.status(), 2);
    EXPECT_EQ(config.max_unacked(), 1u);
    ASSERT_EQ(config.configuration_indices_size(), 1);
    EXPECT_EQ(config.configuration_indices(0), 0u);
    source::Ack ack;
    ack.set_session_id(0);
    ack.set_ack(1);
    ack.add_receive_timestamp_ns(1234);
    ASSERT_TRUE(submit(payload(0x8004, ack)));
    EXPECT_EQ(sent.size(), 1u);
}

TEST_F(MicrophoneWire, RepeatedOpensRefuseCaptureAndClosesSucceedWithoutSessionOrPcm) {
    aa::audio::MicrophoneGate gate;
    EXPECT_EQ(gate.state(), aa::audio::MicrophoneState::disabled);
    for (const bool open : {true, true, false, false, true, false}) {
        source::MicrophoneRequest request;
        request.set_open(open);
        request.set_anc_enabled(true);
        request.set_ec_enabled(false);
        request.set_max_unacked(4);
        const auto bytes = payload(0x8005, request);
        const auto decoded = proto::decode_microphone_request(bytes);
        ASSERT_TRUE(decoded);
        EXPECT_EQ(decoded.value().open, open);
        EXPECT_EQ(decoded.value().anc_enabled, true);
        EXPECT_EQ(decoded.value().ec_enabled, false);
        EXPECT_EQ(decoded.value().max_unacked, 4);
        EXPECT_EQ(proto::encode(decoded.value()).value(), bytes);
        const auto before = sent.size();
        ASSERT_TRUE(submit(bytes));
        ASSERT_EQ(sent.size(), before + 1);
        const auto& reply = sent.back();
        EXPECT_EQ(reply[0], std::byte{0x80});
        EXPECT_EQ(reply[1], std::byte{6});
        source::MicrophoneResponse response;
        ASSERT_TRUE(response.ParseFromArray(reply.data() + 2, static_cast<int>(reply.size() - 2)));
        EXPECT_EQ(response.status(), open ? -250 : 0);
        EXPECT_FALSE(response.has_session_id());
        EXPECT_EQ(reply.size(), open ? 13u : 4u);
        if (open) {
            EXPECT_EQ(reply[3], std::byte{0x86});
            EXPECT_EQ(reply[4], std::byte{0xfe});
            for (std::size_t i = 5; i < 12; ++i) { EXPECT_EQ(reply[i], std::byte{0xff}); }
            EXPECT_EQ(reply[12], std::byte{1});
        }
        EXPECT_EQ(proto::decode_microphone_response(reply).value().status, open ? -250 : 0);
        EXPECT_EQ(gate.state(), aa::audio::MicrophoneState::disabled);
    }
    EXPECT_EQ(sent.size(), 6u);
}

TEST_F(MicrophoneWire, StrictRequestsSetupAndAckRejectMalformedFieldsWithoutReply) {
    const std::vector<std::vector<std::byte>> malformed{
        {}, {std::byte{0x80}},
        {std::byte{0x80}, std::byte{0}},
        {std::byte{0x80}, std::byte{0}, std::byte{8}, std::byte{3}},
        {std::byte{0x80}, std::byte{0}, std::byte{8}, std::byte{0x7f}},
        {std::byte{0x80}, std::byte{5}},
        {std::byte{0x80}, std::byte{5}, std::byte{8}, std::byte{0x80}},
        {std::byte{0x80}, std::byte{4}},
        {std::byte{0x80}, std::byte{4}, std::byte{0x0a}, std::byte{0}}};
    for (const auto& bytes : malformed) { EXPECT_FALSE(submit(bytes)); }
    for (const unsigned id : {0x8000u, 0x8005u, 0x8004u}) {
        const std::vector<std::byte> valid{std::byte{0x80}, static_cast<std::byte>(id & 255u),
                                         std::byte{8}, std::byte{1}};
        const unsigned last_tag = id == 0x8005 ? 4u : (id == 0x8004 ? 3u : 1u);
        for (unsigned tag = 1; tag <= last_tag; ++tag) {
            auto bytes = valid;
            bytes.push_back(static_cast<std::byte>((tag << 3U) | 5U));
            bytes.insert(bytes.end(), 4, std::byte{0});
            EXPECT_FALSE(submit(bytes));
        }
        auto extension = valid;
        extension.insert(extension.end(), {std::byte{0x78}, std::byte{0}});
        EXPECT_FALSE(submit(extension));
        auto truncated = valid;
        truncated.push_back(std::byte{0x80});
        EXPECT_FALSE(submit(truncated));
    }
    const auto valid = proto::encode(proto::MicrophoneSetup{}).value();
    EXPECT_FALSE(sink.on_message({{proto::ChannelRole::input, 1, {}}, valid}));
    EXPECT_FALSE(submit({std::byte{0}, std::byte{0}, std::byte{0}}));
    EXPECT_TRUE(sent.empty());
}

TEST_F(MicrophoneWire, SendFailuresPropagateForSetupOpenAndClose) {
    unsigned attempts = 0;
    aa::channels::MicrophoneSink failing{[&](const std::vector<std::byte>&) -> aa::core::Result<void> {
        ++attempts;
        return aa::Error{aa::ErrorCode::transport_io};
    }};
    for (const auto& bytes : {proto::encode(proto::MicrophoneSetup{}).value(),
                             proto::encode(proto::MicrophoneRequest{true}).value(),
                             proto::encode(proto::MicrophoneRequest{false}).value()}) {
        const auto result = failing.on_message({{proto::ChannelRole::microphone, 1, {}}, bytes});
        ASSERT_FALSE(result);
        EXPECT_EQ(result.error().code(), aa::ErrorCode::transport_io);
    }
    EXPECT_EQ(attempts, 3u);
}

TEST_F(MicrophoneWire, AdvertisementRequiresHandlerAndDispatchRequiresOpenInCurrentGeneration) {
    aa::channels::ServiceRegistry registry;
    auto support = aa::channels::SupportConfiguration::create({
        {proto::ServiceKey{106}, proto::ChannelRole::microphone,
         proto::MicrophoneConfiguration{{16000, 16, 1}}}});
    ASSERT_TRUE(support);
    aa::channels::Negotiator negotiator{std::move(support).value(), registry};
    EXPECT_TRUE(negotiator.start().services.empty());
    EXPECT_EQ(negotiator.open({1, proto::ServiceKey{106}}).value().status,
              proto::ChannelOpenStatus::unsupported);
    ASSERT_TRUE(registry.register_sink(proto::ChannelRole::microphone, sink));
    ASSERT_EQ(negotiator.start().services.size(), 1u);
    const auto bytes = proto::encode(proto::MicrophoneRequest{true}).value();
    EXPECT_FALSE(negotiator.dispatch({106, 1, {}, bytes}));
    ASSERT_TRUE(negotiator.open({1, proto::ServiceKey{106}}));
    ASSERT_TRUE(negotiator.dispatch({106, 1, {}, bytes}));
    ASSERT_EQ(sent.size(), 1u);
    ASSERT_TRUE(registry.unregister_sink(proto::ChannelRole::microphone));
    EXPECT_FALSE(negotiator.dispatch({106, 1, {}, bytes}));
    ASSERT_TRUE(registry.register_sink(proto::ChannelRole::microphone, sink));
    EXPECT_FALSE(negotiator.dispatch({106, 1, {}, bytes}));
    negotiator.reset();
    ASSERT_EQ(negotiator.start().services.size(), 1u);
    EXPECT_FALSE(negotiator.dispatch({106, 1, {}, bytes}));
    ASSERT_TRUE(negotiator.open({1, proto::ServiceKey{106}}));
    ASSERT_TRUE(negotiator.dispatch({106, 1, {}, bytes}));
    EXPECT_EQ(sent.size(), 2u);
}
} // namespace
