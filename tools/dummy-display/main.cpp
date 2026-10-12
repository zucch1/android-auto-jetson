// SPDX-License-Identifier: GPL-3.0-or-later
// Dummy display application (task 24): a Qt 6 test consumer for the REAL
// todo-21 session D-Bus contract. TEST-ONLY autostart is available solely via
// the explicit --test-only-autostart flag (aa::display::TestOnlyAutostart);
// this application NEVER installs or implies a production autostart entry
// (no systemd user unit, no XDG/desktop autostart) - the production UI
// handoff is todo 44. Input events travel the owner-ruled input socket
// (task 24 F3, execution-input-transport-decision.json) to the receiver's
// endpoint; the D-Bus contract stays control-only.
#include <aa/display/Display.hpp>
#include <aa/ipc/InputChannel.hpp>

#include "ProjectionWidget.hpp"
#include "test_control.hpp"

#include <QApplication>
#include <QTimer>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <optional>
#include <string>

namespace {

using aa::display::DisplayState;

// Display-side control wiring (task 24 F2): the widget's F5/F6 shortcuts
// reach DisplayController::start_projection/stop_projection through this
// seam, so interactive start/stop travels the same real contract path as the
// test-only autostart. Status text is refreshed from the controller state.
class ControlWiring final : public aa::display::ProjectionControl {
public:
    ControlWiring(std::optional<aa::display::DisplayController>& controller,
                  aa::ipc::VideoSocketPath video, aa::display::qt_adapter::ProjectionWidget& widget)
        : controller_(controller), video_(std::move(video)), widget_(widget) {}

    void on_start_projection() override {
        if (!controller_.has_value()) {
            return;
        }
        report("StartProjection", controller_->start_projection(video_));
    }

    void on_stop_projection() override {
        if (!controller_.has_value()) {
            return;
        }
        report("StopProjection", controller_->stop_projection());
    }

private:
    // Captures the result, refreshes the state text and reports a typed
    // failure ("<state>: <Operation> failed (<code>)") on stderr and through
    // the widget error setter; a successful recovery clears the error while
    // the success-state text stays owned by set_status().
    void report(const char* operation, const aa::core::Result<void>& result) {
        widget_.set_status(controller_->state());
        if (result.has_value()) {
            widget_.set_error(std::string_view{});
            return;
        }
        const auto state = aa::display::to_string(controller_->state());
        const auto code = aa::to_string(result.error().code());
        std::string message{};
        message.reserve(state.size() + code.size() + 32);
        message.append(state).append(": ").append(operation).append(" failed (");
        message.append(code).append(")");
        std::fputs(message.c_str(), stderr);
        std::fputc('\n', stderr);
        widget_.set_error(message);
    }

    std::optional<aa::display::DisplayController>& controller_;
    aa::ipc::VideoSocketPath video_;
    aa::display::qt_adapter::ProjectionWidget& widget_;
};

std::optional<aa::display::DisplayController> g_controller{};
aa::ipc::InputSender* g_sender{nullptr};
aa::display::qt_adapter::ProjectionWidget* g_widget{nullptr};

aa::display::testcontrol::Status probe_status() {
    aa::display::testcontrol::Status status{};
    if (!g_controller.has_value() || g_widget == nullptr) {
        return status;
    }
    status.state = g_controller->state();
    status.frames_presented = g_controller->frames_presented();
    status.frames_painted = g_widget->frames_painted();
    status.helper_started = g_controller->helper_started();
    if (g_sender != nullptr) {
        status.input_sent = g_sender->sent();
        status.input_dropped = g_sender->dropped();
        const auto failure = g_sender->last_failure();
        if (failure.has_value()) {
            const auto name = aa::to_string(failure.value());
            status.input_last_error.assign(name.data(), name.size());
        }
    }
    const auto error = g_widget->error_text().toStdString();
    status.last_error = error;
    const QPixmap probe = g_widget->grab();
    status.render_ok = !probe.isNull();
    status.render_width = probe.width();
    status.render_height = probe.height();
    return status;
}

} // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);

    bool test_autostart = false;
    bool test_control_stdin = false;
    std::string video_socket =
        aa::ipc::production_video_socket_path().value;
    std::string input_socket =
        aa::ipc::production_input_socket_path().value;
    for (int index = 1; index < argc; ++index) {
        if (std::strcmp(argv[index], "--test-only-autostart") == 0) {
            test_autostart = true;
        } else if (std::strcmp(argv[index], "--test-only-control-stdin") == 0) {
            test_control_stdin = true;
        } else if (std::strcmp(argv[index], "--video-socket") == 0 && index + 1 < argc) {
            video_socket = argv[++index];
        } else if (std::strcmp(argv[index], "--input-socket") == 0 && index + 1 < argc) {
            input_socket = argv[++index];
        } else {
            return 2;
        }
    }

    aa::ipc::InputSender sender{};
    sender.attach(aa::ipc::InputSocketPath{input_socket}, aa::ipc::current_uid());
    g_sender = &sender;

    aa::display::DisplayConfig config{};
    config.consumer_name = aa::ipc::BusName{"org.custom.DummyDisplay1"};
    config.viewport = aa::ipc::Viewport{1280, 720, 160};
    config.caps = aa::protocol::VideoProfile{1280, 720, 30};

    aa::display::qt_adapter::ProjectionWidget widget(sender, config.caps);
    g_widget = &widget;
    widget.set_status(DisplayState::unavailable);

    auto client = aa::ipc::ControlBusClient::connect();
    if (client.has_value()) {
        aa::display::TestOnlyAutostart gate{};
        gate.marker = test_autostart ? aa::display::TestOnlyAutostart::kMarker : std::string_view{};
        gate.video_socket = aa::ipc::VideoSocketPath{video_socket};
        aa::display::DisplayController::Deps deps{client.value(), aa::display::helper_decode_sessions(),
                                                  widget};
        auto attached = test_autostart
                            ? aa::display::attach_test_only_autostart(gate, deps, config)
                            : aa::display::DisplayController::attach(std::move(deps), config);
        if (attached.has_value()) {
            widget.set_status(attached.value().state());
            g_controller.emplace(std::move(attached.value()));
        }
    }

    ControlWiring controls{g_controller, aa::ipc::VideoSocketPath{video_socket}, widget};
    widget.set_projection_control(&controls);

    // Pump timer must outlive app.exec(): declared at main scope, not inside
    // the attach block (task 24 F1 - a timer destroyed before the event loop
    // starts never pumps video into the helper or presents frames).
    auto pump_timer = std::make_unique<QTimer>();
    QObject::connect(pump_timer.get(), &QTimer::timeout, [] {
        if (g_controller.has_value()) {
            static_cast<void>(g_controller->pump(aa::core::Milliseconds{0}));
        }
    });
    if (g_controller.has_value()) {
        pump_timer->start(30);
    }

    std::unique_ptr<aa::display::testcontrol::Channel> control{};
    if (test_control_stdin) {
        control = std::make_unique<aa::display::testcontrol::Channel>(widget, probe_status);
    }

    widget.show();
    const int outcome = app.exec();
    if (g_controller.has_value()) {
        g_controller->shutdown();
    }
    return outcome;
}
