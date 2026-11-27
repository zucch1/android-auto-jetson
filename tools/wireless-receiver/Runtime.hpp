// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "AdmissionEvents.hpp"
#include "Media.hpp"
#include <aa/app/WirelessReceiver.hpp>
#include <aa/ipc/ControlBusService.hpp>
#include <aa/ipc/IdrRecovery.hpp>
#include <aa/ipc/InputChannel.hpp>
#include <aa/ipc/VideoChannel.hpp>
#include <algorithm>
#include <csignal>
#include <deque>
#include <limits>
#include <thread>

namespace aa::wireless_receiver {
inline void on_signal(int) { running.store(false); }

struct Runtime final {
    using Clock = std::chrono::steady_clock;
    app::WirelessReceiver& receiver;
    helper::BusClient& helper;
    ipc::ControlBusService& bus;
    ipc::VideoServer& video_server;
    ipc::InputServer& input_server;
    Media& media;
    // nullopt: admit whichever approved wireless phone the helper observes.
    std::optional<trust::TransportIdentity> identity;
    Clock::time_point deadline;
    std::optional<ipc::InputEndpoint> input;
    std::deque<ipc::InputEvent> queued_input;
    std::optional<helper::Source> acquisition;
    core::ConsumerId owner{};
    std::uint64_t confirmation{};
    Clock::time_point retry{}, attempt_until{};
    std::chrono::milliseconds backoff{250};
    bool attempting{}, failed{}, stopping{};
    bool audio_failure_reported{};
    bool in_audio_completions{};
    bool in_poll_{};
    std::optional<Error> pending_ack_error_{};
    app::WiredSession* configured{};
    std::size_t offered_video_profiles{};
    ipc::VideoSelectionWatchdog selection_watchdog{};
    std::function<void(bool session_live)> on_turn_for_test{};
    std::function<void(app::WiredSession&, bool in_poll)> on_before_audio_completions_for_test{};
    std::function<void(app::WiredSession&, bool in_poll, const core::Result<void>& serviced)>
        on_after_audio_completions_for_test{};

    // A managed projection stops its media and leaves the shared bus alive.
    std::function<bool(ipc::StateSnapshot, const std::optional<trust::TransportIdentity>&)>
        keep_running{};
    // The phone the current attempt admitted (the fixed identity, or the one observed).
    std::optional<trust::TransportIdentity> admitted{};

    // Ends this projection. A managed projection's failure stays contained to
    // this transport: only a process-level cause (signal, deadline, stdout log
    // or bus failure) clears the process-wide flag the supervisor runs on.
    void stop_runtime(bool process = false) {
        stopping = true;
        if (process || !keep_running) running.store(false);
    }

    bool service_audio_failure() {
        const auto error = media.audio.failure();
        if (!error) return false;
        failed = true;
        stop_runtime();
        if (auto* live = receiver.session(); live && !live->failure()) {
            (void)live->fail(*error);
        }
        if (!audio_failure_reported) {
            audio_failure_reported = true;
            log_line_nonblocking(audio::audio_failure_event_json(
                "audio_sink_failure", error->code(), media.audio.failure_context()));
        }
        return true;
    }

    void service_audio_completions_if_ready() {
        if (service_audio_failure() || in_audio_completions) return;
        auto* audio_session = receiver.session();
        if (!audio_session || audio_session != configured ||
            audio_session->outbound_in_progress() || audio_session->failure()) {
            return;
        }
        in_audio_completions = true;
        struct Guard {
            bool& active;
            ~Guard() { active = false; }
        } guard{in_audio_completions};
        if (on_before_audio_completions_for_test) {
            on_before_audio_completions_for_test(*audio_session, in_poll_);
        }
        const auto serviced = app::service_audio_completions(*audio_session, media.audio, false);
        if (on_after_audio_completions_for_test) {
            on_after_audio_completions_for_test(*audio_session, in_poll_, serviced);
        }
        if (!serviced) {
            if (!service_audio_failure()) {
                const auto& error = serviced.error();
                if (error.code() == ErrorCode::cancelled &&
                    (!running.load() || Clock::now() >= deadline || stopping)) {
                    stop_runtime();
                    return;
                }
                if (in_poll_) {
                    if (!pending_ack_error_) pending_ack_error_ = error;
                    audio_session->request_stop();
                } else {
                    report("wireless link retired", error.code());
                    defer();
                }
            }
        }
    }

