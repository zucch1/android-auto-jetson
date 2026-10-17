// SPDX-License-Identifier: GPL-3.0-or-later
#include <aa/channels/AvSink.hpp>
#include <aa/ipc/VideoSocket.hpp>
#include <aa/transport/Frames.hpp>

#include <gtest/gtest.h>

namespace {
namespace p = aa::protocol;
using Bytes = std::vector<std::byte>;

p::ServiceDescriptor audio(p::ChannelRole role = p::ChannelRole::media_audio) {
    return {p::ServiceKey{5}, role, p::AudioConfiguration{{{48000, 16, 2}}}};
}
p::ServiceDescriptor video() {
    return {p::ServiceKey{6}, p::ChannelRole::video,
            p::VideoConfiguration{{{1280, 720, 30}, {800, 480, 30}}}};
}
Bytes data(std::size_t size = 4) {
    Bytes bytes(10 + size, std::byte{0x55});
    bytes[0] = bytes[1] = std::byte{0};
    return bytes;
}
Bytes config(std::size_t size = 3) {
    Bytes bytes(2 + size, std::byte{0x67});
    bytes[0] = std::byte{0};
    bytes[1] = std::byte{1};
    return bytes;
}
struct Raw final : p::MessageSink {
    std::vector<Bytes> received;
    p::MessageHeader header{};
    bool fail{};
    aa::core::Result<void> on_message(const p::MessageView& message) override {
        if (fail) { return aa::Error{aa::ErrorCode::ipc_queue_full}; }
        header = message.header;
        received.emplace_back(message.payload.begin(), message.payload.end());
        return {};
    }
};
class AvSinkWire : public ::testing::Test {
protected:
    Raw raw;
    std::vector<Bytes> sent;
    bool fail_send{};
    aa::channels::AvSink::Send send = [this](Bytes bytes) -> aa::core::Result<void> {
        if (fail_send) { return aa::Error{aa::ErrorCode::transport_io}; }
        sent.push_back(std::move(bytes));
        return {};
    };
    aa::channels::AvSink sink{audio(), raw, send};
    aa::core::Result<void> submit(const Bytes& bytes) {
        return sink.on_message({{p::ChannelRole::media_audio, 42, aa::core::Nanoseconds{123}}, bytes});
    }
    void setup() { ASSERT_TRUE(submit(p::encode(p::AvSinkSetup{p::AvSinkCodec::pcm}).value())); }
    void start(std::int32_t id = -7) { ASSERT_TRUE(submit(p::encode(p::AvSinkStart{id, 0}).value())); }
};

TEST_F(AvSinkWire, LiteralPcmSetupNeverReachesRawAndReadyIsExact) {
    ASSERT_TRUE(submit({std::byte{0x80}, std::byte{0}, std::byte{8}, std::byte{1}}));
    ASSERT_EQ(sent.size(), 1u);
    EXPECT_EQ(sent.front(), p::encode_av_sink_ready().value());
    EXPECT_TRUE(raw.received.empty());
    EXPECT_FALSE(sink.input_focus_allowed());
}

TEST_F(AvSinkWire, DataRequiresStartStripsTimestampAndPreservesLocalHeader) {
    EXPECT_FALSE(submit(data()));
    setup();
    EXPECT_FALSE(submit(data()));
    start();
    ASSERT_TRUE(submit(data()));
    ASSERT_EQ(raw.received.size(), 1u);
    EXPECT_EQ(raw.received[0], Bytes(4, std::byte{0x55}));
    EXPECT_EQ(raw.header.sequence, 42u);
    EXPECT_EQ(raw.header.timestamp, aa::core::Nanoseconds{123});
    EXPECT_EQ(sink.peer_timestamp(), 0x5555555555555555ULL);
    ASSERT_EQ(sent.size(), 2u);
    EXPECT_EQ(sent.back(), p::encode_av_sink_ack(-7).value());
}

TEST_F(AvSinkWire, WrongCodecIndexRoleAndMalformedControlsHaveNoCredit) {
    EXPECT_FALSE(submit(p::encode(p::AvSinkSetup{p::AvSinkCodec::h264}).value()));
    EXPECT_FALSE(submit(p::encode(p::AvSinkStart{1, 0}).value()));
    setup();
    EXPECT_FALSE(submit(p::encode(p::AvSinkStart{1, 1}).value()));
    EXPECT_FALSE(sink.on_message({{p::ChannelRole::system_audio, 0, {}}, data()}));
    EXPECT_FALSE(submit({std::byte{0x80}, std::byte{2}, std::byte{8}, std::byte{1}}));
    EXPECT_FALSE(submit({std::byte{0x80}, std::byte{4}}));
    EXPECT_EQ(sent.size(), 1u);
    EXPECT_TRUE(raw.received.empty());
}

TEST_F(AvSinkWire, DuplicateStartStopSetupAndResetClearStreamWithoutControlCredits) {
    ASSERT_TRUE(submit(p::encode(p::AvSinkStop{}).value()));
    EXPECT_FALSE(submit(p::encode(p::AvSinkStart{0, 0}).value()));
    setup(); start(0); start(0);
    ASSERT_TRUE(submit(data()));
    EXPECT_EQ(sent.back(), p::encode_av_sink_ack(0).value());
    for (int i = 0; i < 2; ++i) { ASSERT_TRUE(submit(p::encode(p::AvSinkStop{}).value())); }
    EXPECT_FALSE(submit(data()));
    start(-2147483647 - 1);
    ASSERT_TRUE(submit(data()));
    EXPECT_EQ(sent.back(), p::encode_av_sink_ack(-2147483647 - 1).value());
    setup();
    EXPECT_FALSE(submit(data()));
    EXPECT_FALSE(sink.peer_timestamp());
    sink.reset();
    EXPECT_FALSE(submit(p::encode(p::AvSinkStart{1, 0}).value()));
    EXPECT_FALSE(sink.peer_timestamp());
    EXPECT_EQ(sent.size(), 4u);
}

TEST_F(AvSinkWire, PcmAlignmentAndConfigPolicy) {
    setup();
    EXPECT_FALSE(submit(config()));
    ASSERT_TRUE(submit(config(0)));
    EXPECT_FALSE(submit(config(0)));
    EXPECT_EQ(sent.size(), 1u);
    start();
    EXPECT_EQ(sent.back(), p::encode_av_sink_ack(-7).value());
    EXPECT_TRUE(raw.received.empty());
    EXPECT_FALSE(submit(data(3)));
    EXPECT_FALSE(submit(data(0)));
    EXPECT_FALSE(submit(config()));
    ASSERT_TRUE(submit(config(0)));
    ASSERT_TRUE(submit(data()));
    EXPECT_EQ(sent.size(), 4u);
    EXPECT_EQ(raw.received.size(), 1u);
}

TEST_F(AvSinkWire, DownstreamAndAckFailuresPropagateWithoutImplicitRedelivery) {
    fail_send = true;
    EXPECT_FALSE(submit(p::encode(p::AvSinkSetup{p::AvSinkCodec::pcm}).value()));
    EXPECT_FALSE(submit(p::encode(p::AvSinkStart{1, 0}).value()));
    fail_send = false;
    setup(); start();
    raw.fail = true;
    auto rejected = submit(data());
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().code(), aa::ErrorCode::ipc_queue_full);
    EXPECT_EQ(sent.size(), 1u);
    raw.fail = false; fail_send = true;
    rejected = submit(data());
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().code(), aa::ErrorCode::transport_io);
    EXPECT_EQ(raw.received.size(), 1u);
    fail_send = false;
    start();
    EXPECT_EQ(raw.received.size(), 1u);
    EXPECT_EQ(sent.size(), 1u);
}

