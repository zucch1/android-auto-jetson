// SPDX-License-Identifier: GPL-3.0-or-later
#include "control_fixture.hpp"

namespace {
namespace p = aa::protocol;
namespace tr = aa::transport;
using Bytes = std::vector<std::byte>;

Bytes data() {
    return {std::byte{0}, std::byte{0}, std::byte{0xff}, std::byte{2}, std::byte{3},
            std::byte{4}, std::byte{5}, std::byte{6}, std::byte{7}, std::byte{8},
            std::byte{0x10}, std::byte{0x20}, std::byte{0x30}, std::byte{0x40}};
}

class WiredAvSink : public wiredcontrol::WiredControl {
protected:
    wiredtest::RecordingSink video;
    wiredtest::RecordingSink audio;

    void SetUp() override {
        WiredControl::SetUp();
        ASSERT_TRUE(session.register_av_sink(p::ChannelRole::video, video));
        ASSERT_TRUE(session.register_av_sink(p::ChannelRole::media_audio, audio));
        ASSERT_TRUE(session.announce_services());
    }

    aa::core::Result<bool> service(std::uint8_t channel, Bytes payload) {
        auto frames = tr::encode_message({channel, tr::MessageKind::specific, std::move(payload)},
                                         tr::Encryption::encrypted, cipher);
        if (!frames) { return frames.error(); }
        for (auto &frame : frames.value()) { transport.push_inbound(std::move(frame)); }
        return session.poll_transport();
    }

    void reply(std::uint8_t channel, const Bytes &expected) {
        const auto frame = tr::decode_frame(transport.last_sent());
        ASSERT_TRUE(frame);
        EXPECT_EQ(frame.value().header.channel, channel);
        EXPECT_EQ(frame.value().header.kind, tr::MessageKind::specific);
        EXPECT_EQ(frame.value().header.encryption, tr::Encryption::encrypted);
        const auto opened = cipher->open(frame.value().payload);
        ASSERT_TRUE(opened);
        EXPECT_EQ(opened.value(), expected);
    }

    void setup_video() {
        ASSERT_TRUE(service(99, p::encode(p::AvSinkSetup{p::AvSinkCodec::h264}).value()));
    }

