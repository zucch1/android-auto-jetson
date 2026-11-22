// SPDX-License-Identifier: GPL-3.0-or-later
#include <aa/display/Display.hpp>
#include <aa/ipc/IdrRecovery.hpp>

#include <utility>

namespace aa::display {

namespace {

using aa::core::Milliseconds;

#if AA_DISPLAY_HAS_CONSUMER
// Production decode session: the real todo-23 consumer helper behind the
// DecodeSession seam (GStreamer stays in aareceiver-consumer's adapter).
// Compiled ONLY when aareceiver-consumer exists in this configuration
// (AA_DISPLAY_HAS_CONSUMER, set PRIVATE from that target): HelperSession
// references the helper's submit/pump/close/destruction, so guarding open()
// alone would still leave undefined symbols for every downstream link.
class HelperSession final : public DecodeSession {
public:
    explicit HelperSession(consumer::DecodeHelper helper) : helper_(std::move(helper)) {}

    [[nodiscard]] core::Result<void> submit(const ipc::VideoAccessUnit& unit) override {
        return helper_.submit(unit);
    }
    [[nodiscard]] core::Result<void> end_of_stream() override { return helper_.end_of_stream(); }
    [[nodiscard]] core::Result<std::size_t> pump(Milliseconds wait) override {
        return helper_.pump(wait);
    }
    [[nodiscard]] const consumer::DecodeReport& report() const noexcept override {
        return helper_.report();
    }
    void close() noexcept override { helper_.close(); }

private:
    consumer::DecodeHelper helper_;
};
#endif

constexpr std::size_t kReceiveBudget = 16;

} // namespace

DecodeSessionFactory helper_decode_sessions() {
#if AA_DISPLAY_HAS_CONSUMER
    return [](const consumer::DecodeConfig& config)
               -> core::Result<std::unique_ptr<DecodeSession>> {
        auto opened = consumer::DecodeHelper::open(config);
        if (!opened.has_value()) {
            return opened.error();
        }
        return std::unique_ptr<DecodeSession>(
            std::make_unique<HelperSession>(std::move(opened.value())));
    };
#else
    // Typed-unavailable factory (the rejecting_factory() convention): with no
    // aareceiver-consumer target in this configuration there is no decoder
    // backend to open, so every request fails channel_decode_unavailable -
    // the toolkit-free session logic still compiles AND links everywhere.
    return [](const consumer::DecodeConfig&)
               -> core::Result<std::unique_ptr<DecodeSession>> {
        return Error{ErrorCode::channel_decode_unavailable};
    };
#endif
}

struct DisplayController::Impl final {
    ipc::ControlBusClient& control;
    DecodeSessionFactory decode_sessions;
    FrameTarget& frames;
    DisplayConfig config{};
    protocol::VideoProfile active_caps{};
    DisplayState state{DisplayState::unavailable};
    core::ConsumerId consumer{};
    std::uint64_t frames_presented{0};
    bool helper_started{false};
    bool eof{false};
    std::optional<ipc::VideoConsumer> video{};
    std::unique_ptr<DecodeSession> session{};
    // Exactly one rejected AU is retained. While occupied, receive() is not
    // called: saturation applies source backpressure instead of dropping or
    // queueing later units, and this AU's metadata remains untouched.
    std::optional<ipc::VideoAccessUnit> pending_unit{};
    bool saw_video_unit{false};
    std::optional<std::chrono::steady_clock::time_point> projection_started_at{};
    ipc::IdrRecoveryCoordinator idr_recovery{};

    [[nodiscard]] std::chrono::steady_clock::time_point now() const {
        return config.clock ? config.clock() : std::chrono::steady_clock::now();
    }

    void emit_diagnostic(std::string line) const {
        if (config.on_diagnostic) {
            config.on_diagnostic(std::move(line));
        }
    }

    void configure_receive_buffer(ipc::VideoConsumer& consumer,
                                  const protocol::VideoProfile& caps) const {
        if (const auto sized = consumer.configure_receive_buffer(caps); sized.has_value()) {
            emit_diagnostic(ipc::video_socket_buffer_json("consumer", sized.value(), caps));
            if (sized.value().has_warning()) {
                emit_diagnostic(ipc::video_socket_buffer_warning_json("consumer", sized.value(), caps));
            }
        }
    }