TEST_F(AvSinkWire, AudioRolesMaintainIndependentSessionCredits) {
    setup(); start(-9);
    for (const auto role : {p::ChannelRole::guidance_audio, p::ChannelRole::system_audio,
                            p::ChannelRole::speech_audio}) {
        aa::channels::AvSink other{audio(role), raw, send};
        const auto call = [&](const Bytes& bytes) { return other.on_message({{role, 0, {}}, bytes}); };
        ASSERT_TRUE(call(p::encode(p::AvSinkSetup{p::AvSinkCodec::pcm}).value()));
        ASSERT_TRUE(call(p::encode(p::AvSinkStart{17, 0}).value()));
        ASSERT_TRUE(call(data()));
        EXPECT_EQ(sent.back(), p::encode_av_sink_ack(17).value());
        other.reset();
        ASSERT_TRUE(submit(data()));
        EXPECT_EQ(sent.back(), p::encode_av_sink_ack(-9).value());
    }
}

TEST_F(AvSinkWire, DescriptorMustActuallyHaveMatchingProfiles) {
    for (const auto& descriptor : {
            p::ServiceDescriptor{p::ServiceKey{5}, p::ChannelRole::media_audio, p::AudioConfiguration{}},
            p::ServiceDescriptor{p::ServiceKey{5}, p::ChannelRole::media_audio, p::VideoConfiguration{{{1280,720,30}}}},
            p::ServiceDescriptor{p::ServiceKey{6}, p::ChannelRole::video, p::VideoConfiguration{}}}) {
        aa::channels::AvSink invalid{descriptor, raw, send};
        const auto codec = descriptor.role == p::ChannelRole::video ? p::AvSinkCodec::h264 : p::AvSinkCodec::pcm;
        EXPECT_FALSE(invalid.on_message({{descriptor.role, 0, {}}, p::encode(p::AvSinkSetup{codec}).value()}));
    }
    EXPECT_TRUE(sent.empty());
}