    void checkpoint() {
        if (!running.load() || Clock::now() >= deadline) stop_runtime();
        if (stopping) {
            if (auto* session = receiver.session()) session->request_stop();
            return;
        }
        if (auto* session = receiver.session(); session && session != configured) {
            configured = session;
            if (auto bound = media.bind(*session); !bound) {
                report("session media sinks", bound.error().code()); failed = true; stop_runtime();
            }
        }
        service_audio_completions_if_ready();
        if (!running.load() || Clock::now() >= deadline) stop_runtime();
        if (stopping) {
            if (auto* session = receiver.session()) session->request_stop();
            return;
        }
        receiver.set_io_checkpoint_interval(
            (media.video.has_pending() || media.audio.outstanding())
                ? std::chrono::milliseconds{5}
                : std::chrono::milliseconds{25});
        if (auto step = bus.cycle(core::Milliseconds{0}); !step) {
            report("session bus dispatch", step.error().code()); failed = true; stop_runtime(true);
        }
        const auto state = bus.service().state();
        if (stopping || (keep_running && !keep_running(state, admitted))) {
            stopping = true;
            if (auto* session = receiver.session()) session->request_stop();
            return;
        }
        auto* recovery_session = receiver.session();
        if (recovery_session && !recovery_session->outbound_in_progress()) {
            if (auto pending = recovery_session->service_pending_video_idr(); !pending) {
                report("idr recovery", pending.error().code());
            }
        }
        // Held video leaves on time even while the session is mid-send: the
        // release needs no session I/O (IDR requests wait for service_pending).
        if (auto held = media.video.release_held(); !held) {
            report("video held release", held.error().code());
            if (auto* live = receiver.session(); live && !live->failure()) { (void)live->fail(held.error()); }
        }
        if ((!recovery_session || !recovery_session->outbound_in_progress()) &&
            !media.video.idr_dispatching()) {
            if (auto pending_video = media.video.service_pending(); !pending_video) {
                report("video pending flush", pending_video.error().code());
                if (auto* live = receiver.session(); live && !live->failure()) {
                    (void)live->fail(pending_video.error());
                }
            }
        }
        if ((!recovery_session || !recovery_session->outbound_in_progress()) &&
            !media.video.idr_dispatching() && bus.service().consume_idr_recovery_request()) {
            const auto idr = media.video.request_idr_external("consumer_ipc");
            if (!idr) {
                report("idr recovery", idr.error().code());
                if (idr.error().code() == ErrorCode::channel_focus_restore_failed) {
                    if (auto* live = receiver.session(); live && !live->failure()) {
                        (void)live->fail(idr.error());
                    }
                }
            }
        }
        auto* live_session = receiver.session();
        const bool session_ready = live_session &&
            live_session->state() == session::State::active && live_session->authenticated();
        const bool has_selection =
            live_session && live_session->selected_video_profile().has_value();
        if (selection_watchdog.check(session_ready, has_selection, Clock::now())) {
            log_media("video_selection_timeout",
                      ipc::video_selection_timeout_json(
                          ipc::kVideoSelectionWarningTimeout, offered_video_profiles));
        }
        // Owner loss is handled by WirelessConfirmation's confirmed-ACTIVE grace.
        // An explicit StopProjection by the still-live owner is immediate.
        if (stopping || (state.consumer.value && state.projection == ipc::ProjectionState::stopped)) {
            if (auto* session = receiver.session()) session->request_stop();
        }
        if (!recovery_session || !recovery_session->outbound_in_progress()) {
            auto producer = video_server.accept_consumer(core::Milliseconds{0});
            if (producer) media.video.install(std::move(producer.value()));
        }
        if (!input) {
            auto accepted = input_server.accept_sender(core::Milliseconds{0});
            if (accepted) input.emplace(std::move(accepted.value()));
        }
        // Bound input work even if a consumer continuously floods its socket.
        for (unsigned i = 0; input && i < 8; ++i) {
            auto event = input->receive(core::Milliseconds{0});
            if (!event) {
                if (event.error().code() != ErrorCode::timeout) input.reset();
                break;
            }
            if (queued_input.size() < 64 && receiver.active() &&
                state.projection == ipc::ProjectionState::projecting)
                queued_input.push_back(event.value());
        }
        if (on_turn_for_test) { on_turn_for_test(receiver.session() != nullptr); }
    }
    void defer() {
        pending_ack_error_.reset();
        receiver.stop(); media.video.reset(); configured = nullptr; attempting = false; acquisition.reset(); queued_input.clear();
        if (!identity) admitted.reset();
        retry = Clock::now() + backoff;
        backoff = std::min(backoff * 2, std::chrono::milliseconds{5000});
    }
    core::Result<void> admit(ipc::StateSnapshot state) {
        // reconnect() stops the previous epoch and settles its denial before contacting the helper.
        if (identity) {
            const auto result = receiver.reconnect(bus.service(), *identity);
            media.video.reset(); configured = nullptr;
            if (!result) return result.error();
            admitted = identity;
        } else {
            auto result = receiver.reconnect_observed(bus.service());
            media.video.reset(); configured = nullptr;
            if (!result) return result.error();
            admitted = std::move(result.value());
        }
        auto status = helper.status(helper::Action::ap_status);
        if (!status) return status.error();
        if (status.value() != helper::State::active) {
            if (confirmation == std::numeric_limits<std::uint64_t>::max())
                return Error{ErrorCode::invalid_argument};
            const helper::Source source{state.consumer.value, ++confirmation};
            receiver.stop(); media.video.reset();
            const auto acquired = receiver.acquire_ap(bus.service(), source);
            if (!acquired) return acquired.error();
            // No reconnect here: acquisition owns the helper lease from now on.
            acquisition = source;
            retry = Clock::now();
        }
        attempting = true;
        attempt_until = Clock::now() + std::chrono::seconds{15};
        log_line("{\"event\":\"wireless_admission\"}");
        return {};
    }
    int run() {
        while (!stopping) {
            const auto turn = Clock::now();
            checkpoint();
            if (stopping) break;
            const auto state = bus.service().state();
            const bool wants = state.consumer.value && state.projection == ipc::ProjectionState::projecting;
            if (wants && owner != state.consumer) {
                receiver.stop(); media.video.reset(); configured = nullptr; owner = state.consumer; attempting = false;
                acquisition.reset(); queued_input.clear();
                retry = turn; backoff = std::chrono::milliseconds{250};
            }
            if (!wants && state.consumer.value) {
                receiver.stop();
                if (configured || media.video.has_pending()) media.video.reset();
                configured = nullptr; attempting = false; acquisition.reset(); queued_input.clear();
            }
            // A bootstrap observed late in the attempt runs to its own bounded deadline: deferring
            // would deny it and leave the phone retrying against a fresh window (#161).
            if (wants && attempting && !receiver.session() && turn >= attempt_until && !receiver.bootstrapping()) {
                // Not an error code: name why the attempt deadline passed instead of retrying
                // silently (#160). No addresses or identities are logged.
                // Asked at expiry, before defer() stops the epoch: the link may have arrived since.
                log_line(admission_expired_json(acquisition.has_value(),
                    !acquisition && receiver.link_wait() == helper::BootstrapWait::no_bluetooth_link));
                defer();
            }
            if (wants && !receiver.session() && !attempting && turn >= retry) {
                if (auto admitted = admit(state); !admitted) {
                    report("approved wireless admission", admitted.error().code()); defer();
                }
            }
            if (wants && acquisition && Clock::now() >= retry) {
                const auto status = helper.status(helper::Action::ap_status);
                if (!status) { report("AP activation status", status.error().code()); defer(); }
                else if (status.value() == helper::State::active) {
                    const auto begun = receiver.begin(bus.service(), *acquisition);
                    acquisition.reset();
                    if (!begun) { report("AP handoff begin", begun.error().code()); defer(); }
                } else retry = Clock::now() + std::chrono::milliseconds{250};
            }
            pending_ack_error_.reset();
            const auto polled = [&] {
                in_poll_ = true;
                struct Guard {
                    bool& active;
                    ~Guard() { active = false; }
                } guard{in_poll_};
                return receiver.poll(bus.service(), [this] { checkpoint(); });
            }();
            if (!polled) {
                if (!service_audio_failure()) {
                    const auto code = pending_ack_error_ ? pending_ack_error_->code()
                                                         : polled.error().code();
                    if (code == ErrorCode::ipc_queue_full ||
                        code == ErrorCode::channel_not_registered) {
                        log_line(app::poll_error_json(
                            code, media.audio, receiver.last_reject(),
                            receiver.last_queue_rejection()));
                    }
                    report("wireless link retired", code);
                    defer();
                }
            } else {
                service_audio_completions_if_ready();
            }
            if (on_turn_for_test) { on_turn_for_test(receiver.session() != nullptr); }
            if (receiver.active()) backoff = std::chrono::milliseconds{250};
            for (unsigned i = 0; i < 8 && !queued_input.empty(); ++i) {
                const auto event = queued_input.front(); queued_input.pop_front();
                const auto live = bus.service().state();
                if (receiver.active() && live.consumer == owner &&
                    live.projection == ipc::ProjectionState::projecting) {
                    const auto sent = receiver.session()->submit_input(event);
                    if (!sent) { report("input forwarding", sent.error().code()); break; }
                }
            }
            // TcpTransport::poll_receive() is intentionally unsupported, so
            // WirelessLink::poll() calls blocking session_->poll_transport(), which
            // runs this checkpoint() on TcpIo's configurable poll slices while TCP is idle.
            // While video frames or audio completions are outstanding, TcpIo uses 5 ms slices.
            const auto poll_step = (media.video.has_pending() || media.audio.outstanding())
                ? std::chrono::milliseconds{5}
                : std::chrono::milliseconds{10};
            if (!stopping && (!polled || !polled.value())) {
                std::this_thread::sleep_until(std::min(deadline, turn + poll_step));
            }
        }
        if (!keep_running) {
            running.store(false);
            bus.service().shutdown();
        }
        media.audio.stop();
        receiver.stop();
        if (on_turn_for_test) { on_turn_for_test(false); }
        media.video.close();
        if (auto finished = media.audio.finish(); !finished) {
            if (!audio_failure_reported) {
                audio_failure_reported = true;
                log_line(audio::audio_failure_event_json(
                    "audio_finish", finished.error().code(), media.audio.failure_context()));
            }
            failed = true;
        }
        log_line("{\"event\":\"stopping\"}");
        if (const auto* writer = diagnostics::active_stdout_log.load();
            writer && writer->latched_error()) {
            failed = true;
        }
        return failed ? 1 : 0;
    }
};
} // namespace aa::wireless_receiver
