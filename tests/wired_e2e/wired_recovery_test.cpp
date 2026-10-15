// SPDX-License-Identifier: GPL-3.0-or-later
//
// Task 31 wired reconnect and recovery matrix (host-side, no hardware). Every
// scenario from the plan is exercised at the LOGIC level against the task-30
// integrator (aa::app::WiredSession), REBASED onto the lane-30 AAP auth API:
// every link (initial attach and every reconnect) walks the full
// version_response -> encapsulated_ssl -> auth_complete progression on a FRESH
// per-link cipher before any channel traffic is admitted, exactly as the
// composed receiver does (bind_link_cipher). A retired link's cipher and
// counters are dropped with it (reset_link at attach_transport): nothing from
// the old link can authenticate or seal on the new one.
//
//   * USB unplug/replug        -> link_lost + a fresh transport, recovered within
//                                 the fixed 5 s wired reconnect budget (and a
//                                 deterministic fail-closed check past the bound);
//                                 the replugged link re-authenticates from scratch
//                                 (no stale-cipher resumption, no stale-key sealing).
//   * receiver restart         -> a fresh receiver has NO stale active session or
//                                 trust: it always re-admits from the TrustStore.
//   * dummy consumer restart   -> the degrade/recover cycle returns to active and
//                                 the media path keeps routing.
//   * malformed input          -> typed transport_malformed_frame / transport_io,
//                                 never a crash or a delivery.
//   * unknown-phone attempts    -> a synthetic identity absent from the store
//                                 fails closed at discovery AND on reconnect.
//   * phone lock/unlock + AA restart -> the session-side pause/resume/restart
//                                 equivalent (degrade/recover + link_lost/reconnect).
//
// Retry counts and failure outcomes are reported through the task-19 diagnostics
// boundary (EventKind::reconnect); the assertions read the redacted JSON only.

#include "harness.hpp"

#include "../session/Fakes.hpp"

#include <aa/session/Clock.hpp>

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <memory>
#include <span>
#include <vector>

namespace {

namespace app = aa::app;
namespace diag = aa::diagnostics;
namespace proto = aa::protocol;
namespace ss = aa::session;
namespace tr = aa::transport;

using wiredtest::CaptureSink;
using wiredtest::RecordingSink;
using wiredtest::ScriptedTransport;

const aa::trust::PhoneIdentity kApproved{"recovery-approved"};
const aa::trust::PhoneIdentity kUnknown{"recovery-unknown"};

// Drives a fresh session to active over `transport` on the approved identity.
// Attach order per link (lane-30 API): attach_transport() first (caller), then
// attach_cipher() here; then the full AAP auth progression is walked before any
// channel traffic, and one discovery + one channel-open request complete the
// task-30 happy path. Returns the link's negotiated cipher via `cipher`
// (optional; nullable) so callers can seal media with it.
void drive_to_active(app::WiredSession &session, ScriptedTransport &transport,
                     wiredtest::FakeHandshakeCipher **cipher = nullptr) {
    auto owned = std::make_unique<wiredtest::FakeHandshakeCipher>();
    auto *bound = owned.get();
    session.attach_cipher(std::move(owned));
    if (cipher != nullptr) {
        *cipher = bound;
    }
    ASSERT_TRUE(session.apply(ss::Event::start_discovery).has_value());
    ASSERT_TRUE(session.phone_discovered(kApproved).has_value());
    ASSERT_TRUE(session.apply(ss::Event::transport_ready).has_value());
    wiredtest::walk_auth(session, transport);
    ASSERT_TRUE(session.authenticated());
    transport.push_inbound(wiredtest::discovery_request_frame());
    auto discovery = session.poll_transport();
    ASSERT_TRUE(discovery.has_value());
    transport.push_inbound(wiredtest::open_request_frame(99));
    auto opened = session.poll_transport();
    ASSERT_TRUE(opened.has_value());
    ASSERT_TRUE(session.apply(ss::Event::negotiation_succeeded).has_value());
    ASSERT_EQ(session.state(), ss::State::active);
}

// Completes a link-restored recovery on the (already attached) fresh transport:
// reconnect opens it, transport_ready re-admits, then the NEW link must walk a
// FULL handshake on a FRESH per-link cipher (the retired link's cipher is gone -
// reset_link() dropped it with its counters) before negotiation_succeeded ->
// active. This is the production reconnect flow (bind_link_cipher per link).
void complete_recovery(app::WiredSession &session, ScriptedTransport &transport,
                       wiredtest::FakeHandshakeCipher **cipher = nullptr) {
    ASSERT_TRUE(session.apply(ss::Event::reconnect).has_value());
    ASSERT_EQ(session.state(), ss::State::connecting);
    ASSERT_TRUE(session.apply(ss::Event::transport_ready).has_value());
    auto owned = std::make_unique<wiredtest::FakeHandshakeCipher>();
    auto *bound = owned.get();
    session.attach_cipher(std::move(owned));
    if (cipher != nullptr) {
        *cipher = bound;
    }
    wiredtest::walk_auth(session, transport);
    ASSERT_TRUE(session.authenticated());
    transport.push_inbound(wiredtest::open_request_frame(99));
    auto opened = session.poll_transport();
    ASSERT_TRUE(opened.has_value());
    ASSERT_TRUE(session.apply(ss::Event::negotiation_succeeded).has_value());
    ASSERT_EQ(session.state(), ss::State::active);
}

// A transport double whose receive() fails with a typed transport I/O error,
// modelling an unplugged pipe the session observes during the media pump.
class IoFaultTransport final : public tr::Transport {
  public:
    [[nodiscard]] tr::Kind kind() const noexcept override { return tr::Kind::usb_aoa; }

