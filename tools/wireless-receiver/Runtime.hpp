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
inline volatile std::sig_atomic_t running = 1;
inline void on_signal(int) { running = 0; }

struct Runtime final {
    using Clock = std::chrono::steady_clock;
    app::WirelessReceiver& receiver;
    helper::BusClient& helper;
    ipc::ControlBusService& bus;
    ipc::VideoServer& video_server;
    ipc::InputServer& input_server;
    Media& media;
    trust::TransportIdentity identity;
    Clock::time_point deadline;
    std::optional<ipc::InputEndpoint> input;
    std::deque<ipc::InputEvent> queued_input;
    std::optional<helper::Source> acquisition;
    core::ConsumerId owner{};
    std::uint64_t confirmation{};
    Clock::time_point retry{}, attempt_until{};
    std::chrono::milliseconds backoff{250};
    bool attempting{}, failed{}, stopping{};
    app::WiredSession* configured{};
    std::size_t offered_video_profiles{};
    ipc::VideoSelectionWatchdog selection_watchdog{};

    void checkpoint() {
        if (auto* session = receiver.session(); session && session != configured) {
            configured = session;
            if (auto bound = media.bind(*session); !bound) {
                report("session media sinks", bound.error().code()); failed = true; stopping = true;
            }
        }
        if (!running || Clock::now() >= deadline) stopping = true;
        if (auto step = bus.cycle(core::Milliseconds{0}); !step) {
            report("session bus dispatch", step.error().code()); failed = true; stopping = true;
        }
        const auto state = bus.service().state();
        auto* recovery_session = receiver.session();
        if (recovery_session && !recovery_session->outbound_in_progress()) {
            if (auto pending = recovery_session->service_pending_video_idr(); !pending) {
                report("idr recovery", pending.error().code());
            }
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
            log_line(ipc::video_selection_timeout_json(
                ipc::kVideoSelectionWarningTimeout, offered_video_profiles));
        }
        // Owner loss is handled by WirelessConfirmation's confirmed-ACTIVE grace.
        // An explicit StopProjection by the still-live owner is immediate.
        if (stopping || (state.consumer.value && state.projection == ipc::ProjectionState::stopped)) {
            if (auto* session = receiver.session()) session->request_stop();
        }
        auto producer = video_server.accept_consumer(core::Milliseconds{0});
        if (producer) media.video.install(std::move(producer.value()));
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
    }
    void defer() {
        receiver.stop(); media.video.reset(); configured = nullptr; attempting = false; acquisition.reset(); queued_input.clear();
        retry = Clock::now() + backoff;
        backoff = std::min(backoff * 2, std::chrono::milliseconds{5000});
    }
    core::Result<void> admit(ipc::StateSnapshot state) {
        // Stop the previous epoch before synchronous, bounded helper operations:
        // there is no live admission lease while these calls run.
        receiver.stop(); media.video.reset(); configured = nullptr;
        if (auto result = receiver.reconnect(bus.service(), identity); !result) return result.error();
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
            const auto polled = receiver.poll(bus.service(), [this] { checkpoint(); });
            if (!polled) {
                report("wireless link retired", polled.error().code()); defer();
            }
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
            // runs this checkpoint() on TcpIo's 25ms poll slices while TCP is idle.
            // Thus when TCP is quiet, held video servicing/expiry runs on ~25ms
            // checkpoint slices and can overshoot nominal max_age by up to ~25ms.
            // Tightening the outer sleep from 10ms to 5ms while frames are held
            // avoids adding a full idle sleep on non-blocked/active turns.
            const auto poll_step = media.video.has_pending() ? std::chrono::milliseconds{5}
                                                             : std::chrono::milliseconds{10};
            std::this_thread::sleep_until(std::min(deadline, turn + poll_step));
        }
        bus.service().shutdown(); receiver.stop(); media.video.reset();
        log_line("{\"event\":\"stopping\"}");
        return failed ? 1 : 0;
    }
};
} // namespace aa::wireless_receiver
