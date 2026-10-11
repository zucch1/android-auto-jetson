// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <aa/consumer/Decode.hpp>
#include <aa/core/Result.hpp>
#include <aa/core/Time.hpp>
#include <aa/ipc/ControlBusClient.hpp>
#include <aa/ipc/InputChannel.hpp>
#include <aa/ipc/InputSocket.hpp>
#include <aa/ipc/VideoChannel.hpp>
#include <aa/protocol/Protocol.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <string_view>
#include <variant>

namespace aa::display {

// Normalized input seam (the todo-26 input shape): keyboard/D-pad key events
// with Android keycodes and mouse-to-touch events in absolute viewport
// pixels. The canonical definitions live with the task-24 input transport
// (aa/ipc/InputSocket.hpp - see the owner ruling on input transport) because
// that is where todo 26's receiver-side endpoint consumes them; they are
// re-exported here so the display seam keeps one name and one concept. The
// Qt widget normalizes its input into this shape and delivers it to an
// InputSink (the shipped sink is the socket InputSender; in-memory sinks are
// test artifacts only). No vehicle sensor state is ever fabricated here.
using ipc::InputEvent;
using ipc::InputSink;
using ipc::KeyEvent;
using ipc::TouchEvent;
using ipc::TouchAction;

// Minimal test-display controls (task 24 F2): the dummy display's keyboard
// shortcuts request projection start/stop through this seam. The app wires it
// to the REAL contract methods on DisplayController (start_projection /
// stop_projection); nothing here bypasses the contract.
class ProjectionControl {
public:
    virtual ~ProjectionControl() = default;
    ProjectionControl() = default;
    ProjectionControl(const ProjectionControl&) = delete;
    ProjectionControl& operator=(const ProjectionControl&) = delete;
    ProjectionControl(ProjectionControl&&) = delete;
    ProjectionControl& operator=(ProjectionControl&&) = delete;

    virtual void on_start_projection() = 0;
    virtual void on_stop_projection() = 0;
};

// Display lifecycle as the dummy consumer shows it: no receiver -> unavailable
// (a shown state in which nothing is started; also the terminal state after a
// lost registration), registered -> idle, projecting -> projecting, and a
// failed StopProjection whose inactivity could NOT be confirmed -> stop_failed.
// stop_failed is a shown, recoverable state: the projection may still be
// active remotely, so a new start is prohibited (no second remote
// StartProjection) while a stop retry is allowed; recovery re-runs the stop
// and only a confirmed stop releases the media path and returns idle.
enum class DisplayState { unavailable, idle, projecting, stop_failed };

[[nodiscard]] constexpr std::string_view to_string(DisplayState state) noexcept {
    switch (state) {
    case DisplayState::unavailable: return "unavailable";
    case DisplayState::idle: return "idle";
    case DisplayState::projecting: return "projecting";
    case DisplayState::stop_failed: return "stop_failed";
    }
    return "unknown";
}

// Decoded-frame presentation seam (the Qt widget implements it).
class FrameTarget {
public:
    virtual ~FrameTarget() = default;
    FrameTarget() = default;
    FrameTarget(const FrameTarget&) = delete;
    FrameTarget& operator=(const FrameTarget&) = delete;
    FrameTarget(FrameTarget&&) = delete;
    FrameTarget& operator=(FrameTarget&&) = delete;

    virtual void on_frame(const consumer::DecodedFrame& frame) = 0;
};

// Decode-session seam: production wraps aa::consumer::DecodeHelper; tests
// inject fakes so "the helper was never started" is assertable on the
// receiver-absent path. One thread at a time drives a session.
class DecodeSession {
public:
    virtual ~DecodeSession() = default;
    DecodeSession() = default;
    DecodeSession(const DecodeSession&) = delete;
    DecodeSession& operator=(const DecodeSession&) = delete;
    DecodeSession(DecodeSession&&) = delete;
    DecodeSession& operator=(DecodeSession&&) = delete;

