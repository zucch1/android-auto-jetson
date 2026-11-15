// SPDX-License-Identifier: GPL-3.0-or-later
#include "control_io_fixture.hpp"
#include "phone.hpp"
#include "../../tools/wired-receiver/VideoOutlet.hpp"
#include <aa/protocol/AvSink.hpp>
#include <algorithm>
#include <future>
#include <mutex>
#include <optional>
#include <vector>

aa::core::Result<std::vector<std::vector<std::byte>>> real_encode_message(
    const aa::transport::Message& message, aa::transport::Encryption encryption,
    aa::transport::PayloadCipher* cipher)
    asm("__real__ZN2aa9transport14encode_messageERKNS0_7MessageENS0_10EncryptionEPNS0_13PayloadCipherE");
aa::core::Result<std::vector<std::vector<std::byte>>> wrap_encode_message(
    const aa::transport::Message& message, aa::transport::Encryption encryption,
    aa::transport::PayloadCipher* cipher)
    asm("__wrap__ZN2aa9transport14encode_messageERKNS0_7MessageENS0_10EncryptionEPNS0_13PayloadCipherE");

namespace {
struct VideoFocusSendFaultState {
    std::mutex mutex;
    bool armed{false};
    std::optional<int> fail_focus_send_index{};
    int focus_sends{0};
    int native_sends{0};
    std::vector<std::byte> native_unsolicited;
    std::vector<std::byte> projected_unsolicited;
    std::vector<std::byte> projected_reply;
    std::vector<std::byte> no_input_unsolicited;
    std::vector<std::byte> no_input_reply;
};

VideoFocusSendFaultState& video_focus_fault_state() {
    static VideoFocusSendFaultState state;
    return state;
}

struct ScopedVideoFocusFault {
    ScopedVideoFocusFault() {
        auto& state = video_focus_fault_state();
        std::lock_guard lock(state.mutex);
        state.native_unsolicited = aa::protocol::encode(
            aa::protocol::VideoFocusNotification{aa::protocol::VideoFocusMode::native, true}).value();
        state.projected_unsolicited = aa::protocol::encode(
            aa::protocol::VideoFocusNotification{aa::protocol::VideoFocusMode::projected, true}).value();
        state.projected_reply = aa::protocol::encode(
            aa::protocol::VideoFocusNotification{aa::protocol::VideoFocusMode::projected, false}).value();
        state.no_input_unsolicited = aa::protocol::encode(
            aa::protocol::VideoFocusNotification{aa::protocol::VideoFocusMode::projected_no_input_focus, true}).value();
        state.no_input_reply = aa::protocol::encode(
            aa::protocol::VideoFocusNotification{aa::protocol::VideoFocusMode::projected_no_input_focus, false}).value();
        state.focus_sends = 0;
        state.native_sends = 0;
        state.fail_focus_send_index.reset();
        state.armed = true;
    }
    ~ScopedVideoFocusFault() {
        auto& state = video_focus_fault_state();
        std::lock_guard lock(state.mutex);
        state.armed = false;
        state.fail_focus_send_index.reset();
    }
    void fail_at(std::optional<int> one_based_focus_send_index) {
        auto& state = video_focus_fault_state();
        std::lock_guard lock(state.mutex);
        state.fail_focus_send_index = one_based_focus_send_index;
    }
    [[nodiscard]] int focus_sends() {
        auto& state = video_focus_fault_state();
        std::lock_guard lock(state.mutex);
        return state.focus_sends;
    }
    [[nodiscard]] int native_sends() {
        auto& state = video_focus_fault_state();
        std::lock_guard lock(state.mutex);
        return state.native_sends;
    }
};
} // namespace

