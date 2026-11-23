// SPDX-License-Identifier: GPL-3.0-or-later
#include "pipeline_options.hpp"
#include "../json_stdout.hpp"
#include "../video_capabilities.hpp"
#include <aa/config/VideoProfiles.hpp>
// Dummy display application (task 24): a Qt 6 test consumer for the REAL
// todo-21 session D-Bus contract. TEST-ONLY autostart is available solely via
// the explicit --test-only-autostart flag (aa::display::TestOnlyAutostart);
// this application NEVER installs or implies a production autostart entry
// (no systemd user unit, no XDG/desktop autostart) - the production UI
// handoff is todo 44. Input events travel the owner-ruled input socket
// (task 24 F3, execution-input-transport-decision.json) to the receiver's
// endpoint; the D-Bus contract stays control-only.
#include <aa/display/Display.hpp>
#include <aa/display/TelemetryWriter.hpp>
#include <aa/ipc/InputChannel.hpp>

#include "ProjectionWidget.hpp"
#include "decode_loss.hpp"
#include "pump_timer.hpp"
#include "shutdown_outcome.hpp"
#include "signal_quit.hpp"
#include "test_control.hpp"

#include <QApplication>
#include <QTimer>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace {

using aa::display::DisplayState;

// display_error lines go through the telemetry writer once it is attached, so
// runtime error reporting on the GUI thread never waits on stdout either.
class ErrorReporter final {
public:
    void attach(aa::display::TelemetryWriter* writer) noexcept { writer_ = writer; }

    void begin_attempt() {
        emitted_ = 0;
        last_.reset();
    }

    void report(std::string_view stage, aa::ErrorCode code) {
        const auto pair = std::pair{stage, code};
        if (emitted_ == 4 || last_ == pair) {
            return;
        }
        ++emitted_;
        last_ = pair;
        const auto code_str = aa::to_string(code);
        std::string line{};
        line.reserve(64 + stage.size() + code_str.size());
        line.append("{\"event\":\"display_error\",\"stage\":\"");
        line.append(stage);
        line.append("\",\"code\":\"");
        line.append(code_str);
        line.append("\"}");
        if (writer_ != nullptr) {
            static_cast<void>(writer_->enqueue(std::move(line)));
            return;
        }
        line.push_back('\n');
        static_cast<void>(aa::tools::write_all(aa::tools::json_fd(), line));
    }

private:
    aa::display::TelemetryWriter* writer_{nullptr};
    unsigned emitted_{0};
    std::optional<std::pair<std::string_view, aa::ErrorCode>> last_{};
};

// Display-side control wiring (task 24 F2): the widget's F5/F6 shortcuts
// reach DisplayController::start_projection/stop_projection through this
// seam, so interactive start/stop travels the same real contract path as the
// test-only autostart. Status text is refreshed from the controller state.
class ControlWiring final : public aa::display::ProjectionControl {
public:
    ControlWiring(std::optional<aa::display::DisplayController>& controller,
                  aa::ipc::VideoSocketPath video, aa::display::qt_adapter::ProjectionWidget& widget,
                  ErrorReporter& errors, aa::display::TelemetryWriter& diagnostics)
        : controller_(controller), video_(std::move(video)), widget_(widget), errors_(errors),
          diagnostics_(diagnostics) {}

    void on_start_projection() override {
        report("StartProjection", controller_ ? controller_->start_projection(video_)
                                               : unavailable());
    }

    void on_stop_projection() override {
        report("StopProjection", controller_ ? controller_->stop_projection() : unavailable());
    }

private:
    static aa::core::Result<void> unavailable() {
        return aa::Error{aa::ErrorCode::session_illegal_transition};
    }