    aa::core::Result<void> open(aa::core::CancellationToken) override {
        open_ = true;
        return {};
    }

    aa::core::Result<void> send(std::span<const std::byte>) override {
        if (!open_) {
            return aa::Error{aa::ErrorCode::transport_io};
        }
        return {};
    }

    aa::core::Result<std::vector<std::byte>> receive() override {
        return aa::Error{aa::ErrorCode::transport_io};
    }

    void close() noexcept override { open_ = false; }

  private:
    bool open_{false};
};

// ---------------------------------------------------------------------------
// 1. USB unplug/replug: link loss then a fresh transport recovers within the
//    fixed 5 s wired reconnect budget; past the bound it fails closed.
// ---------------------------------------------------------------------------

TEST(WiredRecoveryUnplugReplug, FreshTransportRecoversToActiveWithinWiredBudget) {
    ss::test::MapTrustStore trust;
    trust.set(kApproved, aa::trust::Decision::approved);
    ss::test::ManualClock clock;
    CaptureSink capture;
    diag::Diagnostics diagnostics{capture};
    RecordingSink video_sink;

    ScriptedTransport transport;
    ScriptedTransport fresh;
    app::WiredSession session{{trust, clock, diagnostics}, app::WiredSessionConfig{}};
    ASSERT_TRUE(session.configure(wiredtest::golden_services()).has_value());
    ASSERT_TRUE(session.register_sink(proto::ChannelRole::video, video_sink).has_value());
    session.attach_transport(&transport);
    wiredtest::FakeHandshakeCipher *cipher = nullptr;
    drive_to_active(session, transport, &cipher);
    ASSERT_NE(cipher, nullptr);

    clock.set(aa::core::Nanoseconds{1'000'000'000}); // t = 1 s
    const auto lost_at = clock.now();
    ASSERT_TRUE(session.apply(ss::Event::link_lost).has_value());
    ASSERT_EQ(session.state(), ss::State::reconnecting);
    EXPECT_GE(transport.close_calls(), 1); // the retired handle is closed on loss
    EXPECT_FALSE(transport.is_open());

    // The replug supplies a fresh handle and the cycle completes well inside 5 s.
    clock.advance(aa::core::Milliseconds{250});
    session.attach_transport(&fresh);
    EXPECT_FALSE(session.authenticated()); // the retired link's auth did not survive
    EXPECT_EQ(session.version_responses(), 0U);
    EXPECT_EQ(session.encapsulated_ssls(), 0U);
    EXPECT_EQ(session.auth_completes(), 0U);
    EXPECT_EQ(session.channel_opens(), 0U);
    wiredtest::FakeHandshakeCipher *fresh_cipher = nullptr;
    complete_recovery(session, fresh, &fresh_cipher);
    EXPECT_NE(fresh_cipher, nullptr);

    const auto recovered_ms = (clock.now().count - lost_at.count) / 1'000'000;
    EXPECT_LE(recovered_ms, 5'000);
    EXPECT_TRUE(fresh.is_open());
    EXPECT_EQ(session.reconnect_attempts(), 1U);
    EXPECT_EQ(session.recovery_failures(), 0U);

    // Retry/failure telemetry is reported through the redaction boundary.
    EXPECT_TRUE(capture.contains("\"kind\":\"reconnect\""));
    EXPECT_TRUE(capture.contains("\"status\":\"accepted\""));
    EXPECT_TRUE(capture.contains("\"retries\":1"));
    EXPECT_FALSE(capture.contains("recovery-approved"));
}

// 31b proof (gate P1), part 1: the retired link's cipher and counters are
// GONE at the replug (reset_link at attach_transport). Against pre-f955401
// session core this case FAILS: the stale cipher still authenticated and
// sealed on the new link.
TEST(WiredRecoveryUnplugReplug, ReconnectDropsStaleCipherAndLinkCounters) {
    ss::test::MapTrustStore trust;
    trust.set(kApproved, aa::trust::Decision::approved);
    ss::test::ManualClock clock;
    CaptureSink capture;
    diag::Diagnostics diagnostics{capture};
    RecordingSink video_sink;

    ScriptedTransport transport;
    ScriptedTransport fresh;
    app::WiredSession session{{trust, clock, diagnostics}, app::WiredSessionConfig{}};
    ASSERT_TRUE(session.configure(wiredtest::golden_services()).has_value());
    ASSERT_TRUE(session.register_sink(proto::ChannelRole::video, video_sink).has_value());
    session.attach_transport(&transport);
    drive_to_active(session, transport);
    ASSERT_EQ(session.channel_opens(), 1U);

    ASSERT_TRUE(session.apply(ss::Event::link_lost).has_value());
    session.attach_transport(&fresh);
    EXPECT_EQ(session.version_responses(), 0U);
    EXPECT_EQ(session.encapsulated_ssls(), 0U);
    EXPECT_EQ(session.auth_completes(), 0U);
    EXPECT_EQ(session.channel_opens(), 0U);

    ASSERT_TRUE(session.apply(ss::Event::reconnect).has_value());
    ASSERT_TRUE(session.apply(ss::Event::transport_ready).has_value());
    ASSERT_TRUE(fresh.is_open());

    // No cipher is bound to the new link yet: the handshake cannot proceed
    // and nothing can seal - the stale one is gone, not merely inactive.
    fresh.push_inbound(
        wiredtest::control_frame(proto::encode(proto::VersionResponse{1, 6, 0}).value()));
    auto response = session.poll_transport();
    ASSERT_FALSE(response.has_value());
    EXPECT_FALSE(session.authenticated());
    EXPECT_EQ(session.auth_completes(), 0U);
    auto sealed = session.announce_services();
    ASSERT_FALSE(sealed.has_value());
    EXPECT_EQ(sealed.error().code(), aa::ErrorCode::transport_unauthorized_peer);
    EXPECT_EQ(fresh.sent_count(), 0U);
}

// 31b proof (gate P1), part 2: the replugged link needs a FULL handshake on a
// FRESH per-link cipher - VersionResponse{1,6,0} alone must not authenticate,
// and nothing may seal until auth_complete has been sent.
TEST(WiredRecoveryUnplugReplug, ReconnectFreshCipherRequiresFullHandshake) {
    ss::test::MapTrustStore trust;
    trust.set(kApproved, aa::trust::Decision::approved);
    ss::test::ManualClock clock;
    CaptureSink capture;
    diag::Diagnostics diagnostics{capture};
    RecordingSink video_sink;

    ScriptedTransport transport;
    ScriptedTransport fresh;
    app::WiredSession session{{trust, clock, diagnostics}, app::WiredSessionConfig{}};
    ASSERT_TRUE(session.configure(wiredtest::golden_services()).has_value());
    ASSERT_TRUE(session.register_sink(proto::ChannelRole::video, video_sink).has_value());
    session.attach_transport(&transport);
    drive_to_active(session, transport);

    ASSERT_TRUE(session.apply(ss::Event::link_lost).has_value());
    session.attach_transport(&fresh);
    ASSERT_TRUE(session.apply(ss::Event::reconnect).has_value());
    ASSERT_TRUE(session.apply(ss::Event::transport_ready).has_value());

    // Production reconnect flow: bind a FRESH per-link cipher.
    session.attach_cipher(std::make_unique<wiredtest::FakeHandshakeCipher>());

    fresh.push_inbound(
        wiredtest::control_frame(proto::encode(proto::VersionResponse{1, 6, 0}).value()));
    ASSERT_TRUE(session.poll_transport().has_value());
    EXPECT_FALSE(session.authenticated());
    EXPECT_EQ(session.auth_completes(), 0U);
    auto sealed = session.announce_services();
    ASSERT_FALSE(sealed.has_value());
    EXPECT_EQ(sealed.error().code(), aa::ErrorCode::transport_unauthorized_peer);
    ASSERT_EQ(fresh.sent_count(), 1U); // exactly the handshake chunk, nothing sealed
    auto handshake = tr::decode_frame(fresh.last_sent());
    ASSERT_TRUE(handshake.has_value());
    EXPECT_EQ(handshake.value().header.encryption, tr::Encryption::plain);
    EXPECT_TRUE(proto::decode_encapsulated_ssl(handshake.value().payload).has_value());

    fresh.push_inbound(
        wiredtest::control_frame(proto::encode(proto::EncapsulatedSsl{{std::byte{0x01}}}).value()));
    ASSERT_TRUE(session.poll_transport().has_value());
    EXPECT_TRUE(session.authenticated());
    EXPECT_EQ(session.auth_completes(), 1U);
    EXPECT_TRUE(session.announce_services().has_value());
    EXPECT_TRUE(capture.contains("\"kind\":\"reconnect\""));
    EXPECT_TRUE(capture.contains("\"retries\":1"));
}

TEST(WiredRecoveryUnplugReplug, RecoveryPastWiredDeadlineFailsClosedWithTypedTimeout) {
    ss::test::MapTrustStore trust;
    trust.set(kApproved, aa::trust::Decision::approved);
    ss::test::ManualClock clock;
    CaptureSink capture;
    diag::Diagnostics diagnostics{capture};
    RecordingSink video_sink;

    ScriptedTransport transport;
    app::WiredSession session{{trust, clock, diagnostics}, app::WiredSessionConfig{}};
    ASSERT_TRUE(session.configure(wiredtest::golden_services()).has_value());
    ASSERT_TRUE(session.register_sink(proto::ChannelRole::video, video_sink).has_value());
    session.attach_transport(&transport);
    drive_to_active(session, transport);

    clock.set(aa::core::Nanoseconds{0});
    ASSERT_TRUE(session.apply(ss::Event::link_lost).has_value());

    // 4999 ms < budget: the recovery cycle is still legitimately open.
    clock.advance(aa::core::Milliseconds{4'999});
    session.tick();
    EXPECT_EQ(session.state(), ss::State::reconnecting);

    // At exactly the 5 s bound the cycle fails closed with a typed timeout.
    clock.advance(aa::core::Milliseconds{1});
    session.tick();
    EXPECT_EQ(session.state(), ss::State::failed);
    ASSERT_TRUE(session.failure().has_value());
    EXPECT_EQ(session.failure()->code(), aa::ErrorCode::timeout);
}

// 31 review fix (item 1): acceptance is GATED on State::active. A successful
// Event::reconnect only CONNECTS (state connecting); claiming "accepted" there
// would report a recovery that has not happened yet. The accepted record is
// emitted when the recovery actually reaches active.
TEST(WiredRecoveryUnplugReplug, ReconnectEmitsAcceptedOnlyAfterSessionReachesActive) {
    ss::test::MapTrustStore trust;
    trust.set(kApproved, aa::trust::Decision::approved);
    ss::test::ManualClock clock;
    CaptureSink capture;
    diag::Diagnostics diagnostics{capture};
    RecordingSink video_sink;

    ScriptedTransport transport;
    ScriptedTransport fresh;
    app::WiredSession session{{trust, clock, diagnostics}, app::WiredSessionConfig{}};
    ASSERT_TRUE(session.configure(wiredtest::golden_services()).has_value());
    ASSERT_TRUE(session.register_sink(proto::ChannelRole::video, video_sink).has_value());
    session.attach_transport(&transport);
    drive_to_active(session, transport);

    ASSERT_TRUE(session.apply(ss::Event::link_lost).has_value());
    session.attach_transport(&fresh);
    ASSERT_TRUE(session.apply(ss::Event::reconnect).has_value());
    ASSERT_EQ(session.state(), ss::State::connecting);
    // Only connecting so far: no recovery may be accepted yet.
    EXPECT_FALSE(capture.contains("\"status\":\"accepted\""));
    EXPECT_EQ(session.recovery_failures(), 0U);

    ASSERT_TRUE(session.apply(ss::Event::transport_ready).has_value());
    session.attach_cipher(std::make_unique<wiredtest::FakeHandshakeCipher>());
    wiredtest::walk_auth(session, fresh);
    ASSERT_TRUE(session.authenticated());
    fresh.push_inbound(wiredtest::open_request_frame(99));
    ASSERT_TRUE(session.poll_transport().has_value());
    ASSERT_TRUE(session.apply(ss::Event::negotiation_succeeded).has_value());
    EXPECT_EQ(session.state(), ss::State::active);
    EXPECT_TRUE(capture.contains("\"status\":\"accepted\""));
}

// 31 review fix (item 1): WiredSession::tick() used to forward deadline expiry
// without emitting or counting the recovery failure - a recovery killed by the
// 5 s wired deadline left NO terminal failed telemetry. The terminal failed
// record (and the failure count) must appear when the deadline ends the cycle.
TEST(WiredRecoveryUnplugReplug, DeadlineExpiryDuringRecoveryEmitsTerminalFailedTelemetry) {
    ss::test::MapTrustStore trust;
    trust.set(kApproved, aa::trust::Decision::approved);
    ss::test::ManualClock clock;
    CaptureSink capture;
    diag::Diagnostics diagnostics{capture};
    RecordingSink video_sink;

    ScriptedTransport transport;
    ScriptedTransport fresh;
    app::WiredSession session{{trust, clock, diagnostics}, app::WiredSessionConfig{}};
    ASSERT_TRUE(session.configure(wiredtest::golden_services()).has_value());
    ASSERT_TRUE(session.register_sink(proto::ChannelRole::video, video_sink).has_value());
    session.attach_transport(&transport);
    drive_to_active(session, transport);

    clock.set(aa::core::Nanoseconds{0});
    ASSERT_TRUE(session.apply(ss::Event::link_lost).has_value());
    session.attach_transport(&fresh);
    ASSERT_TRUE(session.apply(ss::Event::reconnect).has_value());
    ASSERT_TRUE(session.apply(ss::Event::transport_ready).has_value());
    ASSERT_NE(session.state(), ss::State::active);
    EXPECT_EQ(session.recovery_failures(), 0U);

    // The wired reconnect deadline ends the open recovery with a typed timeout.
    clock.advance(aa::core::Milliseconds{5'000});
    session.tick();
    EXPECT_EQ(session.state(), ss::State::failed);
    ASSERT_TRUE(session.failure().has_value());
    EXPECT_EQ(session.failure()->code(), aa::ErrorCode::timeout);
    EXPECT_EQ(session.recovery_failures(), 1U);
    EXPECT_TRUE(capture.contains("\"kind\":\"reconnect\""));
    EXPECT_TRUE(capture.contains("\"status\":\"failed\""));
    EXPECT_TRUE(capture.contains("\"code\":3"));
}

// 31 review fix (item 1): a negotiation/handshake failure while the recovery is
// open must end it as a TERMINAL failed record (counted), not leave the attempt
// silently unresolved.
TEST(WiredRecoveryUnplugReplug, NegotiationFailureDuringRecoveryEmitsTerminalFailedTelemetry) {
    ss::test::MapTrustStore trust;
    trust.set(kApproved, aa::trust::Decision::approved);
    ss::test::ManualClock clock;
    CaptureSink capture;
    diag::Diagnostics diagnostics{capture};
    RecordingSink video_sink;

    ScriptedTransport transport;
    ScriptedTransport fresh;
    app::WiredSession session{{trust, clock, diagnostics}, app::WiredSessionConfig{}};
    ASSERT_TRUE(session.configure(wiredtest::golden_services()).has_value());
    ASSERT_TRUE(session.register_sink(proto::ChannelRole::video, video_sink).has_value());
    session.attach_transport(&transport);
    drive_to_active(session, transport);

    ASSERT_TRUE(session.apply(ss::Event::link_lost).has_value());
    session.attach_transport(&fresh);
    ASSERT_TRUE(session.apply(ss::Event::reconnect).has_value());
    ASSERT_TRUE(session.apply(ss::Event::transport_ready).has_value());
    ASSERT_NE(session.state(), ss::State::active);

    // The version/negotiation step fails terminally mid-recovery.
    ASSERT_TRUE(session.fail(aa::Error{aa::ErrorCode::protocol_version_mismatch}).has_value());
    EXPECT_EQ(session.state(), ss::State::failed);
    EXPECT_EQ(session.recovery_failures(), 1U);
    EXPECT_TRUE(capture.contains("\"status\":\"failed\""));
    EXPECT_TRUE(capture.contains("\"code\":200"));
    EXPECT_FALSE(capture.contains("\"status\":\"accepted\""));
}

TEST(WiredRecoveryUnplugReplug, CancelMidRecoveryTearsDownCleanly) {
    ss::test::MapTrustStore trust;
    trust.set(kApproved, aa::trust::Decision::approved);
    ss::test::ManualClock clock;
    CaptureSink capture;
    diag::Diagnostics diagnostics{capture};
    RecordingSink video_sink;

    ScriptedTransport transport;
    app::WiredSession session{{trust, clock, diagnostics}, app::WiredSessionConfig{}};
    ASSERT_TRUE(session.configure(wiredtest::golden_services()).has_value());
    ASSERT_TRUE(session.register_sink(proto::ChannelRole::video, video_sink).has_value());
    session.attach_transport(&transport);
    drive_to_active(session, transport);

    // Stop requested WHILE a recovery cycle is open: the teardown is bounded and
    // leaves no dangling handle (the retired transport is closed exactly once),
    // then the next cycle can start on a fresh cancellation token.
    ASSERT_TRUE(session.apply(ss::Event::link_lost).has_value());
    ASSERT_EQ(session.state(), ss::State::reconnecting);
    session.request_stop();
    session.tick();
    EXPECT_EQ(session.state(), ss::State::stopping);
    ASSERT_TRUE(session.apply(ss::Event::cleanup_finished).has_value());
    EXPECT_EQ(session.state(), ss::State::disconnected);
    EXPECT_GE(transport.close_calls(), 1);
    EXPECT_FALSE(transport.is_open());

    // The fresh cycle re-admits from the store (no cached admission survives).
    ASSERT_TRUE(session.apply(ss::Event::start_discovery).has_value());
    EXPECT_EQ(session.state(), ss::State::discovering);
}

// ---------------------------------------------------------------------------
// 2. Receiver restart: a fresh receiver re-admits from the persistent
//    TrustStore; a crash leaves NO stale trust or active session.
// ---------------------------------------------------------------------------

TEST(WiredRecoveryReceiverRestart, CleanRestartReAdmitsFromTrustStore) {
    ss::test::MapTrustStore trust;
    trust.set(kApproved, aa::trust::Decision::approved);
    ss::test::ManualClock clock;
    CaptureSink capture;
    diag::Diagnostics diagnostics{capture};

    // Receiver A: reaches active, then stops cleanly (transport closed).
    {
        RecordingSink sink;
        ScriptedTransport transport;
        app::WiredSession session{{trust, clock, diagnostics}, app::WiredSessionConfig{}};
        ASSERT_TRUE(session.configure(wiredtest::golden_services()).has_value());
        ASSERT_TRUE(session.register_sink(proto::ChannelRole::video, sink).has_value());
        session.attach_transport(&transport);
        drive_to_active(session, transport);

        session.request_stop();
        session.tick();
        ASSERT_EQ(session.state(), ss::State::stopping);
        ASSERT_TRUE(session.apply(ss::Event::cleanup_finished).has_value());
        EXPECT_EQ(session.state(), ss::State::disconnected);
        EXPECT_GE(transport.close_calls(), 1);
    }

    // Receiver B (restart): starts disconnected - no stale active session - and
    // must walk the full admission path again from the store.
    RecordingSink sink2;
    ScriptedTransport transport2;
    app::WiredSession session2{{trust, clock, diagnostics}, app::WiredSessionConfig{}};
    EXPECT_EQ(session2.state(), ss::State::disconnected);
    ASSERT_TRUE(session2.configure(wiredtest::golden_services()).has_value());
    ASSERT_TRUE(session2.register_sink(proto::ChannelRole::video, sink2).has_value());
    session2.attach_transport(&transport2);
    drive_to_active(session2, transport2);
    EXPECT_EQ(session2.state(), ss::State::active);
}

TEST(WiredRecoveryReceiverRestart, CrashLeavesNoStaleTrustOrActiveSession) {
    ss::test::MapTrustStore trust;
    trust.set(kApproved, aa::trust::Decision::approved);
    ss::test::ManualClock clock;
    CaptureSink capture;
    diag::Diagnostics diagnostics{capture};

    // Receiver A: active, then destroyed abruptly (no cleanup -> simulates a
    // crash). Its session state cannot survive process death.
    {
        RecordingSink sink;
        ScriptedTransport transport;
        app::WiredSession session{{trust, clock, diagnostics}, app::WiredSessionConfig{}};
        ASSERT_TRUE(session.configure(wiredtest::golden_services()).has_value());
        ASSERT_TRUE(session.register_sink(proto::ChannelRole::video, sink).has_value());
        session.attach_transport(&transport);
        drive_to_active(session, transport);
    }

    // The phone is revoked between the crash and the restart.
    ASSERT_TRUE(trust.forget(kApproved).has_value());

    // Receiver B: no stale active session, and the revoked identity fails closed
    // because admission is re-read from the store, never cached.
    RecordingSink sink2;
    ScriptedTransport transport2;
    app::WiredSession session2{{trust, clock, diagnostics}, app::WiredSessionConfig{}};
    EXPECT_EQ(session2.state(), ss::State::disconnected);
    ASSERT_TRUE(session2.configure(wiredtest::golden_services()).has_value());
    ASSERT_TRUE(session2.register_sink(proto::ChannelRole::video, sink2).has_value());
    session2.attach_transport(&transport2);
    ASSERT_TRUE(session2.apply(ss::Event::start_discovery).has_value());
    auto discovered = session2.phone_discovered(kApproved);
    ASSERT_FALSE(discovered.has_value());
    EXPECT_EQ(discovered.error().code(), aa::ErrorCode::session_unknown_phone);
    EXPECT_NE(session2.state(), ss::State::active);
    EXPECT_NE(session2.state(), ss::State::connecting);
}

// ---------------------------------------------------------------------------
// 3. Dummy consumer restart: the degrade/recover cycle returns to active and the
//    media path keeps routing; a revoke while degraded fails closed.
// ---------------------------------------------------------------------------

TEST(WiredRecoveryConsumerRestart, DegradeThenRecoverReturnsToActiveAndKeepsRouting) {
    ss::test::MapTrustStore trust;
    trust.set(kApproved, aa::trust::Decision::approved);
    ss::test::ManualClock clock;
    CaptureSink capture;
    diag::Diagnostics diagnostics{capture};
    RecordingSink video_sink;

    ScriptedTransport transport;
    app::WiredSession session{{trust, clock, diagnostics}, app::WiredSessionConfig{}};
    ASSERT_TRUE(session.configure(wiredtest::golden_services()).has_value());
    ASSERT_TRUE(session.register_sink(proto::ChannelRole::video, video_sink).has_value());
    session.attach_transport(&transport);
    wiredtest::FakeHandshakeCipher *cipher = nullptr;
    drive_to_active(session, transport, &cipher);
    ASSERT_NE(cipher, nullptr);

    const std::size_t delivered_before = video_sink.payloads.size();

    // Consumer bounce: the session degrades, then the consumer returns.
    ASSERT_TRUE(session.apply(ss::Event::degrade).has_value());
    EXPECT_EQ(session.state(), ss::State::degraded);
    clock.advance(aa::core::Milliseconds{500}); // inside the consumer grace
    ASSERT_TRUE(session.apply(ss::Event::recover).has_value());
    EXPECT_EQ(session.state(), ss::State::active);
    EXPECT_TRUE(session.authenticated()); // recovery kept this link's negotiated cipher

    // The channel/session path still routes media after recovery, and the
    // inbound record opens with the negotiated cipher (E=1, not plaintext).
    auto inbound = tr::encode_message(
        tr::Message{std::uint8_t{99}, tr::MessageKind::specific, wiredtest::kVideoIdr},
        tr::Encryption::encrypted, cipher);
    ASSERT_TRUE(inbound.has_value());
    for (const auto &frame : inbound.value()) {
        transport.push_inbound(frame);
    }
    auto routed = session.poll_transport();
    ASSERT_TRUE(routed.has_value());
    EXPECT_TRUE(routed.value());
    ASSERT_EQ(video_sink.payloads.size(), delivered_before + 1);
    EXPECT_EQ(video_sink.payloads.at(delivered_before), wiredtest::kVideoIdr);

    EXPECT_EQ(session.reconnect_attempts(), 1U); // the recover ran one admission
    EXPECT_EQ(session.recovery_failures(), 0U);
    EXPECT_TRUE(capture.contains("\"reason\":\"peer disconnected\""));
}

TEST(WiredRecoveryConsumerRestart, RecoverAfterRevokeFailsClosedWithTelemetry) {
    ss::test::MapTrustStore trust;
    trust.set(kApproved, aa::trust::Decision::approved);
    ss::test::ManualClock clock;
    CaptureSink capture;
    diag::Diagnostics diagnostics{capture};
    RecordingSink video_sink;

    ScriptedTransport transport;
    app::WiredSession session{{trust, clock, diagnostics}, app::WiredSessionConfig{}};
    ASSERT_TRUE(session.configure(wiredtest::golden_services()).has_value());
    ASSERT_TRUE(session.register_sink(proto::ChannelRole::video, video_sink).has_value());
    session.attach_transport(&transport);
    drive_to_active(session, transport);

    ASSERT_TRUE(session.apply(ss::Event::degrade).has_value());
    EXPECT_EQ(session.state(), ss::State::degraded);
    ASSERT_TRUE(trust.forget(kApproved).has_value());

    auto recovered = session.apply(ss::Event::recover);
    ASSERT_FALSE(recovered.has_value());
    EXPECT_EQ(recovered.error().code(), aa::ErrorCode::session_unknown_phone);
    EXPECT_EQ(session.state(), ss::State::failed);
    EXPECT_EQ(session.recovery_failures(), 1U);
    EXPECT_EQ(session.reconnect_attempts(), 1U);
    EXPECT_TRUE(capture.contains("\"status\":\"failed\""));
    EXPECT_TRUE(capture.contains("\"code\":302"));
    EXPECT_FALSE(capture.contains("recovery-approved"));
}

TEST(WiredRecoveryConsumerRestart, ConsumerAbsentPastGraceEnterReconnect) {
    ss::test::MapTrustStore trust;
    trust.set(kApproved, aa::trust::Decision::approved);
    ss::test::ManualClock clock;
    CaptureSink capture;
    diag::Diagnostics diagnostics{capture};
    RecordingSink video_sink;

    ScriptedTransport transport;
    app::WiredSession session{{trust, clock, diagnostics}, app::WiredSessionConfig{}};
    ASSERT_TRUE(session.configure(wiredtest::golden_services()).has_value());
    ASSERT_TRUE(session.register_sink(proto::ChannelRole::video, video_sink).has_value());
    session.attach_transport(&transport);
    drive_to_active(session, transport);

    clock.set(aa::core::Nanoseconds{0});
    ASSERT_TRUE(session.apply(ss::Event::degrade).has_value());

    // Before the 10 s consumer grace the session still tolerates the outage.
    clock.advance(aa::core::Milliseconds{9'999});
    session.tick();
    EXPECT_EQ(session.state(), ss::State::degraded);

    // At the grace bound the consumer is treated as gone: fail closed toward a
    // reconnect cycle (bounded work, no hang).
    clock.advance(aa::core::Milliseconds{1});
    session.tick();
    EXPECT_EQ(session.state(), ss::State::reconnecting);
}

// ---------------------------------------------------------------------------
// 4. Malformed input: typed errors, never a crash or a delivery; the session
//    stays recoverable after the stream parser has closed.
// ---------------------------------------------------------------------------

TEST(WiredRecoveryMalformed, ReservedFlagFrameIsTypedAndNeverDeliveredThenRecovers) {
    ss::test::MapTrustStore trust;
    trust.set(kApproved, aa::trust::Decision::approved);
    ss::test::ManualClock clock;
    CaptureSink capture;
    diag::Diagnostics diagnostics{capture};
    RecordingSink video_sink;

    ScriptedTransport transport;
    ScriptedTransport fresh;
    app::WiredSession session{{trust, clock, diagnostics}, app::WiredSessionConfig{}};
    ASSERT_TRUE(session.configure(wiredtest::golden_services()).has_value());
    ASSERT_TRUE(session.register_sink(proto::ChannelRole::video, video_sink).has_value());
    session.attach_transport(&transport);
    drive_to_active(session, transport);

    const std::size_t delivered_before = video_sink.payloads.size();
    // Reserved flag bits -> the parser refuses the frame with a typed error.
    transport.push_inbound(
        std::vector<std::byte>{std::byte{0x00}, std::byte{0x13}, std::byte{0x00}, std::byte{0x00}});
    auto malformed = session.poll_transport();
    ASSERT_FALSE(malformed.has_value());
    EXPECT_EQ(malformed.error().code(), aa::ErrorCode::transport_malformed_frame);
    EXPECT_EQ(video_sink.payloads.size(), delivered_before);

    // The closed stream is dropped with the handle on reconnect: recovery works.
    ASSERT_TRUE(session.apply(ss::Event::link_lost).has_value());
    session.attach_transport(&fresh);
    complete_recovery(session, fresh);
    EXPECT_EQ(session.state(), ss::State::active);
}

TEST(WiredRecoveryMalformed, TransportIoErrorIsTypedAndNeverADelivery) {
    ss::test::MapTrustStore trust;
    trust.set(kApproved, aa::trust::Decision::approved);
    ss::test::ManualClock clock;
    CaptureSink capture;
    diag::Diagnostics diagnostics{capture};
    RecordingSink video_sink;

    ScriptedTransport transport;
    app::WiredSession session{{trust, clock, diagnostics}, app::WiredSessionConfig{}};
    ASSERT_TRUE(session.configure(wiredtest::golden_services()).has_value());
    ASSERT_TRUE(session.register_sink(proto::ChannelRole::video, video_sink).has_value());
    session.attach_transport(&transport);
    drive_to_active(session, transport);

    // An unplugged pipe surfaces as a typed transport I/O error on the pump.
    IoFaultTransport faulty;
    session.attach_transport(&faulty);
    const std::size_t delivered_before = video_sink.payloads.size();
    auto outcome = session.poll_transport();
    ASSERT_FALSE(outcome.has_value());
    EXPECT_EQ(outcome.error().code(), aa::ErrorCode::transport_io);
    EXPECT_EQ(video_sink.payloads.size(), delivered_before);
    EXPECT_EQ(aa::domain_of(outcome.error().code()), aa::ErrorDomain::transport);
}

// ---------------------------------------------------------------------------
// 5. Unknown-phone attempts: a synthetic identity absent from the store fails
//    closed at discovery and on reconnect (telemetry reports the failure).
// ---------------------------------------------------------------------------

TEST(WiredRecoveryUnknownPhone, UnknownIdentityFailsClosedAtDiscovery) {
    ss::test::MapTrustStore trust;
    trust.set(kApproved, aa::trust::Decision::approved);
    ss::test::ManualClock clock;
    CaptureSink capture;
    diag::Diagnostics diagnostics{capture};
    RecordingSink video_sink;

    ScriptedTransport transport;
    app::WiredSession session{{trust, clock, diagnostics}, app::WiredSessionConfig{}};
    ASSERT_TRUE(session.configure(wiredtest::golden_services()).has_value());
    ASSERT_TRUE(session.register_sink(proto::ChannelRole::video, video_sink).has_value());
    session.attach_transport(&transport);
    ASSERT_TRUE(session.apply(ss::Event::start_discovery).has_value());

    auto discovered = session.phone_discovered(kUnknown);
    ASSERT_FALSE(discovered.has_value());
    EXPECT_EQ(discovered.error().code(), aa::ErrorCode::session_unknown_phone);
    EXPECT_EQ(session.state(), ss::State::discovering);

    // A repeated attempt is still refused: discovery never grants trust.
    auto again = session.phone_discovered(kUnknown);
    ASSERT_FALSE(again.has_value());
    EXPECT_EQ(again.error().code(), aa::ErrorCode::session_unknown_phone);
    EXPECT_NE(session.state(), ss::State::connecting);
    EXPECT_NE(session.state(), ss::State::active);
}

TEST(WiredRecoveryUnknownPhone, UnknownOnReconnectFailsClosedWithTelemetry) {
    ss::test::MapTrustStore trust;
    trust.set(kApproved, aa::trust::Decision::approved);
    ss::test::ManualClock clock;
    CaptureSink capture;
    diag::Diagnostics diagnostics{capture};
    RecordingSink video_sink;

    ScriptedTransport transport;
    ScriptedTransport fresh;
    app::WiredSession session{{trust, clock, diagnostics}, app::WiredSessionConfig{}};
    ASSERT_TRUE(session.configure(wiredtest::golden_services()).has_value());
    ASSERT_TRUE(session.register_sink(proto::ChannelRole::video, video_sink).has_value());
    session.attach_transport(&transport);
    drive_to_active(session, transport);

    ASSERT_TRUE(trust.forget(kApproved).has_value());
    ASSERT_TRUE(session.apply(ss::Event::link_lost).has_value());
    session.attach_transport(&fresh);
    auto reconnect = session.apply(ss::Event::reconnect);
    ASSERT_FALSE(reconnect.has_value());
    EXPECT_EQ(reconnect.error().code(), aa::ErrorCode::session_unknown_phone);
    EXPECT_EQ(session.state(), ss::State::failed);
    EXPECT_EQ(session.recovery_failures(), 1U);
    EXPECT_EQ(session.reconnect_attempts(), 1U);
    EXPECT_TRUE(capture.contains("\"status\":\"failed\""));
    EXPECT_TRUE(capture.contains("\"code\":302"));
}

// ---------------------------------------------------------------------------
// 6. Phone lock/unlock + phone-side Android Auto restart (logic equivalent):
//    the session-side pause/resume and link restart both recover to active.
//    Real ADB stimulation is phase B (harness-documented).
// ---------------------------------------------------------------------------

TEST(WiredRecoveryPhoneLockUnlock, LockUnlockAndPhoneSideRestartRecover) {
    ss::test::MapTrustStore trust;
    trust.set(kApproved, aa::trust::Decision::approved);
    ss::test::ManualClock clock;
    CaptureSink capture;
    diag::Diagnostics diagnostics{capture};
    RecordingSink video_sink;

    ScriptedTransport transport;
    ScriptedTransport fresh;
    app::WiredSession session{{trust, clock, diagnostics}, app::WiredSessionConfig{}};
    ASSERT_TRUE(session.configure(wiredtest::golden_services()).has_value());
    ASSERT_TRUE(session.register_sink(proto::ChannelRole::video, video_sink).has_value());
    session.attach_transport(&transport);
    drive_to_active(session, transport);

    // Lock: the phone stops producing -> session degrades (grace-bounded).
    ASSERT_TRUE(session.apply(ss::Event::degrade).has_value());
    EXPECT_EQ(session.state(), ss::State::degraded);

    // Unlock: the phone returns -> recover.
    ASSERT_TRUE(session.apply(ss::Event::recover).has_value());
    EXPECT_EQ(session.state(), ss::State::active);

    // Phone-side Android Auto restart: the link drops and comes back.
    ASSERT_TRUE(session.apply(ss::Event::link_lost).has_value());
    EXPECT_EQ(session.state(), ss::State::reconnecting);
    session.attach_transport(&fresh);
    complete_recovery(session, fresh);
    EXPECT_EQ(session.state(), ss::State::active);

    EXPECT_EQ(session.reconnect_attempts(), 2U); // one recover, one reconnect
    EXPECT_EQ(session.recovery_failures(), 0U);
    EXPECT_TRUE(capture.contains("\"kind\":\"reconnect\""));
    EXPECT_TRUE(capture.contains("\"retries\":2"));
}

// ---------------------------------------------------------------------------
// 7. Real-clock bounded work: the same recovery under a steady clock completes
//    with a generous wall-clock ceiling (no tight timing, no fixed sleeps).
// ---------------------------------------------------------------------------

TEST(WiredRecoveryRealClock, RecoveryCompletesBoundedWorkOnSteadyClock) {
    ss::test::MapTrustStore trust;
    trust.set(kApproved, aa::trust::Decision::approved);
    ss::SteadyClock clock;
    CaptureSink capture;
    diag::Diagnostics diagnostics{capture};
    RecordingSink video_sink;

    ScriptedTransport transport;
    ScriptedTransport fresh;
    app::WiredSession session{{trust, clock, diagnostics}, app::WiredSessionConfig{}};
    ASSERT_TRUE(session.configure(wiredtest::golden_services()).has_value());
    ASSERT_TRUE(session.register_sink(proto::ChannelRole::video, video_sink).has_value());
    session.attach_transport(&transport);
    drive_to_active(session, transport);

    const auto start = std::chrono::steady_clock::now();
    ASSERT_TRUE(session.apply(ss::Event::link_lost).has_value());
    session.attach_transport(&fresh);
    complete_recovery(session, fresh);
    const auto elapsed = std::chrono::steady_clock::now() - start;

    EXPECT_EQ(session.state(), ss::State::active);
    EXPECT_LT(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count(), 5'000);
}

} // namespace