TEST_F(AvSinkWire, VideoStagingDuplicateStartAndNewSessionCacheRules) {
    aa::channels::AvSink v{video(), raw, send};
    const auto call = [&](const Bytes& bytes) { return v.on_message({{p::ChannelRole::video, 8, aa::core::Nanoseconds{321}}, bytes}); };
    ASSERT_TRUE(call(p::encode(p::AvSinkSetup{p::AvSinkCodec::h264}).value()));
    ASSERT_EQ(sent.size(), 2u);
    ASSERT_TRUE(call(config()));
    EXPECT_FALSE(call(config()));
    EXPECT_EQ(sent.size(), 2u);
    ASSERT_TRUE(call(p::encode(p::AvSinkStart{-1, 0}).value()));
    EXPECT_EQ(sent.back(), p::encode_av_sink_ack(-1).value());
    ASSERT_TRUE(call(p::encode(p::AvSinkStart{-1, 0}).value()));
    EXPECT_EQ(sent.size(), 3u);
    ASSERT_TRUE(call(data()));
    const Bytes combined{std::byte{0x67}, std::byte{0x67}, std::byte{0x67},
                         std::byte{0x55}, std::byte{0x55}, std::byte{0x55}, std::byte{0x55}};
    EXPECT_EQ(raw.received.back(), combined);
    EXPECT_EQ(raw.header.sequence, 8u);
    EXPECT_EQ(raw.header.timestamp, aa::core::Nanoseconds{321});
    ASSERT_TRUE(call(data()));
    EXPECT_EQ(raw.received.back().size(), 4u);
    ASSERT_TRUE(call(config()));
    ASSERT_TRUE(call(p::encode(p::AvSinkStart{2, 0}).value()));
    ASSERT_TRUE(call(data()));
    EXPECT_EQ(raw.received.back().size(), 4u);
    ASSERT_TRUE(call(config()));
    ASSERT_TRUE(call(p::encode(p::AvSinkStop{}).value()));
    ASSERT_TRUE(call(p::encode(p::AvSinkStart{3, 0}).value()));
    ASSERT_TRUE(call(data()));
    EXPECT_EQ(raw.received.back().size(), 4u);
}

TEST_F(AvSinkWire, VideoBoundsAreAppliedToCacheAndCombinedAccessUnit) {
    aa::channels::AvSink v{video(), raw, send};
    const auto call = [&](const Bytes& bytes) { return v.on_message({{p::ChannelRole::video, 0, {}}, bytes}); };
    ASSERT_TRUE(call(p::encode(p::AvSinkSetup{p::AvSinkCodec::h264}).value()));
    ASSERT_TRUE(call(p::encode(p::AvSinkStart{4, 0}).value()));
    EXPECT_FALSE(call(config(aa::ipc::kMaxAccessUnitBytes + 1)));
    ASSERT_TRUE(call(config(aa::transport::kMaxMessageBytes - 2)));
    EXPECT_FALSE(call(config()));
    EXPECT_FALSE(call(data(4)));
    EXPECT_TRUE(raw.received.empty());
    EXPECT_EQ(sent.size(), 3u);
}