    // Captures the result, refreshes the state text and reports a typed
    // failure ("<state>: <Operation> failed (<code>)") on stderr and through
    // the widget error setter; a successful recovery clears the error while
    // the success-state text stays owned by set_status(). The stderr line is
    // queued on its own writer: this runs on the GUI thread.
    void report(const char* operation, const aa::core::Result<void>& result) {
        const auto display_state = controller_ ? controller_->state() : DisplayState::unavailable;
        widget_.set_status(display_state);
        if (result.has_value()) {
            widget_.set_error(std::string_view{});
            return;
        }
        errors_.report(std::strcmp(operation, "StartProjection") == 0 ? "start" : "stop",
                       result.error().code());
        const auto state = aa::display::to_string(display_state);
        const auto code = aa::to_string(result.error().code());
        std::string message{};
        message.reserve(state.size() + code.size() + 32);
        message.append(state).append(": ").append(operation).append(" failed (");
        message.append(code).append(")");
        static_cast<void>(diagnostics_.enqueue(message));
        widget_.set_error(message);
    }

    std::optional<aa::display::DisplayController>& controller_;
    aa::ipc::VideoSocketPath video_;
    aa::display::qt_adapter::ProjectionWidget& widget_;
    ErrorReporter& errors_;
    aa::display::TelemetryWriter& diagnostics_;
};

aa::display::testcontrol::Status probe_status(
    const std::optional<aa::display::DisplayController>& controller,
    aa::display::qt_adapter::ProjectionWidget& widget, const aa::ipc::InputSender& sender,
    std::uint64_t previous_frames) {
    aa::display::testcontrol::Status status{};
    status.frames_presented = previous_frames;
    status.frames_painted = widget.frames_painted();
    if (controller.has_value()) {
        status.state = controller->state();
        status.frames_presented += controller->frames_presented();
        status.helper_started = controller->helper_started();
        const auto& decode = controller->decode_report();
        status.frames_lost_retention = decode.frames_lost_retention;
        status.frames_lost_capacity = decode.frames_lost_capacity;
        status.frames_lost_eos = decode.frames_lost_eos;
        status.frames_lost_close = decode.frames_lost_close;
        status.frames_discarded_awaiting_idr = decode.frames_discarded_awaiting_idr;
        status.awaiting_idr = decode.awaiting_idr;
    }
    status.input_sent = sender.sent();
    status.input_dropped = sender.dropped();
    if (const auto failure = sender.last_failure()) {
        status.input_last_error = aa::to_string(*failure);
    }
    status.last_error = widget.error_text().toStdString();
    const QPixmap probe = widget.grab();
    status.render_ok = !probe.isNull();
    status.render_width = probe.width();
    status.render_height = probe.height();
    return status;
}

constexpr const char* kUsage =
    "usage: aa-dummy-display [--video-socket PATH] [--input-socket PATH]\n"
               "                        [--decoder software|hardware|auto] [--presentation cpu|gpu|zerocopy]\n"
               "                        [--media-thread gui|worker] [--check-pipeline] [--list-video-capabilities]\n"
               "                        [--decoder-backend software|nvidia|automatic (legacy flag)]\n"
               "                        [--video-profiles WxH@FPS:CODEC,...]\n"
               "                        [--test-only-autostart] [--test-only-control-stdin]\n"
               "\n"
               "TEST-ONLY Android Auto projection consumer.\n"
               "  F5 start projection   F6 stop projection\n"
               "  F1 toggle HUD         F11 toggle full screen\n"
               "  arrows/enter/letters  forwarded as keys; left mouse button = touch\n";

void print_usage(std::FILE* stream) { std::fputs(kUsage, stream); }

} // namespace