    void observe_idr_recovery() {
        const auto current_time = now();
        const bool awaiting = session && session->report().awaiting_idr;
        const bool past_startup_grace =
            projection_started_at.has_value() &&
            (current_time - *projection_started_at) >= ipc::kIdrStartupGrace;
        const bool socket_waiting =
            video.has_value() && video->waiting_idr() &&
            (saw_video_unit || video->rejected() > 0 || video->suppressed() > 0 || past_startup_grace);
        if (awaiting || socket_waiting) {
            const char* reason = awaiting ? "decode_awaiting_idr" : "video_consumer_waiting_idr";
            if (!idr_recovery.waiting()) {
                emit_diagnostic(ipc::idr_wait_started_json("consumer", reason));
            }
            idr_recovery.begin_wait(current_time);
            if (idr_recovery.try_request(current_time, state == DisplayState::projecting)) {
                const auto requested = control.request_idr_recovery();
                if (!requested.has_value()) {
                    // An idle link may become active at any point. Keep the
                    // normal cadence for that state and back off on I/O errors.
                    const bool backoff = requested.error().code() != ErrorCode::session_illegal_transition;
                    idr_recovery.note_request_failed(current_time, backoff);
                } else {
                    emit_diagnostic(ipc::idr_requested_json("consumer", reason));
                }
            }
            return;
        }
        if (idr_recovery.waiting()) {
            const auto recovered = idr_recovery.on_idr_delivered(current_time);
            if (recovered.recovered) {
                emit_diagnostic(ipc::idr_recovered_json("consumer", recovered.latency_ms,
                                                        recovered.episode_requests,
                                                        recovered.total_requests));
            }
        }
    }

    void on_decoded(const consumer::DecodedFrame& frame) {
        ++frames_presented;
        frames.on_frame(frame);
    }

    void release_media() noexcept {
        pending_unit.reset();
        saw_video_unit = false;
        projection_started_at.reset();
        idr_recovery.reset();
        if (session) {
            session->close();
            session.reset();
        }
        if (video.has_value()) {
            video->close();
            video.reset();
        }
        eof = false;
    }

    // Failed-stop reconciliation: exactly ONE GetState exchange, run ONLY
    // after a failed stop (never per pump/tick). The snapshot decides the
    // state per the decision table and releases or retains the media to
    // match: same consumer stopped -> media released, idle; still projecting
    // or the reconciliation itself failed -> media RETAINED, stop_failed;
    // different/zero consumer (lost registration) -> media released, consumer
    // id cleared, unavailable.
    void reconcile_failed_stop() noexcept {
        const auto snapshot = control.get_state();
        if (!snapshot.has_value()) {
            state = DisplayState::stop_failed;
            return;
        }
        if (snapshot.value().consumer == core::ConsumerId{} ||
            snapshot.value().consumer != consumer) {
            release_media();
            consumer = core::ConsumerId{};
            state = DisplayState::unavailable;
            return;
        }
        if (snapshot.value().projection == ipc::ProjectionState::stopped) {
            release_media();
            state = DisplayState::idle;
            return;
        }
        state = DisplayState::stop_failed;
    }