aa::core::Result<std::vector<std::vector<std::byte>>> wrap_encode_message(
    const aa::transport::Message& message, aa::transport::Encryption encryption,
    aa::transport::PayloadCipher* cipher) {
    {
        auto& state = video_focus_fault_state();
        std::lock_guard lock(state.mutex);
        if (state.armed && message.channel == 99 &&
            message.kind == aa::transport::MessageKind::specific) {
            const bool is_native = (message.payload == state.native_unsolicited);
            const bool is_focus = is_native ||
                message.payload == state.projected_unsolicited ||
                message.payload == state.projected_reply ||
                message.payload == state.no_input_unsolicited ||
                message.payload == state.no_input_reply;
            if (is_focus) {
                ++state.focus_sends;
                if (state.fail_focus_send_index.has_value() &&
                    state.focus_sends == *state.fail_focus_send_index) {
                    return aa::Error{aa::ErrorCode::transport_io};
                }
                if (is_native) {
                    ++state.native_sends;
                }
            }
        }
    }
    return real_encode_message(message, encryption, cipher);
}

namespace wireless_test {
struct ActiveCase { bool backpressure; ControlOperation operation; bool independent_loss{}; };
class WirelessControlActive : public testing::TestWithParam<ActiveCase> {};
TEST_P(WirelessControlActive, ActualControlRetiresActiveIo) {
    ControlIoFixture f; ASSERT_NO_FATAL_FAILURE(f.start());
    const auto scenario = GetParam();
    auto tcp = f.f.connect(); ASSERT_TRUE(tcp);
    tcp.value()->set_io_deadline(ControlIoFixture::Clock::now() + 5s);
    std::atomic<bool> active{}, finished{};
    auto phone = std::async(std::launch::async, [&] {
        walk_phone(*tcp.value(), [&] { return active.load(); }); finished = true;
    });
    bool input_sent{}, writing{};
    unsigned blocked_checkpoints{};
    bool grace_at_nine{}, invalidated{};
    bool precommit_preserved{};
    ControlIoFixture::Clock::time_point invalidated_at{};
    auto send_started = ControlIoFixture::Clock::now();
    const auto work = [&] {
        if (!f.dispatched && finished) {
            if (!scenario.backpressure || (writing && ControlIoFixture::Clock::now() - send_started >= 75ms)) {
                if (scenario.operation == ControlOperation::forget && !precommit_preserved) {
                    aa::trust::test::faults::ScopedFault fault{aa::trust::test::faults::Fault::temp_fsync};
                    const auto result = f.control.forget_phone(f.caller, f.phone);
                    EXPECT_FALSE(result); EXPECT_EQ(f.store.phones().size(), 1U);
                    EXPECT_TRUE(f.coordinator.valid_session()); EXPECT_TRUE(f.coordinator.admitting());
                    EXPECT_EQ(f.helper.revocations, 0U);
                    EXPECT_EQ(f.link.session()->state(), aa::session::State::active);
                    precommit_preserved = true;
                    return;
                }
                f.dispatch(scenario.operation);
            }
            if (writing) ++blocked_checkpoints;
        }
        f.checkpoint();
        if (f.dispatched && scenario.operation == ControlOperation::owner_loss) {
            EXPECT_FALSE(f.coordinator.admitting()); EXPECT_FALSE(f.link.listening());
            const auto elapsed = ControlIoFixture::Clock::now() - f.lost;
            if (scenario.independent_loss && !invalidated && elapsed >= 200ms) {
                EXPECT_TRUE(f.coordinator.valid_session());
                invalidated_at = ControlIoFixture::Clock::now();
                f.f.helper_live = false; invalidated = true;
            }
            if (!scenario.independent_loss && elapsed >= 9s && elapsed < 9500ms) {
                EXPECT_TRUE(f.coordinator.valid_session()); grace_at_nine = true;
            }
        }
    };
    const auto until = ControlIoFixture::Clock::now() + 5s;
    while (!finished && ControlIoFixture::Clock::now() < until) {
        const auto result = f.link.poll(f.coordinator, work);
        if (!result) break;
        if (f.link.session() && f.link.session()->state() == aa::session::State::active) {
            active = true;
            if (!f.f.audio.messages.empty() && !input_sent) {
                ASSERT_TRUE(f.link.session()->submit_input(aa::ipc::InputEvent{aa::ipc::KeyEvent{66, true}}));
                input_sent = true;
                break;
            }
        }
    }
    phone.get(); ASSERT_TRUE(finished); ASSERT_TRUE(active); ASSERT_TRUE(input_sent);
    if (scenario.backpressure) {
        ASSERT_NE(f.link.session(), nullptr);
        std::size_t sent{};
        const auto fill_until = ControlIoFixture::Clock::now() + 4s;
        while (!f.dispatched && ControlIoFixture::Clock::now() < fill_until) {
            writing = true; blocked_checkpoints = 0; send_started = ControlIoFixture::Clock::now();
            const auto result = f.link.session()->send_channel_payload(aa::protocol::ChannelRole::input,
                std::vector<std::byte>(16'000, std::byte{0x55}));
            const auto send_returned = ControlIoFixture::Clock::now();
            if (f.dispatched) {
                RecordProperty("final_send_ok", result ? "true" : "false");
                RecordProperty("final_send_after_control_ms", std::to_string(
                    std::chrono::duration_cast<std::chrono::milliseconds>(send_returned - f.lost).count()));
                if (scenario.operation == ControlOperation::owner_loss) { EXPECT_TRUE(result); }
                std::cout << "final_send_ok=" << result.has_value()
                          << " final_send_error=" << (result ? 0 : static_cast<int>(result.error().code()))
                          << " final_send_duration_ms="
                          << std::chrono::duration_cast<std::chrono::milliseconds>(send_returned - send_started).count()
                          << " final_send_after_control_ms="
                          << std::chrono::duration_cast<std::chrono::milliseconds>(send_returned - f.lost).count()
                          << " session_present_before_poll=" << (f.link.session() != nullptr) << std::endl;
            }
            writing = false;
            if (!result) break;
            sent += 16'000;
        }
        EXPECT_GT(sent, 64'000U); EXPECT_GE(blocked_checkpoints, 3U);
        EXPECT_TRUE(f.dispatched);
        std::cout << "backpressure_bytes=" << sent << " wait_checkpoints=" << blocked_checkpoints << '\n';
    }
    if (f.link.session()) (void)f.link.poll(f.coordinator, work);
    const bool grace = scenario.operation == ControlOperation::owner_loss && !scenario.independent_loss;
    if (grace) { EXPECT_TRUE(grace_at_nine); }
    if (scenario.independent_loss) {
        ASSERT_TRUE(invalidated);
        EXPECT_LT(ControlIoFixture::Clock::now() - invalidated_at, 5s);
        EXPECT_FALSE(f.link.listening()); EXPECT_FALSE(f.coordinator.admitting());
    }
    f.retired(grace);
    if (scenario.operation == ControlOperation::forget) { EXPECT_TRUE(precommit_preserved); }
    // Drain queued complete frames; failure must be peer closure, not a deadline.
    tcp.value()->set_io_deadline(ControlIoFixture::Clock::now() + 1s);
    bool closed{};
    for (unsigned frame = 0; frame < 4096; ++frame) {
        const auto result = tcp.value()->receive();
        if (!result) {
            EXPECT_NE(result.error().code(), aa::ErrorCode::timeout);
            closed = true; break;
        }
    }
    EXPECT_TRUE(closed);
}
INSTANTIATE_TEST_SUITE_P(States, WirelessControlActive, testing::Values(
    ActiveCase{false, ControlOperation::forget},
    ActiveCase{true, ControlOperation::forget},
    ActiveCase{true, ControlOperation::owner_loss},
    ActiveCase{false, ControlOperation::owner_loss},
    ActiveCase{false, ControlOperation::owner_loss, true},
    ActiveCase{false, ControlOperation::forget703}));

struct VideoIdrControlFixture {
    using Clock = std::chrono::steady_clock;
    std::string directory;
    aa::trust::ApprovedPhoneStore store;
    Fixture f;
    aa::app::WirelessLink::Config video_config() {
        auto cfg = f.config();
        cfg.sinks.erase(aa::protocol::ChannelRole::video);
        return cfg;
    }
    aa::app::WirelessLink link{{store, f.clock, f.diagnostics}, video_config()};
    aa::app::HandoffEligibilityCoordinator coordinator{store,
         {[this](aa::app::HandoffOperations::Validation request) -> aa::core::Result<aa::app::HandoffObservation> {
             if (!f.helper_live) return aa::Error{aa::ErrorCode::helper_unavailable};
             return aa::app::HandoffObservation{f.peer, request.credentials_allowed ? std::optional{f.snapshot} : std::nullopt};
         }, [](aa::helper::Source, std::uint64_t, bool) -> aa::core::Result<void> { return {}; },
         [](aa::helper::Source, std::uint64_t) -> aa::core::Result<void> { return {}; }},
        {[this](const aa::helper::ApSnapshot& ap) { return link.listen(ap); }, [this] { link.close_admission(); }}};
    ControlHelper helper;
    aa::app::WirelessConfirmation confirmation{helper, [this] {
        return link.session() && link.session()->state() == aa::session::State::active;
    }, [this](aa::core::ConsumerId) { coordinator.consumer(std::nullopt); }};
    aa::trust::SteadyClock clock;
    aa::trust::PairingPolicy phones{store, clock};
    aa::ipc::test::FakePeerCredentials peers;
    aa::ipc::test::RecordingEventSink events;
    aa::ipc::test::SequencePairingIds ids;
    aa::ipc::ControlService control{{::getuid()}, {peers, events, ids, phones, &confirmation}};
    const aa::ipc::BusName caller{":1.901"}, name{"org.custom.VideoConsumer"};
    aa::core::PhoneId phone{};

    VideoIdrControlFixture()
        : directory(ControlIoFixture::temporary_directory()),
          store(std::filesystem::path{directory} / "phones") {}
    ~VideoIdrControlFixture() {
        link.stop();
        coordinator.retire();
        std::filesystem::remove_all(directory);
    }
    void start() {
        ASSERT_TRUE(store.load());
        const auto approved = store.approve(aa::trust::TransportIdentity{aa::trust::WirelessIdentity{
            f.peer.identity.address, f.peer.identity.adapter}});
        ASSERT_TRUE(approved);
        phone = approved.value();
        peers.add_connection(caller, ::getuid());
        peers.set_owner(name, caller);
        const auto consumer = control.register_consumer(caller, name);
        ASSERT_TRUE(consumer);
        ASSERT_TRUE(control.start_projection(caller));
        coordinator.consumer(consumer.value());
        coordinator.begin({consumer.value().value, 0});
        const auto until = Clock::now() + 500ms;
        while (!coordinator.admitting() && Clock::now() < until) {
            coordinator.tick();
            std::this_thread::sleep_for(2ms);
        }
        ASSERT_TRUE(coordinator.admitting());
        ASSERT_TRUE(link.listening());
    }
};

class WirelessVideoIdr : public testing::TestWithParam<proto::VideoFocusMode> {};

TEST_P(WirelessVideoIdr, PartialFocusRestoreFailureRetiresWirelessSessionWithoutLaterVideo) {
    const auto initial_mode = GetParam();
    ScopedVideoFocusFault fault;
    VideoIdrControlFixture fix;
    ASSERT_NO_FATAL_FAILURE(fix.start());

    std::vector<std::string> logs;
    auto steady_now = std::chrono::steady_clock::time_point{std::chrono::seconds{400}};
    aa::wired_receiver::VideoOutlet outlet{[&](const std::string& line) { logs.push_back(line); }};
    outlet.set_idr_recovery({
        [&fix] { return fix.link.session() && fix.link.session()->video_projecting(); },
        [&fix] { return fix.link.session()->request_video_idr(); },
        [&steady_now] { return steady_now; },
        [&fix](bool focus_transition) { return fix.link.session()->request_video_idr(focus_transition); },
        aa::ipc::IdrRecoveryStrategy::auto_escalate,
    });

    auto tcp = fix.f.connect();
    ASSERT_TRUE(tcp);
    tcp.value()->set_io_deadline(ControlIoFixture::Clock::now() + 5s);

    std::atomic<bool> active{}, video_ready{}, send_keepalive_ping{}, keepalive_acked{};
    std::mutex ping_mutex;
    std::condition_variable ping_cv;
    std::vector<std::vector<std::byte>> phone_video_messages;
    std::optional<aa::ErrorCode> phone_close_error;
    auto phone = std::async(std::launch::async, [&] {
        Codec cipher{aa::tls::Policy{[] { return true; }, aa::tls::Mode::encryption_only_compatibility}, Codec::Role::server};
        auto version = receive(*tcp.value());
        ASSERT_TRUE(version);
        ASSERT_TRUE(proto::decode_version_request(version.value().payload));
        ASSERT_TRUE(send(*tcp.value(), 0, proto::encode(proto::VersionResponse{1, 6, 0}).value()));
        bool authenticated = false;
        for (int round = 0; round < 16 && !authenticated; ++round) {
            auto message = receive(*tcp.value());
            ASSERT_TRUE(message);
            if (auto auth = proto::decode_auth_complete(message.value().payload); auth) {
                ASSERT_TRUE(cipher.is_active());
                authenticated = true;
            } else {
                auto ssl = proto::decode_encapsulated_ssl(message.value().payload);
                ASSERT_TRUE(ssl);
                cipher.give_handshake(ssl.value().bytes);
                ASSERT_TRUE(cipher.step_handshake());
                auto output = cipher.take_handshake();
                if (!output.empty()) {
                    ASSERT_TRUE(send(*tcp.value(), 0, proto::encode(proto::EncapsulatedSsl{std::move(output)}).value()));
                }
            }
        }
        ASSERT_TRUE(authenticated);
        ASSERT_TRUE(send(*tcp.value(), 0, {std::byte{0x00}, std::byte{0x05}}, &cipher));
        auto discovery = receive(*tcp.value(), &cipher);
        ASSERT_TRUE(discovery);
        ASSERT_TRUE(send(*tcp.value(), 0, proto::encode(proto::ChannelOpenRequest{0, proto::ServiceKey{73}}).value(), &cipher));
        ASSERT_TRUE(receive(*tcp.value(), &cipher));
        ASSERT_TRUE(send(*tcp.value(), 73, {std::byte{0x80}, std::byte{0x01}}, &cipher));
        ASSERT_TRUE(receive(*tcp.value(), &cipher));
        for (int round = 0; round < 100 && !active.load(); ++round) {
            ASSERT_TRUE(send(*tcp.value(), 0, proto::encode(proto::PingRequest{round, false, {}}).value(), &cipher));
            auto ping = receive(*tcp.value(), &cipher);
            ASSERT_TRUE(ping);
            ASSERT_TRUE(proto::decode_ping_response(ping.value().payload));
            std::this_thread::sleep_for(std::chrono::milliseconds{5});
        }
        ASSERT_TRUE(active.load());

        // Set up video channel and wait for ping barrier, then send ZERO further video AUs.
        ASSERT_TRUE(send(*tcp.value(), 99, proto::encode(proto::AvSinkSetup{proto::AvSinkCodec::h264}).value(), &cipher));
        ASSERT_TRUE(receive(*tcp.value(), &cipher)); // AvSinkReady
        ASSERT_TRUE(receive(*tcp.value(), &cipher)); // Initial VideoFocusNotification(projected, true)
        ASSERT_TRUE(send(*tcp.value(), 99, proto::encode(proto::AvSinkStart{1, 0}).value(), &cipher));
        if (initial_mode == proto::VideoFocusMode::projected_no_input_focus) {
            ASSERT_TRUE(send(*tcp.value(), 99, proto::encode(proto::VideoFocusRequest{initial_mode}).value(), &cipher));
            ASSERT_TRUE(receive(*tcp.value(), &cipher)); // VideoFocusNotification(projected_no_input_focus, false)
        }
        ASSERT_TRUE(send(*tcp.value(), 0, proto::encode(proto::PingRequest{999, false, {}}).value(), &cipher));
        auto ready_ping = receive(*tcp.value(), &cipher);
        ASSERT_TRUE(ready_ping);
        ASSERT_TRUE(proto::decode_ping_response(ready_ping.value().payload));
        video_ready = true;

        {
            std::unique_lock lock(ping_mutex);
            ping_cv.wait(lock, [&] { return send_keepalive_ping.load(); });
        }
        ASSERT_TRUE(send(*tcp.value(), 0, proto::encode(proto::PingRequest{1000, false, {}}).value(), &cipher));
        for (;;) {
            auto alive_ping = receive(*tcp.value(), &cipher);
            ASSERT_TRUE(alive_ping);
            if (alive_ping.value().channel == 99) {
                phone_video_messages.push_back(std::move(alive_ping.value().payload));
                continue;
            }
            ASSERT_EQ(alive_ping.value().channel, 0);
            ASSERT_TRUE(proto::decode_ping_response(alive_ping.value().payload));
            {
                std::lock_guard lock(ping_mutex);
                keepalive_acked = true;
            }
            ping_cv.notify_all();
            break;
        }

        tcp.value()->set_io_deadline(ControlIoFixture::Clock::now() + 4s);
        for (;;) {
            auto msg = receive(*tcp.value(), &cipher);
            if (!msg) {
                phone_close_error = msg.error().code();
                break;
            }
            if (msg.value().channel == 99) {
                phone_video_messages.push_back(std::move(msg.value().payload));
            }
        }
    });

    aa::app::WiredSession* configured = nullptr;
    const auto ensure_video_sink = [&] {
        const auto state = fix.control.state();
        fix.coordinator.consumer(state.consumer.value ? std::optional{state.consumer} : std::nullopt);
        if (auto* session = fix.link.session(); session && session != configured) {
            configured = session;
            EXPECT_TRUE(session->register_av_sink(aa::protocol::ChannelRole::video, outlet));
        }
        if (fix.link.session() && fix.link.session()->state() == aa::session::State::active) {
            active = true;
        }
    };

    const auto until = ControlIoFixture::Clock::now() + 4s;
    while (!video_ready.load() && ControlIoFixture::Clock::now() < until) {
        ASSERT_TRUE(fix.link.poll(fix.coordinator, ensure_video_sink));
    }
    ASSERT_TRUE(video_ready.load());
    ASSERT_NE(fix.link.session(), nullptr);
    ASSERT_TRUE(fix.link.session()->video_projecting());

    // Step 1-3: Escalate through 3 nudges.
    for (int i = 0; i < 3; ++i) {
        const auto nudge = outlet.request_idr_external("consumer_ipc");
        ASSERT_TRUE(nudge);
        EXPECT_TRUE(nudge.value());
        steady_now += aa::ipc::kIdrRequestMinInterval;
    }

    // Step 4a: Fail FIRST send (native) in focus_transition — session must stay active and poll must succeed.
    const int base_focus_sends = fault.focus_sends();
    fault.fail_at(base_focus_sends + 1);
    const auto first_fail = outlet.request_idr_external("consumer_ipc");
    ASSERT_FALSE(first_fail);
    EXPECT_EQ(first_fail.error().code(), aa::ErrorCode::transport_io);
    EXPECT_EQ(fault.native_sends(), 0);
    ASSERT_NE(fix.link.session(), nullptr);
    EXPECT_EQ(fix.link.session()->state(), aa::session::State::active);
    EXPECT_FALSE(fix.link.session()->failure().has_value());
    EXPECT_TRUE(fix.link.session()->video_projecting());
    const auto before_pings = fix.link.session()->pings_answered();
    {
        std::lock_guard lock(ping_mutex);
        send_keepalive_ping = true;
    }
    ping_cv.notify_all();
    const auto alive_until = ControlIoFixture::Clock::now() + 2s;
    while (fix.link.session() && fix.link.session()->pings_answered() == before_pings &&
           ControlIoFixture::Clock::now() < alive_until) {
        const auto still_alive = fix.link.poll(fix.coordinator, ensure_video_sink);
        ASSERT_TRUE(still_alive);
    }
    ASSERT_NE(fix.link.session(), nullptr);
    EXPECT_GT(fix.link.session()->pings_answered(), before_pings);
    {
        std::unique_lock lock(ping_mutex);
        ASSERT_TRUE(ping_cv.wait_for(lock, 2s, [&] { return keepalive_acked.load(); }));
    }

    // Step 4b: Inside the owner-loop checkpoint (no subsequent video AUs), fail SECOND send (restore) after
    // native succeeds; Runtime/owner forwards channel_focus_restore_failed and WirelessLink::poll exits immediately.
    bool partial_dispatched = false;
    aa::core::Result<bool> partial_poll{true};
    const auto partial_until = ControlIoFixture::Clock::now() + 2s;
    const auto owner_checkpoint = [&] {
        ensure_video_sink();
        if (!partial_dispatched && !outlet.idr_dispatching()) {
            partial_dispatched = true;
            fault.fail_at(fault.focus_sends() + 2);
            const auto idr = outlet.request_idr_external("consumer_ipc");
            EXPECT_FALSE(idr);
            if (!idr) {
                EXPECT_EQ(idr.error().code(), aa::ErrorCode::channel_focus_restore_failed);
                if (auto* live = fix.link.session(); live && !live->failure()) {
                    (void)live->fail(idr.error());
                }
            }
        }
    };
    while (ControlIoFixture::Clock::now() < partial_until) {
        partial_poll = fix.link.poll(fix.coordinator, owner_checkpoint);
        if (!partial_poll) {
            break;
        }
    }
    ASSERT_TRUE(partial_dispatched);
    ASSERT_FALSE(partial_poll);
    EXPECT_EQ(partial_poll.error().code(), aa::ErrorCode::channel_focus_restore_failed);
    EXPECT_EQ(fix.link.session(), nullptr);
    EXPECT_FALSE(fix.coordinator.valid_session());
    EXPECT_FALSE(fix.link.listening());
    EXPECT_FALSE(outlet.idr_dispatching());
    EXPECT_EQ(fault.native_sends(), 1);

    phone.get();
    ASSERT_TRUE(phone_close_error.has_value());
    EXPECT_NE(*phone_close_error, aa::ErrorCode::timeout);

    const auto expected_nudge = proto::encode(proto::VideoFocusNotification{initial_mode, true}).value();
    const auto expected_native = proto::encode(proto::VideoFocusNotification{proto::VideoFocusMode::native, true}).value();
    EXPECT_EQ(std::count(phone_video_messages.begin(), phone_video_messages.end(), expected_nudge), 3);
    EXPECT_EQ(std::count(phone_video_messages.begin(), phone_video_messages.end(), expected_native), 1);

    EXPECT_NE(std::find(logs.begin(), logs.end(),
                        aa::ipc::idr_transition_failed_json(
                            "producer", "restore_focus", "consumer_ipc", 4, 4,
                            "channel_focus_restore_failed")),
              logs.end());
}

INSTANTIATE_TEST_SUITE_P(FocusModes, WirelessVideoIdr, testing::Values(
    proto::VideoFocusMode::projected,
    proto::VideoFocusMode::projected_no_input_focus));
}
