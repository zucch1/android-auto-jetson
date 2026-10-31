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
    std::vector<aa::channels::AvDeliveryId> deliveries;
    std::vector<std::pair<p::ChannelRole, std::uint64_t>> invalidations;
    aa::core::Result<void> submission;

    void SetUp() override {
        WiredControl::SetUp();
        ASSERT_TRUE(session.register_av_sink(p::ChannelRole::video, video));
        ASSERT_TRUE(session.register_av_sink(p::ChannelRole::media_audio, audio));
        ASSERT_TRUE(session.announce_services());
    }

    aa::core::Result<bool> service(std::uint8_t channel, Bytes payload) {
        return service_on(session, channel, std::move(payload));
    }

    aa::core::Result<bool> service_on(aa::app::WiredSession& owner, std::uint8_t channel, Bytes payload) {
        auto frames = tr::encode_message({channel, tr::MessageKind::specific, std::move(payload)},
                                         tr::Encryption::encrypted, cipher);
        if (!frames) { return frames.error(); }
        for (auto &frame : frames.value()) { transport.push_inbound(std::move(frame)); }
        return owner.poll_transport();
    }

    void initialize_deferred(aa::app::WiredSession& owner, std::uint8_t audio_channel = 100) {
        auto descriptors = wiredtest::golden_services();
        descriptors[1].id = p::ServiceKey{audio_channel};
        ASSERT_TRUE(owner.configure(std::move(descriptors)));
        ASSERT_TRUE(owner.register_av_sink(p::ChannelRole::video, video));
        ASSERT_TRUE(owner.register_deferred_av_sink(p::ChannelRole::media_audio, {
            [this](const p::MessageView& message, aa::channels::AvDeliveryId id) -> aa::core::Result<void> {
                deliveries.push_back(id);
                if (auto copied = audio.on_message(message); !copied) { return copied.error(); }
                return submission;
            },
            [this](p::ChannelRole role, std::uint64_t generation) {
                invalidations.emplace_back(role, generation);
            }}));
        owner.attach_transport(&transport);
        auto owned = std::make_unique<wiredtest::FakeHandshakeCipher>();
        cipher = owned.get();
        owner.attach_cipher(std::move(owned));
        wiredtest::walk_auth(owner, transport);
        ASSERT_TRUE(owner.authenticated());
        ASSERT_TRUE(owner.announce_services());
    }

    void start_deferred(aa::app::WiredSession& owner, std::int32_t id = -7,
                        std::uint8_t channel = 100) {
        ASSERT_TRUE(service_on(owner, channel, p::encode(p::AvSinkSetup{}).value()));
        ASSERT_TRUE(service_on(owner, channel, p::encode(p::AvSinkStart{id, 0}).value()));
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

TEST_F(WiredAvSink, FailureEmitsTypedErrorCode) {
    setup_video();
    EXPECT_FALSE(capture.contains("\"error_code\":"));
    ASSERT_TRUE(session.fail(aa::Error{aa::ErrorCode::transport_io}));
    EXPECT_TRUE(capture.contains("\"to_state\":\"failed\""));
    EXPECT_TRUE(capture.contains("\"error_code\":" +
                                 std::to_string(static_cast<std::int64_t>(
                                     aa::ErrorCode::transport_io))));
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

TEST_F(WiredAvSink, DeferredAudioAckWaitsForOwnerCompletionWhileVideoStillDispatches) {
    aa::app::WiredSession owner{{trust, clock, diagnostics}};
    initialize_deferred(owner, 23);
    start_deferred(owner, -19, 23);
    const auto before = transport.sent_count();
    clock.set(aa::core::Nanoseconds{123456});
    ASSERT_TRUE(service_on(owner, 23, data()));
    ASSERT_EQ(deliveries.size(), 1u);
    EXPECT_EQ(transport.sent_count(), before);
    EXPECT_EQ(audio.payloads.back(), (Bytes{std::byte{0x10}, std::byte{0x20}, std::byte{0x30}, std::byte{0x40}}));
    EXPECT_EQ(audio.headers.back().channel, p::ChannelRole::media_audio);
    EXPECT_EQ(audio.headers.back().sequence, 3u);
    EXPECT_EQ(audio.headers.back().timestamp, aa::core::Nanoseconds{123456});
    EXPECT_EQ(deliveries.back().session_id, -19);
    ASSERT_TRUE(service_on(owner, 99, p::encode(p::AvSinkSetup{p::AvSinkCodec::h264}).value()));
    ASSERT_TRUE(service_on(owner, 99, p::encode(p::AvSinkStart{42, 0}).value()));
    ASSERT_TRUE(service_on(owner, 99, data()));
    EXPECT_EQ(video.payloads.size(), 1u);
    reply(99, p::encode_av_sink_ack(42).value());
    const auto video_sent = transport.sent_count();
    ASSERT_TRUE(owner.complete_av_delivery(deliveries.back(), {}));
    EXPECT_EQ(transport.sent_count(), video_sent + 1);
    reply(23, p::encode_av_sink_ack(-19).value());
    ASSERT_TRUE(owner.complete_av_delivery(deliveries.back(), {}));
    EXPECT_EQ(transport.sent_count(), video_sent + 1);
}

TEST_F(WiredAvSink, DeferredSubmitAndRenderErrorsAreTypedAndNeverAcked) {
    aa::app::WiredSession owner{{trust, clock, diagnostics}};
    initialize_deferred(owner);
    start_deferred(owner);
    const auto before = transport.sent_count();
    const aa::Error failure{aa::ErrorCode::audio_sink_unavailable};
    submission = failure;
    const auto rejected = service_on(owner, 100, data());
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error(), failure);
    const auto rejected_id = deliveries.back();
    submission = {};
    ASSERT_TRUE(service_on(owner, 100, data()));
    ASSERT_TRUE(owner.complete_av_delivery(rejected_id, {}));
    const auto failed = owner.complete_av_delivery(deliveries.back(), failure);
    ASSERT_FALSE(failed);
    EXPECT_EQ(failed.error(), failure);
    EXPECT_EQ(transport.sent_count(), before);
    ASSERT_TRUE(owner.complete_av_delivery(deliveries.back(), {}));
    EXPECT_EQ(transport.sent_count(), before);
    ASSERT_TRUE(service_on(owner, 100, data()));
    ASSERT_TRUE(owner.complete_av_delivery(deliveries.back(), {}));
    EXPECT_EQ(transport.sent_count(), before + 1);
    reply(100, p::encode_av_sink_ack(-7).value());
}

TEST_F(WiredAvSink, DeferredWireAckFailureConsumesPendingWithoutRetry) {
    aa::app::WiredSession owner{{trust, clock, diagnostics}};
    initialize_deferred(owner);
    start_deferred(owner);
    ASSERT_TRUE(service_on(owner, 100, data()));
    const auto id = deliveries.back();
    const auto before = transport.sent_count();
    transport.fail_send_at(before);
    const auto failed = owner.complete_av_delivery(id, {});
    ASSERT_FALSE(failed);
    EXPECT_EQ(failed.error(), aa::Error{aa::ErrorCode::transport_io});
    transport.fail_send_at(std::nullopt);
    ASSERT_TRUE(owner.complete_av_delivery(id, {}));
    ASSERT_TRUE(service_on(owner, 100, p::encode(p::AvSinkStart{-7, 0}).value()));
    EXPECT_EQ(transport.sent_count(), before);
    ASSERT_TRUE(service_on(owner, 100, data()));
    ASSERT_TRUE(owner.complete_av_delivery(deliveries.back(), {}));
    EXPECT_EQ(transport.sent_count(), before + 1);
    reply(100, p::encode_av_sink_ack(-7).value());
}

TEST_F(WiredAvSink, DeferredWirePendingCapacityBlocksDataAndCodecCreditsButNotSameStart) {
    aa::app::WiredSession owner{{trust, clock, diagnostics}};
    initialize_deferred(owner);
    ASSERT_TRUE(service_on(owner, 100, p::encode(p::AvSinkSetup{}).value()));
    const auto ready_sent = transport.sent_count();
    ASSERT_TRUE(service_on(owner, 100, {std::byte{0}, std::byte{1}}));
    EXPECT_EQ(transport.sent_count(), ready_sent);
    ASSERT_TRUE(service_on(owner, 100, p::encode(p::AvSinkStart{-7, 0}).value()));
    EXPECT_EQ(transport.sent_count(), ready_sent + 1);
    reply(100, p::encode_av_sink_ack(-7).value());
    ASSERT_TRUE(service_on(owner, 100, data()));
    const auto id = deliveries.back();
    const auto count = invalidations.size();
    for (const auto& payload : {data(), Bytes{std::byte{0}, std::byte{1}}}) {
        const auto rejected = service_on(owner, 100, payload);
        ASSERT_FALSE(rejected);
        EXPECT_EQ(rejected.error(), aa::Error{aa::ErrorCode::ipc_queue_full});
        const auto snapshot = owner.last_queue_rejection();
        ASSERT_TRUE(snapshot);
        EXPECT_EQ(snapshot->source, aa::diagnostics::QueueSource::av_pending);
        EXPECT_EQ(snapshot->reason, payload.size() == 2
            ? aa::diagnostics::QueueReason::codec_config : aa::diagnostics::QueueReason::data);
        EXPECT_EQ(snapshot->role, p::ChannelRole::media_audio);
        EXPECT_EQ(snapshot->requested_bytes, payload.size());
    }
    ASSERT_TRUE(service_on(owner, 100, p::encode(p::AvSinkStart{-7, 0}).value()));
    EXPECT_FALSE(owner.last_queue_rejection());
    EXPECT_EQ(invalidations.size(), count);
    EXPECT_EQ(deliveries.size(), 1u);
    EXPECT_EQ(transport.sent_count(), ready_sent + 1);
    ASSERT_TRUE(owner.complete_av_delivery(id, {}));
    EXPECT_EQ(transport.sent_count(), ready_sent + 2);
    ASSERT_TRUE(service_on(owner, 100, {std::byte{0}, std::byte{1}}));
    EXPECT_EQ(transport.sent_count(), ready_sent + 3);
    submission = aa::Error{aa::ErrorCode::ipc_queue_full};
    const auto downstream = service_on(owner, 100, data());
    ASSERT_FALSE(downstream);
    EXPECT_EQ(downstream.error().code(), aa::ErrorCode::ipc_queue_full);
    EXPECT_FALSE(owner.last_queue_rejection());
}

TEST_F(WiredAvSink, DeferredStopChangedStartSetupCloseAndRediscoveryInvalidateWireCredits) {
    aa::app::WiredSession owner{{trust, clock, diagnostics}};
    initialize_deferred(owner);
    start_deferred(owner);
    for (int operation = 0; operation < 6; ++operation) {
        SCOPED_TRACE(operation);
        ASSERT_TRUE(service_on(owner, 100, data()));
        const auto old = deliveries.back();
        if (operation == 0) {
            ASSERT_TRUE(service_on(owner, 100, p::encode(p::AvSinkStop{}).value()));
            ASSERT_TRUE(service_on(owner, 100, p::encode(p::AvSinkStart{-7, 0}).value()));
        } else if (operation == 1) {
            ASSERT_TRUE(service_on(owner, 100, p::encode(p::AvSinkStart{18, 0}).value()));
        } else {
            if (operation == 3) { ASSERT_TRUE(service_on(owner, 100, {std::byte{0}, std::byte{9}})); }
            if (operation == 4) { ASSERT_TRUE(owner.announce_services()); }
            if (operation == 5) { ASSERT_TRUE(service_on(owner, 0, {std::byte{0}, std::byte{5}})); }
            if (operation != 2) {
                EXPECT_FALSE(service_on(owner, 100, p::encode(p::AvSinkStart{-7, 0}).value()));
            }
            start_deferred(owner);
        }
        EXPECT_GT(invalidations.back().second, old.generation);
        const auto reset_sent = transport.sent_count();
        ASSERT_TRUE(owner.complete_av_delivery(old, {}));
        EXPECT_EQ(transport.sent_count(), reset_sent);
        ASSERT_TRUE(service_on(owner, 100, data()));
        const auto current = deliveries.back();
        EXPECT_GT(current.generation, old.generation);
        ASSERT_TRUE(owner.complete_av_delivery(old, aa::Error{aa::ErrorCode::audio_decode_failed}));
        EXPECT_EQ(transport.sent_count(), reset_sent);
        ASSERT_TRUE(owner.complete_av_delivery(current, {}));
        EXPECT_EQ(transport.sent_count(), reset_sent + 1);
        reply(100, p::encode_av_sink_ack(current.session_id).value());
    }
}

TEST_F(WiredAvSink, DeferredLinkReplacementCannotAckBeforeOrAfterFreshAuthentication) {
    aa::app::WiredSession owner{{trust, clock, diagnostics}};
    initialize_deferred(owner);
    start_deferred(owner);
    ASSERT_TRUE(service_on(owner, 100, data()));
    const auto old = deliveries.back();
    owner.attach_transport(&transport);
    EXPECT_FALSE(owner.authenticated());
    const auto before = transport.sent_count();
    ASSERT_TRUE(owner.complete_av_delivery(old, {}));
    EXPECT_EQ(transport.sent_count(), before);
    auto fresh = std::make_unique<wiredtest::FakeHandshakeCipher>();
    cipher = fresh.get();
    owner.attach_cipher(std::move(fresh));
    ASSERT_TRUE(owner.complete_av_delivery(old, {}));
    EXPECT_EQ(transport.sent_count(), before);
    wiredtest::walk_auth(owner, transport);
    ASSERT_TRUE(owner.authenticated());
    ASSERT_TRUE(owner.announce_services());
    start_deferred(owner);
    ASSERT_TRUE(service_on(owner, 100, data()));
    const auto current = deliveries.back();
    EXPECT_EQ(current.session_id, old.session_id);
    EXPECT_GT(current.generation, old.generation);
    EXPECT_EQ(audio.headers.back().sequence, 3u);
    const auto authenticated_sent = transport.sent_count();
    ASSERT_TRUE(owner.complete_av_delivery(old, {}));
    EXPECT_EQ(transport.sent_count(), authenticated_sent);
    ASSERT_TRUE(owner.complete_av_delivery(current, {}));
    EXPECT_EQ(transport.sent_count(), authenticated_sent + 1);
    reply(100, p::encode_av_sink_ack(-7).value());
}

TEST_F(WiredAvSink, DeferredDuplicateAuthControlsPreservePendingAndCipherReplacementInvalidatesIt) {
    aa::app::WiredSession owner{{trust, clock, diagnostics}};
    initialize_deferred(owner);
    start_deferred(owner);
    ASSERT_TRUE(service_on(owner, 100, data()));
    const auto id = deliveries.back();
    const auto before = transport.sent_count();
    const auto count = invalidations.size();
    ASSERT_TRUE(service_on(owner, 0, p::encode(p::EncapsulatedSsl{{std::byte{1}}}).value()));
    const auto duplicate = service_on(owner, 0, p::encode(p::AuthComplete{0}).value());
    ASSERT_FALSE(duplicate);
    EXPECT_EQ(duplicate.error().code(), aa::ErrorCode::transport_unauthorized_peer);
    EXPECT_EQ(invalidations.size(), count);
    EXPECT_EQ(transport.sent_count(), before);
    ASSERT_TRUE(owner.complete_av_delivery(id, {}));
    EXPECT_EQ(transport.sent_count(), before + 1);
    ASSERT_TRUE(service_on(owner, 100, data()));
    const auto old = deliveries.back();
    auto fresh = std::make_unique<wiredtest::FakeHandshakeCipher>(1);
    ASSERT_TRUE(fresh->step_handshake());
    cipher = fresh.get();
    owner.attach_cipher(std::move(fresh));
    EXPECT_GT(invalidations.back().second, old.generation);
    const auto replaced_sent = transport.sent_count();
    ASSERT_TRUE(owner.complete_av_delivery(old, {}));
    EXPECT_EQ(transport.sent_count(), replaced_sent);
    start_deferred(owner);
    ASSERT_TRUE(service_on(owner, 100, data()));
    ASSERT_TRUE(owner.complete_av_delivery(old, {}));
    EXPECT_EQ(transport.sent_count(), replaced_sent + 1);
    ASSERT_TRUE(owner.complete_av_delivery(deliveries.back(), {}));
    EXPECT_EQ(transport.sent_count(), replaced_sent + 2);
    reply(100, p::encode_av_sink_ack(-7).value());
}

TEST_F(WiredAvSink, DeferredLifecycleReconnectFailureAndStoppingDropPendingCompletions) {
    for (int operation = 0; operation < 3; ++operation) {
        SCOPED_TRACE(operation);
        aa::app::WiredSession owner{{trust, clock, diagnostics}};
        initialize_deferred(owner);
        const aa::trust::PhoneIdentity phone{"control-test"};
        ASSERT_TRUE(owner.apply(aa::session::Event::start_discovery));
        ASSERT_TRUE(owner.phone_discovered(phone));
        ASSERT_TRUE(owner.apply(aa::session::Event::transport_ready));
        ASSERT_TRUE(owner.apply(aa::session::Event::negotiation_succeeded));
        start_deferred(owner);
        ASSERT_TRUE(service_on(owner, 100, data()));
        const auto id = deliveries.back();
        if (operation == 0) { ASSERT_TRUE(owner.apply(aa::session::Event::link_lost)); }
        if (operation == 1) { ASSERT_TRUE(owner.fail(aa::Error{aa::ErrorCode::transport_io})); }
        if (operation == 2) {
            owner.request_stop();
            owner.tick();
        }
        EXPECT_GT(invalidations.back().second, id.generation);
        const auto before = transport.sent_count();
        ASSERT_TRUE(owner.complete_av_delivery(id, {}));
        ASSERT_TRUE(owner.complete_av_delivery(id, aa::Error{aa::ErrorCode::audio_decode_failed}));
        EXPECT_EQ(transport.sent_count(), before);
        if (operation == 2) {
            ASSERT_TRUE(owner.apply(aa::session::Event::cleanup_finished));
            ASSERT_TRUE(owner.complete_av_delivery(id, {}));
            EXPECT_EQ(transport.sent_count(), before);
        }
    }
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

TEST(WiredAvRegistration, DeferredRequiresConfiguredAvSubmitAndExclusiveRegistration) {
    aa::session::test::MapTrustStore trust;
    aa::session::test::ManualClock clock;
    wiredtest::CaptureSink capture;
    aa::diagnostics::Diagnostics diagnostics{capture};
    wiredtest::RecordingSink raw;
    aa::app::WiredSession session{{trust, clock, diagnostics}};
    const aa::channels::DeferredAvDelivery delivery{
        [](const p::MessageView&, aa::channels::AvDeliveryId) -> aa::core::Result<void> { return {}; }, {}};
    EXPECT_FALSE(session.register_deferred_av_sink(p::ChannelRole::media_audio, delivery));
    ASSERT_TRUE(session.configure(wiredtest::golden_services()));
    EXPECT_FALSE(session.register_deferred_av_sink(p::ChannelRole::input, delivery));
    EXPECT_FALSE(session.register_deferred_av_sink(p::ChannelRole::microphone, delivery));
    EXPECT_FALSE(session.register_deferred_av_sink(p::ChannelRole::guidance_audio, delivery));
    EXPECT_FALSE(session.register_deferred_av_sink(p::ChannelRole::media_audio, {}));
    ASSERT_TRUE(session.register_deferred_av_sink(p::ChannelRole::media_audio, delivery));
    EXPECT_FALSE(session.register_deferred_av_sink(p::ChannelRole::media_audio, delivery));
    EXPECT_FALSE(session.register_av_sink(p::ChannelRole::media_audio, raw));
    EXPECT_FALSE(session.register_sink(p::ChannelRole::media_audio, raw));
    ASSERT_TRUE(session.register_av_sink(p::ChannelRole::video, raw));
    EXPECT_FALSE(session.register_deferred_av_sink(p::ChannelRole::video, delivery));
    ASSERT_TRUE(session.complete_av_delivery({p::ChannelRole::system_audio, 1, 1, -7}, {}));
}
} // namespace