TEST_F(AvSinkWire, StagedCreditAndHeadersAreClearedBySetupStopAndReset) {
    aa::channels::AvSink v{video(), raw, send};
    const auto call = [&](const Bytes& bytes) { return v.on_message({{p::ChannelRole::video, 0, {}}, bytes}); };
    EXPECT_FALSE(call(config()));
    for (int operation = 0; operation < 3; ++operation) {
        ASSERT_TRUE(call(p::encode(p::AvSinkSetup{p::AvSinkCodec::h264}).value()));
        ASSERT_TRUE(call(config()));
        if (operation == 0) {
            ASSERT_TRUE(call(p::encode(p::AvSinkSetup{p::AvSinkCodec::h264}).value()));
        } else if (operation == 1) {
            ASSERT_TRUE(call(p::encode(p::AvSinkStop{}).value()));
        } else {
            v.reset();
            EXPECT_FALSE(call(p::encode(p::AvSinkStart{8, 0}).value()));
            ASSERT_TRUE(call(p::encode(p::AvSinkSetup{p::AvSinkCodec::h264}).value()));
        }
        const auto before = sent.size();
        ASSERT_TRUE(call(p::encode(p::AvSinkStart{8, 0}).value()));
        EXPECT_EQ(sent.size(), before);
        ASSERT_TRUE(call(data()));
        EXPECT_EQ(raw.received.back().size(), 4u);
        EXPECT_EQ(sent.size(), before + 1);
    }
}

TEST_F(AvSinkWire, StagedAckFailureIsNotRetriedByDuplicateStartOrNewSession) {
    setup();
    ASSERT_TRUE(submit(config(0)));
    fail_send = true;
    const auto result = submit(p::encode(p::AvSinkStart{-17, 0}).value());
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code(), aa::ErrorCode::transport_io);
    fail_send = false;
    start(-17);
    start(20);
    EXPECT_EQ(sent.size(), 1u);
    EXPECT_TRUE(raw.received.empty());
    ASSERT_TRUE(submit(data()));
    EXPECT_EQ(sent.back(), p::encode_av_sink_ack(20).value());
}

TEST_F(AvSinkWire, VideoHeadersAreConsumedOnceOnRawOrAckFailureAndNativeDiscard) {
    aa::channels::AvSink v{video(), raw, send};
    const auto call = [&](const Bytes& bytes) { return v.on_message({{p::ChannelRole::video, 0, {}}, bytes}); };
    ASSERT_TRUE(call(p::encode(p::AvSinkSetup{p::AvSinkCodec::h264}).value()));
    ASSERT_TRUE(call(p::encode(p::AvSinkStart{6, 0}).value()));
    ASSERT_TRUE(call(config()));
    const auto before = sent.size();
    raw.fail = true;
    EXPECT_FALSE(call(data()));
    EXPECT_EQ(sent.size(), before);
    raw.fail = false;
    ASSERT_TRUE(call(data()));
    EXPECT_EQ(raw.received.back().size(), 4u);
    ASSERT_TRUE(call(config()));
    fail_send = true;
    EXPECT_FALSE(call(data()));
    EXPECT_EQ(raw.received.back().size(), 7u);
    fail_send = false;
    ASSERT_TRUE(call(data()));
    EXPECT_EQ(raw.received.back().size(), 4u);
    ASSERT_TRUE(call(config()));
    ASSERT_TRUE(call(p::encode(p::VideoFocusRequest{p::VideoFocusMode::native}).value()));
    const auto delivered = raw.received.size();
    ASSERT_TRUE(call(data()));
    EXPECT_EQ(raw.received.size(), delivered);
    ASSERT_TRUE(call(p::encode(p::VideoFocusRequest{p::VideoFocusMode::projected}).value()));
    ASSERT_TRUE(call(data()));
    EXPECT_EQ(raw.received.back().size(), 4u);
}