    void expect_no_focus() {
        const auto before = transport.sent_count();
        const auto result = session.submit_input(aa::ipc::KeyEvent{66, true});
        ASSERT_FALSE(result);
        EXPECT_EQ(result.error().code(), aa::ErrorCode::channel_unsupported_capability);
        EXPECT_EQ(transport.sent_count(), before);
    }
};

TEST_F(WiredAvSink, EncryptedAudioSetupStartDataAndExactSignedCredits) {
    const auto before = transport.sent_count();
    ASSERT_TRUE(service(100, {std::byte{0x80}, std::byte{0}, std::byte{8}, std::byte{1}}));
    EXPECT_EQ(transport.sent_count(), before + 1);
    reply(100, p::encode_av_sink_ready().value());
    EXPECT_TRUE(audio.payloads.empty());
    EXPECT_FALSE(service(100, data()));
    ASSERT_TRUE(service(100, {std::byte{0}, std::byte{1}}));
    EXPECT_EQ(transport.sent_count(), before + 1);
    for (const std::int32_t id : {-7, 0}) {
        ASSERT_TRUE(service(100, p::encode(p::AvSinkStart{id, 0}).value()));
        reply(100, p::encode_av_sink_ack(id).value());
        const auto sent = transport.sent_count();
        clock.set(aa::core::Nanoseconds{123456});
        ASSERT_TRUE(service(100, data()));
        EXPECT_EQ(transport.sent_count(), sent + 1);
        reply(100, p::encode_av_sink_ack(id).value());
        EXPECT_EQ(audio.payloads.back(), (Bytes{std::byte{0x10}, std::byte{0x20},
                                               std::byte{0x30}, std::byte{0x40}}));
        EXPECT_EQ(audio.headers.back().timestamp, aa::core::Nanoseconds{123456});
        ASSERT_TRUE(service(100, p::encode(p::AvSinkStop{}).value()));
        EXPECT_FALSE(service(100, data()));
        ASSERT_TRUE(service(100, {std::byte{0}, std::byte{1}}));
    }
    expect_ping();
}

TEST_F(WiredAvSink, VideoFocusGatesInputWithoutChangingServiceOpenControl) {
    expect_no_focus();
    ASSERT_TRUE(service(99, p::encode(p::ChannelOpenRequest{0, p::ServiceKey{99}}).value()));
    const auto opened = tr::decode_frame(transport.last_sent());
    ASSERT_TRUE(opened);
    EXPECT_EQ(opened.value().header.kind, tr::MessageKind::control);
    EXPECT_EQ(opened.value().header.encryption, tr::Encryption::encrypted);
    const auto before = transport.sent_count();
    setup_video();
    EXPECT_EQ(transport.sent_count(), before + 2);
    reply(99, p::encode(p::VideoFocusNotification{p::VideoFocusMode::projected, true}).value());
    ASSERT_TRUE(session.submit_input(aa::ipc::KeyEvent{66, true}));
    for (const auto mode : {p::VideoFocusMode::native, p::VideoFocusMode::native_transient,
                            p::VideoFocusMode::projected_no_input_focus}) {
        ASSERT_TRUE(service(99, p::encode(p::VideoFocusRequest{mode}).value()));
        reply(99, p::encode(p::VideoFocusNotification{mode, false}).value());
        expect_no_focus();
    }
    ASSERT_TRUE(service(99, p::encode(p::VideoFocusRequest{p::VideoFocusMode::projected}).value()));
    ASSERT_TRUE(session.submit_input(aa::ipc::KeyEvent{66, false}));
    expect_ping();
}

TEST_F(WiredAvSink, RediscoveryBothPathsAndServiceCloseResetWithoutLosingRegistrations) {
    for (const int reset : {0, 1, 2}) {
        SCOPED_TRACE(reset);
        setup_video();
        ASSERT_TRUE(service(99, p::encode(p::AvSinkStart{42, 0}).value()));
        ASSERT_TRUE(service(99, {std::byte{0}, std::byte{1}, std::byte{0x67}}));
        ASSERT_TRUE(service(100, p::encode(p::AvSinkSetup{}).value()));
        ASSERT_TRUE(service(100, p::encode(p::AvSinkStart{-9, 0}).value()));
        if (reset == 0) { ASSERT_TRUE(session.announce_services()); }
        if (reset == 1) { ASSERT_TRUE(receive({std::byte{0}, std::byte{5}})); }
        if (reset == 2) { ASSERT_TRUE(service(99, {std::byte{0}, std::byte{9}})); }
        expect_no_focus();
        EXPECT_FALSE(service(99, data()));
        EXPECT_FALSE(service(99, p::encode(p::AvSinkStart{42, 0}).value()));
        if (reset == 2) {
            ASSERT_TRUE(service(100, data()));
            reply(100, p::encode_av_sink_ack(-9).value());
        } else {
            EXPECT_FALSE(service(100, data()));
        }
        setup_video();
        ASSERT_TRUE(service(99, p::encode(p::AvSinkStart{0, 0}).value()));
        ASSERT_TRUE(service(99, data()));
        EXPECT_EQ(video.payloads.back().size(), 4U);
    }
    expect_ping();
}

TEST_F(WiredAvSink, FreshLinkResetsReadyCacheAndRequiresAuthenticationAgain) {
    setup_video();
    ASSERT_TRUE(service(99, {std::byte{0}, std::byte{1}, std::byte{0x67}}));
    session.attach_transport(&transport);
    expect_no_focus();
    EXPECT_FALSE(session.authenticated());
    auto owned = std::make_unique<wiredtest::FakeHandshakeCipher>();
    cipher = owned.get();
    session.attach_cipher(std::move(owned));
    wiredtest::walk_auth(session, transport);
    ASSERT_TRUE(session.authenticated());
    ASSERT_TRUE(session.announce_services());
    EXPECT_FALSE(service(99, p::encode(p::AvSinkStart{1, 0}).value()));
    setup_video();
    ASSERT_TRUE(service(99, p::encode(p::AvSinkStart{1, 0}).value()));
    ASSERT_TRUE(service(99, data()));
    EXPECT_EQ(video.payloads.back().size(), 4U);
    expect_ping();
}

TEST_F(WiredAvSink, StopAndCleanupClearFocusOnOwnerThread) {
    setup_video();
    session.request_stop();
    session.tick();
    EXPECT_EQ(session.state(), aa::session::State::stopping);
    expect_no_focus();
    ASSERT_TRUE(session.apply(aa::session::Event::cleanup_finished));
    EXPECT_EQ(session.state(), aa::session::State::disconnected);
    expect_no_focus();
}

TEST_F(WiredAvSink, FailureClearsFocus) {
    setup_video();
    ASSERT_TRUE(session.fail(aa::Error{aa::ErrorCode::transport_io}));
    expect_no_focus();
}

TEST_F(WiredAvSink, LinkLossAndDeadlineFailureClearFocus) {
    ASSERT_TRUE(session.apply(aa::session::Event::negotiation_succeeded));
    setup_video();
    ASSERT_TRUE(session.apply(aa::session::Event::link_lost));
    expect_no_focus();
    clock.advance(aa::core::Milliseconds{5001});
    session.tick();
    EXPECT_EQ(session.state(), aa::session::State::failed);
    expect_no_focus();
}

TEST_F(WiredAvSink, RepliesUseConfiguredDynamicServiceIds) {
    aa::app::WiredSession other{{trust, clock, diagnostics}};
    auto descriptors = wiredtest::golden_services();
    descriptors[0].id = p::ServiceKey{17};
    descriptors[1].id = p::ServiceKey{23};
    descriptors[2].id = p::ServiceKey{41};
    ASSERT_TRUE(other.configure(std::move(descriptors)));
    ASSERT_TRUE(other.register_av_sink(p::ChannelRole::video, video));
    ASSERT_TRUE(other.register_av_sink(p::ChannelRole::media_audio, audio));
    other.attach_transport(&transport);
    auto owned = std::make_unique<wiredtest::FakeHandshakeCipher>();
    auto *other_cipher = owned.get();
    other.attach_cipher(std::move(owned));
    wiredtest::walk_auth(other, transport);
    ASSERT_TRUE(other.authenticated());
    ASSERT_TRUE(other.announce_services());
    for (const std::uint8_t channel : {std::uint8_t{17}, std::uint8_t{23}}) {
        const auto codec = channel == 17 ? p::AvSinkCodec::h264 : p::AvSinkCodec::pcm;
        const auto before = transport.sent_count();
        auto frames = tr::encode_message({channel, tr::MessageKind::specific,
            p::encode(p::AvSinkSetup{codec}).value()}, tr::Encryption::encrypted, other_cipher);
        ASSERT_TRUE(frames);
        for (auto &frame : frames.value()) { transport.push_inbound(std::move(frame)); }
        ASSERT_TRUE(other.poll_transport());
        EXPECT_EQ(transport.sent_count(), before + (channel == 17 ? 2U : 1U));
        reply(channel, channel == 17
            ? p::encode(p::VideoFocusNotification{p::VideoFocusMode::projected, true}).value()
            : p::encode_av_sink_ready().value());
    }
    EXPECT_TRUE(video.payloads.empty());
    EXPECT_TRUE(audio.payloads.empty());
}

TEST(WiredAvRegistration, ConfiguredMatchingAvOnlyAndDuplicateRawOrWrappedRejected) {
    aa::session::test::MapTrustStore trust;
    aa::session::test::ManualClock clock;
    wiredtest::CaptureSink capture;
    aa::diagnostics::Diagnostics diagnostics{capture};
    wiredtest::RecordingSink raw;
    aa::app::WiredSession session{{trust, clock, diagnostics}};
    EXPECT_FALSE(session.register_av_sink(p::ChannelRole::video, raw));
    ASSERT_TRUE(session.configure(wiredtest::golden_services()));
    EXPECT_FALSE(session.register_av_sink(p::ChannelRole::input, raw));
    EXPECT_FALSE(session.register_av_sink(p::ChannelRole::microphone, raw));
    EXPECT_FALSE(session.register_av_sink(p::ChannelRole::guidance_audio, raw));
    ASSERT_TRUE(session.register_av_sink(p::ChannelRole::video, raw));
    EXPECT_FALSE(session.register_av_sink(p::ChannelRole::video, raw));
    EXPECT_FALSE(session.register_sink(p::ChannelRole::video, raw));
    ASSERT_TRUE(session.register_sink(p::ChannelRole::media_audio, raw));
    EXPECT_FALSE(session.register_av_sink(p::ChannelRole::media_audio, raw));
}
} // namespace