// Runs the display; main() owns the stdout isolation and the process exit.
int display_main(int argc, char** argv) {
    const int json = aa::tools::json_fd();
    auto errors = std::make_shared<ErrorReporter>();

    bool test_autostart = false;
    bool test_control_stdin = false;
    std::string video_socket =
        aa::ipc::production_video_socket_path().value;
    std::string input_socket =
        aa::ipc::production_input_socket_path().value;
    aa::display::PipelineOptions pipeline;
    bool check_only = false;
    auto video_profiles = aa::config::default_video_profiles();
    for (int index = 1; index < argc; ++index) {
        if (std::strcmp(argv[index], "--test-only-autostart") == 0) {
            test_autostart = true;
        } else if (std::strcmp(argv[index], "--test-only-control-stdin") == 0) {
            test_control_stdin = true;
        } else if (std::strcmp(argv[index], "--video-socket") == 0 && index + 1 < argc) {
            video_socket = argv[++index];
        } else if (std::strcmp(argv[index], "--input-socket") == 0 && index + 1 < argc) {
            input_socket = argv[++index];
        } else if ((std::strcmp(argv[index], "--decoder-backend") == 0
                    || std::strcmp(argv[index], "--decoder") == 0
                    || std::strcmp(argv[index], "--presentation") == 0
                    || std::strcmp(argv[index], "--media-thread") == 0) && index + 1 < argc) {
            const std::string_view option{argv[index]};
            if (!aa::display::parse_pipeline_option(pipeline, option, argv[++index])) {
                std::fputs("aa-dummy-display: config_invalid: invalid pipeline option value\n", stderr);
                print_usage(stderr);
                return 2;
            }
        } else if (std::strcmp(argv[index], "--list-video-capabilities") == 0) {
            return aa::tools::write_all(json, aa::tools::list_video_capabilities()) ? 0 : 1;
        } else if (std::strcmp(argv[index], "--video-profiles") == 0 && index + 1 < argc) {
            auto parsed = aa::config::parse_video_profiles(argv[++index]);
            if (!parsed) { std::fputs("config_invalid: invalid --video-profiles\n",stderr); return 2; }
            video_profiles = std::move(parsed.value());
        } else if (std::strcmp(argv[index], "--check-pipeline") == 0) {
            check_only = true;
        } else if (std::strcmp(argv[index], "--help") == 0 || std::strcmp(argv[index], "-h") == 0) {
            return aa::tools::write_all(json, kUsage) ? 0 : 1;
        } else {
            std::fprintf(stderr, "aa-dummy-display: unrecognized argument '%s'\n", argv[index]);
            print_usage(stderr);
            return 2;
        }
    }

    const auto decoder_backend = pipeline.decoder == aa::display::Decoder::hardware
        ? aa::consumer::DecodeBackend::nvidia : pipeline.decoder == aa::display::Decoder::automatic
        ? aa::consumer::DecodeBackend::automatic : aa::consumer::DecodeBackend::software;
    std::vector<std::pair<aa::protocol::VideoCodec, aa::consumer::DecodeReport>> codec_reports;
    if (pipeline.legacy_decoder_backend) {
        const auto codec = video_profiles.front().codec;
        codec_reports.emplace_back(codec, aa::consumer::DecodeHelper::probe_backends(codec));
    } else {
        for (const auto& profile : video_profiles) {
            if (std::none_of(codec_reports.begin(), codec_reports.end(),
                    [&](const auto& item) { return item.first == profile.codec; })) {
                codec_reports.emplace_back(profile.codec, aa::consumer::DecodeHelper::probe_backends(profile.codec));
            }
        }
    }
    if (const auto refusal = aa::display::check_pipeline(pipeline, codec_reports)) {
        std::fprintf(stderr,"%.*s: %.*s\n",static_cast<int>(aa::to_string(refusal->code).size()),aa::to_string(refusal->code).data(),
            static_cast<int>(refusal->message.size()),refusal->message.data()); return 2;
    }
    if (check_only) { return aa::tools::write_all(json, "pipeline_available\n") ? 0 : 1; }
    QApplication app(argc, argv);
    // SIGTERM/SIGINT quit the event loop, so the telemetry log is drained and
    // proven complete on a normal runner stop (#90 review 4).
    const auto quit_on_signal = aa::display::signal_quit::install();
    aa::ipc::InputSender sender{};
    sender.attach(aa::ipc::InputSocketPath{input_socket}, aa::ipc::current_uid());

    aa::display::DisplayConfig config{};
    config.consumer_name = aa::ipc::BusName{"org.custom.DummyDisplay1"};
    config.viewport = aa::ipc::Viewport{1280, 720, 160};
    config.caps = video_profiles.front();
    // Endpoint, decode and presentation telemetry is written by a background
    // thread: a slow stdout write must never stall the GUI thread that pumps,
    // decodes and paints (a stalled GUI thread shows up as a frozen display).
    // Raw write(2) on the JSON descriptor: no FILE lock, so a worker that
    // close() abandons on a stalled stdout cannot stall the process exit.
    aa::display::TelemetryWriter telemetry{[json](const std::string& line) {
        return aa::tools::write_all(json, line);
    }};
    // Gate consumers require this last line as proof the telemetry log is complete.
    telemetry.set_completion_record("{\"event\":\"telemetry_complete\"}");
    errors->attach(&telemetry);
    aa::display::TelemetryWriter diagnostics{aa::display::default_stderr_sink, 64};
    config.on_video_endpoint = [&telemetry](const aa::ipc::VideoEndpoint& event) {
        static_cast<void>(telemetry.enqueue(aa::ipc::video_endpoint_json(event)));
    };
    config.on_decode_completion = [&telemetry](const aa::consumer::DecodeCompletion& event) {
        static_cast<void>(telemetry.enqueue(aa::consumer::decode_completion_json(event)));
    };
    config.on_diagnostic = [&telemetry](std::string line) {
        // Emission stamp at the source (CLOCK_MONOTONIC), as on the receiver.
        if (line.size() > 2 && line.front() == '{' && line.back() == '}') {
            const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
            line.pop_back();
            line += ",\"emit_ns\":" + std::to_string(ns) + "}";
        }
        static_cast<void>(telemetry.enqueue(std::move(line)));
    };

    aa::display::qt_adapter::ProjectionWidget widget(sender, config.caps);
    widget.set_input_viewport(static_cast<std::uint16_t>(config.viewport.width),static_cast<std::uint16_t>(config.viewport.height));
    widget.set_telemetry_writer(&telemetry);
    widget.set_status(DisplayState::unavailable);

    aa::display::DecodeLossTelemetry decode_loss;
    auto factory = [errors, decoder_backend, &decode_loss, &telemetry,
                    inner = aa::display::helper_decode_sessions()](
                       const aa::consumer::DecodeConfig& decode_config)
        -> aa::core::Result<std::unique_ptr<aa::display::DecodeSession>> {
        errors->begin_attempt();
        auto effective_config = decode_config;
        effective_config.backend = decoder_backend;
        auto opened = inner(effective_config);
        if (!opened.has_value()) {
            return opened;
        }
        for (auto& line : decode_loss.begin_session()) {
            static_cast<void>(telemetry.enqueue(std::move(line)));
        }
        return std::unique_ptr<aa::display::DecodeSession>(
            std::make_unique<aa::display::LossReportingDecodeSession>(
                std::move(opened.value()), decode_loss,
                [&telemetry](std::string line) {
                    static_cast<void>(telemetry.enqueue(std::move(line)));
                }));
    };

    const aa::ipc::VideoSocketPath video_path{video_socket};
    const aa::display::TestOnlyAutostart gate{aa::display::TestOnlyAutostart::kMarker, video_path};
    std::optional<aa::ipc::ControlBusClient> watcher;
    std::optional<aa::ipc::ControlBusClient> client;
    std::optional<aa::display::DisplayController> controller;
    std::uint64_t previous_frames = 0;
    const auto poll_receiver = [&] {
        if (!watcher.has_value()) {
            auto connected = aa::ipc::ControlBusClient::connect();
            if (!connected.has_value()) {
                errors->report("attach", connected.error().code());
                return;
            }
            watcher.emplace(std::move(connected.value()));
        }
        // Keep the owner's unique name on the controller's client pinned for
        // the lifetime of the registration, including during control calls.
        const auto owner = watcher->receiver_owner();
        if (controller.has_value()) {
            if (owner && owner.value() && controller->attached_to(*owner.value())) {
                return;
            }
            previous_frames += controller->frames_presented();
            controller->receiver_lost();
            controller.reset();
            client.reset();
            widget.set_status(DisplayState::unavailable);
            widget.set_error({});
            return;
        }
        if (!owner || !owner.value()) {
            return;
        }
        auto connected = aa::ipc::ControlBusClient::connect();
        if (!connected.has_value()) {
            errors->report("attach", connected.error().code());
            return;
        }
        client.emplace(std::move(connected.value()));
        aa::display::DisplayController::Deps deps{*client, factory, widget};
        auto attached = test_autostart
                            ? aa::display::attach_test_only_autostart(gate, std::move(deps), config)
                            : aa::display::DisplayController::attach(std::move(deps), config);
        if (!attached.has_value()) {
            errors->report(test_autostart ? "autostart" : "attach", attached.error().code());
            client.reset();
            return;
        }
        if (attached.value().state() == DisplayState::unavailable) {
            // The receiver vanished between the poll and the attach lookup.
            return;
        }
        controller.emplace(std::move(attached.value()));
        widget.set_status(controller->state());
        widget.set_error({});
    };
    // Rearm after each poll so a delayed GUI tick cannot bunch up attempts.
    QTimer receiver_timer;
    receiver_timer.setTimerType(Qt::PreciseTimer);
    receiver_timer.setSingleShot(true);
    receiver_timer.setInterval(std::chrono::seconds{1});
    QObject::connect(&receiver_timer, &QTimer::timeout, [&] {
        poll_receiver();
        receiver_timer.start();
    });
    poll_receiver();
    receiver_timer.start();

    ControlWiring controls{controller, video_path, widget, *errors, diagnostics};
    widget.set_projection_control(&controls);

    // Pump timer must outlive app.exec(): declared at main scope, not inside
    // the attach block (task 24 F1 - a timer destroyed before the event loop
    // starts never pumps video into the helper or presents frames).
    auto pump_timer = std::make_unique<QTimer>();
    aa::display::pump::configure(*pump_timer);
    QObject::connect(pump_timer.get(), &QTimer::timeout,
                     [errors, &telemetry, &widget, &decode_loss, &controller,
                      telemetry_reported = false]() mutable {
        // A full queue or failed write loses measurement events: show it once.
        // Only on screen: a stream write here could block on the same stdout
        // that failed the writer, freezing the GUI thread it protects.
        if (!telemetry_reported && telemetry.failed()) {
            telemetry_reported = true;
            widget.set_error("telemetry failure: events undelivered");
        }
        if (controller.has_value()) {
            const auto pumped = controller->pump(aa::core::Milliseconds{0});
            if (!pumped.has_value()) {
                const auto code = pumped.error().code();
                if (code != aa::ErrorCode::timeout &&
                    code != aa::ErrorCode::transport_queue_full &&
                    code != aa::ErrorCode::session_illegal_transition) {
                    errors->report("pump", code);
                }
            }
            for (auto& line : decode_loss.observe(controller->decode_report(),
                                                  std::chrono::steady_clock::now())) {
                static_cast<void>(telemetry.enqueue(std::move(line)));
            }
        }
    });
    pump_timer->start();

    std::unique_ptr<aa::display::testcontrol::Channel> control{};
    if (test_control_stdin) {
        control = std::make_unique<aa::display::testcontrol::Channel>(
            widget, [&] { return probe_status(controller, widget, sender, previous_frames); },
            &telemetry);
    }

    widget.show();
    int outcome = app.exec();
    receiver_timer.stop();
    pump_timer->stop();
    if (controller.has_value()) {
        // Totals coalesced by the rate limit must not die with the session.
        for (auto& line : decode_loss.observe(controller->decode_report(),
                                              std::chrono::steady_clock::now(), true)) {
            static_cast<void>(telemetry.enqueue(std::move(line)));
        }
        controller->shutdown();
        controller.reset();
    }
    widget.set_telemetry_writer(nullptr);
    errors->attach(nullptr);
    return aa::display::shutdown::finish(outcome, diagnostics, telemetry,
                                         [](const std::string& line) {
                                             std::fputs(line.c_str(), stderr);
                                         });
}

int main(int argc, char** argv) {
    // Before any GStreamer/NVIDIA probe: JSON moves to a private descriptor and
    // fd 1 becomes /dev/null; the run ends with _Exit (tools/json_stdout.hpp).
    aa::tools::StdoutIsolation isolation;
    if (!isolation.ok()) {
        isolation.report_failure("aa-dummy-display");
        aa::tools::StdoutIsolation::end_process(aa::tools::kStdoutIsolationFailed);
    }
    aa::tools::StdoutIsolation::end_process(display_main(argc, argv));
}
