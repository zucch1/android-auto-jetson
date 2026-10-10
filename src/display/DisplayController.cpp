// SPDX-License-Identifier: GPL-3.0-or-later
#include <aa/display/Display.hpp>

#include <utility>

namespace aa::display {

namespace {

using aa::core::Milliseconds;

// Production decode session: the real todo-23 consumer helper behind the
// DecodeSession seam (GStreamer stays in aareceiver-consumer's adapter).
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
    void close() noexcept override { helper_.close(); }

private:
    consumer::DecodeHelper helper_;
};

constexpr std::size_t kReceiveBudget = 16;

} // namespace

DecodeSessionFactory helper_decode_sessions() {
    return [](const consumer::DecodeConfig& config)
               -> core::Result<std::unique_ptr<DecodeSession>> {
        auto opened = consumer::DecodeHelper::open(config);
        if (!opened.has_value()) {
            return opened.error();
        }
        return std::unique_ptr<DecodeSession>(
            std::make_unique<HelperSession>(std::move(opened.value())));
    };
}

struct DisplayController::Impl final {
    ipc::ControlBusClient& control;
    DecodeSessionFactory decode_sessions;
    FrameTarget& frames;
    DisplayConfig config{};
    DisplayState state{DisplayState::unavailable};
    core::ConsumerId consumer{};
    std::uint64_t frames_presented{0};
    bool helper_started{false};
    bool eof{false};
    std::optional<ipc::VideoConsumer> video{};
    std::unique_ptr<DecodeSession> session{};

    void on_decoded(const consumer::DecodedFrame& frame) {
        ++frames_presented;
        frames.on_frame(frame);
    }

    void release_media() noexcept {
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
    auto connected = ipc::connect_video_consumer(
        video_socket, ipc::VideoClientOptions{{}, ipc::current_uid(), Milliseconds{5000}});
    if (!connected.has_value()) {
        static_cast<void>(impl_->control.stop_projection());
        return connected.error();
    }
    consumer::DecodeConfig decode_config{};
    decode_config.caps = impl_->config.caps;
    decode_config.backend = consumer::DecodeBackend::automatic;
    Impl* impl = impl_.get();
    decode_config.on_frame = [impl](const consumer::DecodedFrame& frame) {
        impl->on_decoded(frame);
    };
    auto session = impl_->decode_sessions(decode_config);
    if (!session.has_value()) {
        impl_->video.emplace(std::move(connected.value()));
        impl_->video->close();
        impl_->video.reset();
        static_cast<void>(impl_->control.stop_projection());
        return session.error();
    }
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
    if (impl_->state != DisplayState::projecting) {
        return Error{ErrorCode::session_illegal_transition};
    }
    const auto stopped = impl_->control.stop_projection();
    impl_->release_media();
    impl_->state = DisplayState::idle;
    return stopped;
}

core::Result<std::size_t> DisplayController::pump(Milliseconds wait) {
    if (!impl_) {
        return Error{ErrorCode::transport_closed};
    }
    if (impl_->state != DisplayState::projecting || !impl_->video.has_value() ||
        !impl_->session) {
        return Error{ErrorCode::session_illegal_transition};
    }
    for (std::size_t step = 0; step < kReceiveBudget; ++step) {
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
            return status.error();
        }
        if (status.value() != ipc::ReceiveStatus::complete) {
            continue;
        }
        const auto submitted = impl_->session->submit(impl_->video->access_unit());
        if (!submitted.has_value()) {
            return submitted.error();
        }
    }
    auto frames = impl_->session->pump(wait);
    if (!frames.has_value()) {
        return frames.error();
    }
    return frames.value();
}

void DisplayController::shutdown() noexcept {
    if (!impl_) {
        return;
    }
    if (impl_->state == DisplayState::projecting) {
        static_cast<void>(impl_->control.stop_projection());
        impl_->state = DisplayState::idle;
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