TEST_F(AvSinkWire, VideoSetupFocusSendFailureDoesNotPermitStart) {
    unsigned attempts = 0;
    aa::channels::AvSink v{video(), raw, [&](const Bytes&) -> aa::core::Result<void> {
        if (++attempts == 2) { return aa::Error{aa::ErrorCode::transport_io}; }
        return {};
    }};
    const auto call = [&](const Bytes& bytes) { return v.on_message({{p::ChannelRole::video, 0, {}}, bytes}); };
    EXPECT_FALSE(call(p::encode(p::AvSinkSetup{p::AvSinkCodec::h264}).value()));
    EXPECT_FALSE(v.input_focus_allowed());
    EXPECT_FALSE(call(p::encode(p::AvSinkStart{0, 0}).value()));
    EXPECT_FALSE(call(data()));
    EXPECT_EQ(attempts, 2u);
}

TEST_F(AvSinkWire, PcmAlignmentUsesConfiguredProfileAndMalformedDataHasNoCredit) {
    auto descriptor = audio();
    descriptor.configuration = p::AudioConfiguration{{{44100, 24, 3}}};
    aa::channels::AvSink pcm{descriptor, raw, send};
    const auto call = [&](const Bytes& bytes) { return pcm.on_message({{descriptor.role, 0, {}}, bytes}); };
    ASSERT_TRUE(call(p::encode(p::AvSinkSetup{p::AvSinkCodec::pcm}).value()));
    ASSERT_TRUE(call(p::encode(p::AvSinkStart{3, 0}).value()));
    for (std::size_t size = 0; size <= 10; ++size) {
        EXPECT_FALSE(call(Bytes(size, std::byte{0})));
    }
    EXPECT_FALSE(call(data(4)));
    EXPECT_FALSE(call(data(aa::transport::kMaxMessageBytes)));
    EXPECT_EQ(sent.size(), 1u);
    ASSERT_TRUE(call(data(9)));
    EXPECT_EQ(raw.received.back().size(), 9u);
    EXPECT_EQ(sent.back(), p::encode_av_sink_ack(3).value());
}

TEST_F(AvSinkWire, FocusTruthfullyRoutesVideoAndInputAndCreditsNativeDiscard) {
    aa::channels::AvSink v{video(), raw, send};
    const auto call = [&](const Bytes& bytes) { return v.on_message({{p::ChannelRole::video, 0, {}}, bytes}); };
    EXPECT_FALSE(call(p::encode(p::VideoFocusRequest{}).value()));
    EXPECT_FALSE(v.input_focus_allowed());
    setup();
    EXPECT_FALSE(submit(p::encode(p::VideoFocusRequest{}).value()));
    ASSERT_TRUE(call(p::encode(p::AvSinkSetup{p::AvSinkCodec::h264}).value()));
    EXPECT_TRUE(v.input_focus_allowed());
    EXPECT_EQ(sent.back(), p::encode(p::VideoFocusNotification{p::VideoFocusMode::projected, true}).value());
    ASSERT_TRUE(call(p::encode(p::AvSinkStart{4, 0}).value()));
    for (const auto mode : {p::VideoFocusMode::native, p::VideoFocusMode::native_transient,
                            p::VideoFocusMode::projected_no_input_focus, p::VideoFocusMode::projected}) {
        ASSERT_TRUE(call(p::encode(p::VideoFocusRequest{mode}).value()));
        EXPECT_EQ(v.input_focus_allowed(), mode == p::VideoFocusMode::projected);
        EXPECT_EQ(sent.back(), p::encode(p::VideoFocusNotification{mode, false}).value());
        ASSERT_TRUE(call(p::encode(p::VideoFocusRequest{}).value()));
        EXPECT_EQ(sent.back(), p::encode(p::VideoFocusNotification{mode, false}).value());
        const auto before = raw.received.size();
        ASSERT_TRUE(call(data()));
        const bool visible = mode == p::VideoFocusMode::projected || mode == p::VideoFocusMode::projected_no_input_focus;
        EXPECT_EQ(raw.received.size(), before + (visible ? 1u : 0u));
        EXPECT_EQ(sent.back(), p::encode_av_sink_ack(4).value());
    }
    fail_send = true;
    EXPECT_FALSE(call(p::encode(p::VideoFocusRequest{p::VideoFocusMode::native}).value()));
    EXPECT_TRUE(v.input_focus_allowed());
    v.reset();
    EXPECT_FALSE(v.input_focus_allowed());
}
} // namespace