    // Startup rollback: unwind the just-started remote projection with a
    // best-effort stop whose failure is reconciled exactly like an operator
    // stop failure (media already unwound by the caller); the caller always
    // preserves the ORIGINAL startup error. A confirmed stop simply keeps the
    // pre-start idle state.
    void rollback_projection_start() noexcept {
        const auto stopped = control.stop_projection();
        if (!stopped.has_value()) {
            reconcile_failed_stop();
            return;
        }
        state = DisplayState::idle;
    }
};

core::Result<DisplayController> DisplayController::attach(Deps deps, DisplayConfig config) {
    auto impl = std::make_unique<Impl>(Impl{deps.control, std::move(deps.decode_sessions),
                                            deps.frames, std::move(config)});
    const auto owner = impl->control.receiver_owner();
    if (!owner.has_value()) {
        return owner.error();
    }
    if (!owner.value().has_value()) {
        return DisplayController(std::move(impl));
    }
    const auto registered = impl->control.register_consumer(impl->config.consumer_name);
    if (!registered.has_value()) {
        return registered.error();
    }
    const auto viewport_set = impl->control.set_display_viewport(impl->config.viewport);
    if (!viewport_set.has_value()) {
        static_cast<void>(impl->control.unregister_consumer());
        return viewport_set.error();
    }
    impl->consumer = registered.value();
    impl->state = DisplayState::idle;
    return DisplayController(std::move(impl));
}

DisplayController::DisplayController(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

DisplayController::~DisplayController() { shutdown(); }

DisplayController::DisplayController(DisplayController&& other) noexcept = default;
DisplayController& DisplayController::operator=(DisplayController&& other) noexcept = default;

DisplayState DisplayController::state() const noexcept {
    return impl_ ? impl_->state : DisplayState::unavailable;
}

core::ConsumerId DisplayController::consumer() const noexcept {
    return impl_ ? impl_->consumer : core::ConsumerId{};
}

std::uint64_t DisplayController::frames_presented() const noexcept {
    return impl_ ? impl_->frames_presented : 0;
}

bool DisplayController::helper_started() const noexcept {
    return impl_ && impl_->helper_started;
}

const consumer::DecodeReport& DisplayController::decode_report() const noexcept {
    static const consumer::DecodeReport empty{};
    return impl_ && impl_->session ? impl_->session->report() : empty;
}

core::Result<void> DisplayController::start_projection(const ipc::VideoSocketPath& video_socket) {
    if (!impl_) {
        return Error{ErrorCode::transport_closed};
    }
    if (impl_->state != DisplayState::idle) {
        return Error{ErrorCode::session_illegal_transition};
    }
    const auto started = impl_->control.start_projection();
    if (!started.has_value()) {
        return started.error();
    }
    consumer::DecodeConfig decode_config{};
    decode_config.on_completion = impl_->config.on_decode_completion;
    decode_config.caps = impl_->config.caps;
    impl_->active_caps = decode_config.caps;
    decode_config.backend = consumer::DecodeBackend::automatic;
    Impl* impl = impl_.get();
    decode_config.on_frame = [impl](const consumer::DecodedFrame& frame) {
        impl->on_decoded(frame);
    };
    auto session = impl_->decode_sessions(decode_config);
    if (!session.has_value()) {
        impl_->rollback_projection_start();
        return session.error();
    }
    auto connected = ipc::connect_video_consumer(
        video_socket, ipc::VideoClientOptions{{}, ipc::current_uid(), Milliseconds{5000}});
    if (!connected.has_value()) {
        session.value()->close();
        impl_->rollback_projection_start();
        return connected.error();
    }
    connected.value().set_observer(impl_->config.on_video_endpoint);
    impl_->configure_receive_buffer(connected.value(), impl_->active_caps);
    impl_->saw_video_unit = false;
    impl_->projection_started_at = impl_->now();
    impl_->idr_recovery.reset();
    impl_->idr_recovery.set_strategy(ipc::IdrRecoveryStrategy::nudge);
    impl_->video.emplace(std::move(connected.value()));
    impl_->session = std::move(session.value());
    impl_->helper_started = true;
    impl_->eof = false;
    impl_->state = DisplayState::projecting;
    return {};
}

core::Result<void> DisplayController::stop_projection() {
    if (!impl_) {
        return Error{ErrorCode::transport_closed};
    }
    if (impl_->state != DisplayState::projecting && impl_->state != DisplayState::stop_failed) {
        return Error{ErrorCode::session_illegal_transition};
    }
    const auto stopped = impl_->control.stop_projection();
    if (stopped.has_value()) {
        impl_->release_media();
        impl_->state = DisplayState::idle;
        return {};
    }
    // Failure: reconcile once through GetState and still return the ORIGINAL
    // stop error (the failure is reported even when the snapshot confirms the
    // stop landed and the state is recovered to idle).
    impl_->reconcile_failed_stop();
    return stopped.error();
}

core::Result<std::size_t> DisplayController::pump(Milliseconds wait) {
    if (!impl_) {
        return Error{ErrorCode::transport_closed};
    }
    const bool pumpable =
        impl_->state == DisplayState::projecting || impl_->state == DisplayState::stop_failed;
    if (!pumpable || !impl_->video.has_value() || !impl_->session) {
        return Error{ErrorCode::session_illegal_transition};
    }
    for (std::size_t step = 0; step < kReceiveBudget; ++step) {
        if (impl_->pending_unit.has_value()) {
            const auto retried = impl_->session->submit(impl_->pending_unit.value());
            if (!retried.has_value()) {
                const auto code = retried.error().code();
                if (code == ErrorCode::ipc_queue_full) {
                    // Still full: keep the sole pending AU and drain below.
                    break;
                }
                // IDR-resync discard and terminal failures are settled, not
                // retryable backpressure; never replay this AU on later pumps.
                impl_->pending_unit.reset();
                impl_->observe_idr_recovery();
                return retried.error();
            }
            impl_->pending_unit.reset();
        }
        const auto status =
            impl_->video->receive(step == 0 ? wait : Milliseconds{0});
        if (!status.has_value()) {
            const auto code = status.error().code();
            if (code == ErrorCode::timeout) {
                break;
            }
            if (code == ErrorCode::transport_closed) {
                if (!impl_->eof) {
                    impl_->eof = true;
                    const auto ended = impl_->session->end_of_stream();
                    if (!ended.has_value()) {
                        return ended.error();
                    }
                }
                break;
            }
            impl_->observe_idr_recovery();
            return status.error();
        }
        if (status.value() != ipc::ReceiveStatus::complete) {
            continue;
        }
        impl_->saw_video_unit = true;
        const auto& unit = impl_->video->access_unit();
        if (unit.profile && *unit.profile != impl_->active_caps) {
            if (!unit.idr) { return Error{ErrorCode::channel_caps_unsupported}; }
            consumer::DecodeConfig config{};
            config.caps = *unit.profile;
            config.on_completion = impl_->config.on_decode_completion;
            config.on_frame = [impl = impl_.get()](const consumer::DecodedFrame& frame) { impl->on_decoded(frame); };
            auto opened = impl_->decode_sessions(config);
            if (!opened) { return opened.error(); }
            impl_->session->close();
            impl_->session = std::move(opened.value());
            impl_->active_caps = *unit.profile;
            impl_->configure_receive_buffer(*impl_->video, impl_->active_caps);
        }
        const auto submitted = impl_->session->submit(unit);
        if (!submitted.has_value()) {
            if (submitted.error().code() != ErrorCode::ipc_queue_full) {
                impl_->observe_idr_recovery();
                return submitted.error();
            }
            // Copy before any subsequent receive can overwrite the consumer's
            // stable complete() state, then drain instead of reading ahead.
            impl_->pending_unit = impl_->video->access_unit();
            break;
        }
    }
    auto frames = impl_->session->pump(wait);
    impl_->observe_idr_recovery();
    if (!frames.has_value()) {
        return frames.error();
    }
    return frames.value();
}

void DisplayController::shutdown() noexcept {
    if (!impl_) {
        return;
    }
    if (impl_->state == DisplayState::projecting || impl_->state == DisplayState::stop_failed) {
        // Best-effort contract teardown (stop + unregister) then media
        // release and cleared state - stop_failed is handled DIRECTLY here,
        // never via a temporary idle, and teardown runs no reconciliation.
        static_cast<void>(impl_->control.stop_projection());
        static_cast<void>(impl_->control.unregister_consumer());
        impl_->release_media();
        impl_->state = DisplayState::unavailable;
        impl_->consumer = core::ConsumerId{};
        return;
    }
    impl_->release_media();
    if (impl_->state == DisplayState::idle) {
        static_cast<void>(impl_->control.unregister_consumer());
        impl_->state = DisplayState::unavailable;
        impl_->consumer = core::ConsumerId{};
    }
}

core::Result<DisplayController> attach_test_only_autostart(const TestOnlyAutostart& gate,
                                                           DisplayController::Deps deps,
                                                           DisplayConfig config) {
    if (!is_test_only(gate)) {
        return Error{ErrorCode::invalid_argument};
    }
    auto controller = DisplayController::attach(std::move(deps), std::move(config));
    if (!controller.has_value()) {
        return controller.error();
    }
    if (controller.value().state() == DisplayState::unavailable) {
        return controller;
    }
    const auto started = controller.value().start_projection(gate.video_socket);
    if (!started.has_value()) {
        return started.error();
    }
    return controller;
}

} // namespace aa::display