    [[nodiscard]] virtual core::Result<void> submit(const ipc::VideoAccessUnit& unit) = 0;
    [[nodiscard]] virtual core::Result<void> end_of_stream() = 0;
    [[nodiscard]] virtual core::Result<std::size_t> pump(core::Milliseconds wait) = 0;
    virtual void close() noexcept = 0;
};

using DecodeSessionFactory =
    std::function<core::Result<std::unique_ptr<DecodeSession>>(const consumer::DecodeConfig&)>;

// The production factory: opens the real todo-23 consumer helper.
[[nodiscard]] DecodeSessionFactory helper_decode_sessions();

struct DisplayConfig final {
    ipc::BusName consumer_name{};
    ipc::Viewport viewport{};
    protocol::VideoProfile caps{1280, 720, 30};
};

// Dummy display consumer (task 24). Projection control ALWAYS travels the
// real todo-21 contract (RegisterConsumer / StartProjection / StopProjection /
// GetState / SetDisplayViewport through ControlBusClient); decoded frames come
// from the todo-22 video channel through the todo-23 helper; input events go
// to the todo-26 seam at the widget. Threading: caller-driven, one thread at a
// time per instance (the helper's pump-delivers-on-caller-thread rule).
class DisplayController final {
public:
    struct Deps final {
        ipc::ControlBusClient& control;
        DecodeSessionFactory decode_sessions{helper_decode_sessions()};
        FrameTarget& frames;
    };

    // Attaches through the REAL contract. Receiver absent -> the returned
    // controller is in state unavailable (a shown state, never an error) and
    // no decode helper, video channel or AP is ever started. Receiver present
    // -> RegisterConsumer + SetDisplayViewport -> state idle. Contract
    // rejection (e.g. an already-registered consumer) is a typed error.
    [[nodiscard]] static core::Result<DisplayController> attach(Deps deps, DisplayConfig config);

    ~DisplayController();
    DisplayController(const DisplayController&) = delete;
    DisplayController& operator=(const DisplayController&) = delete;
    DisplayController(DisplayController&& other) noexcept;
    DisplayController& operator=(DisplayController&& other) noexcept;

    [[nodiscard]] DisplayState state() const noexcept;
    [[nodiscard]] core::ConsumerId consumer() const noexcept;
    [[nodiscard]] std::uint64_t frames_presented() const noexcept;
    [[nodiscard]] bool helper_started() const noexcept;

    // StartProjection through the contract, then connect the todo-22 video
    // channel and open the todo-23 decode session. On any failure the
    // contract projection is rolled back and the typed ORIGINAL error is
    // returned (a failed rollback stop is reconciled through GetState and may
    // leave the controller in stop_failed / unavailable, never a false idle).
    // Only the idle state may start: a stop_failed controller is prohibited
    // from requesting a new remote StartProjection until a stop is confirmed.
    [[nodiscard]] core::Result<void> start_projection(const ipc::VideoSocketPath& video_socket);
    // StopProjection through the contract and release the video/decode path.
    // Accepted from projecting and stop_failed (the recovery retry). Success
    // releases the media and lands idle. Failure keeps the ORIGINAL error for
    // the caller and reconciles exactly once through get_state(): same
    // consumer stopped -> release media, idle (the failure is still reported);
    // still projecting or the reconciliation itself failed -> media retained,
    // stop_failed; different/zero consumer -> media released, consumer id
    // cleared, unavailable. The reconciliation bus exchange runs ONLY after a
    // stop failure (never per pump/tick).
    [[nodiscard]] core::Result<void> stop_projection();

    // Drains the video channel into the decode helper and presents decoded
    // frames to the FrameTarget; returns the frames presented this call.
    // Allowed in projecting and (media retained) stop_failed under the SAME
    // video/session preconditions; pump never creates a decoder or session.
    [[nodiscard]] core::Result<std::size_t> pump(core::Milliseconds wait);

    // Contract teardown (StopProjection + UnregisterConsumer, best effort)
    // plus helper/channel release; idempotent.
    void shutdown() noexcept;

    struct Impl;

private:
    explicit DisplayController(std::unique_ptr<Impl> impl) noexcept;
    std::unique_ptr<Impl> impl_;
};

// TEST-ONLY local autostart (NEVER production). The dummy display may come up
// without interactive steps ONLY through this gate; production autostart is
// out of scope (the future infotainment handoff is todo 44) and NOTHING in
// this repository installs a systemd, XDG or desktop autostart entry. The
// gate marker must match exactly or the call fails closed.
struct TestOnlyAutostart final {
    static constexpr std::string_view kMarker{"test-only"};
    std::string_view marker{};
    ipc::VideoSocketPath video_socket{};
};

[[nodiscard]] inline bool is_test_only(const TestOnlyAutostart& gate) noexcept {
    return gate.marker == TestOnlyAutostart::kMarker;
}

// TEST-ONLY end-to-end bring-up without interactive steps: attach, register,
// report viewport and request projection. Refuses with invalid_argument
// unless is_test_only(gate); never installs any autostart entry.
[[nodiscard]] core::Result<DisplayController> attach_test_only_autostart(
    const TestOnlyAutostart& gate, DisplayController::Deps deps, DisplayConfig config);

} // namespace aa::display
