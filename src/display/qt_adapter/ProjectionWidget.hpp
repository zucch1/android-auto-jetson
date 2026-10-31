// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <aa/display/Display.hpp>

#include <QImage>
#include <QRect>
#include <QString>
#include <QWidget>

#include <QTimer>

#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

class QPainter;

namespace aa::display::qt_adapter {

// Presentation-rate meter for the HUD: counts presentations over >= 1 s
// windows; a reading expires once no presentation has arrived for kStaleNs,
// so a stalled stream never keeps showing its last rate.
class FpsMeter final {
public:
    static constexpr std::int64_t kWindowNs{1'000'000'000};
    static constexpr std::int64_t kStaleNs{2'000'000'000};

    void note(std::int64_t presented_ns) noexcept;
    [[nodiscard]] std::optional<double> reading(std::int64_t now_ns) const noexcept;
    void reset() noexcept;

private:
    std::int64_t window_start_ns_{};
    std::int64_t last_ns_{};
    std::uint32_t window_count_{};
    double fps_{};
};

// Qt rendering/input surface for the dummy display (task 24): presents I420
// frames decoded by the todo-23 helper and normalizes Qt keyboard/D-pad and
// mouse input into the todo-26 input seam. Qt includes live only in this
// adapter directory (architecture_boundary adapter zone).
class ProjectionWidget final : public QWidget, public FrameTarget {
public:
    explicit ProjectionWidget(InputSink& input, protocol::VideoProfile caps,
                              QWidget* parent = nullptr);

    void on_frame(const consumer::DecodedFrame& frame) override;

    void set_status(DisplayState state);
    [[nodiscard]] QString status_text() const;
    [[nodiscard]] std::uint64_t frames_painted() const noexcept;

    // Small error presentation seam (stop-recovery reporting): the wiring
    // passes the typed failure line (e.g.
    // "stop_failed: StopProjection failed (transport_io)") and clears it with
    // an empty message after successful recovery. The success-state text is
    // untouched - set_status() keeps owning it.
    void set_error(std::string_view message);
    [[nodiscard]] QString error_text() const;

    // Test-display controls (task 24 F2): F5 requests projection start and F6
    // requests projection stop through this seam. Null leaves the shortcuts
    // inert. The seam is invoked from the real key path only - it never
    // bypasses the D-Bus contract (the app's wiring calls the contract).
    void set_projection_control(ProjectionControl* control) noexcept;

    // Local view controls (never forwarded to the input seam): F1 toggles the
    // diagnostics HUD and F11 toggles full screen.
    [[nodiscard]] bool hud_visible() const noexcept;

    // Area the video occupies inside the widget: the largest rect with the
    // negotiated aspect ratio, centered (letterboxed). Equals rect() at the
    // native size; touch positions are mapped from this area into
    // absolute video pixels.
    [[nodiscard]] QRect video_rect() const;

protected:
    void paintEvent(QPaintEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void keyReleaseEvent(QKeyEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void focusOutEvent(QFocusEvent* event) override;
    void hideEvent(QHideEvent* event) override;

private:
    void deliver_key(QKeyEvent* event, bool pressed);
    void deliver_touch(QMouseEvent* event, TouchAction action);
    // Ends an in-flight gesture with an "up" at its last position so a lost
    // mouse release never leaves the phone (or this widget) stuck mid-touch.
    void cancel_touch();
    bool handle_control_key(QKeyEvent* event);
    bool handle_view_key(QKeyEvent* event);
    void paint_placeholder(QPainter& painter) const;
    void paint_hud(QPainter& painter);
    void paint_error(QPainter& painter) const;
    void expire_stale_fps();

    InputSink& input_;
    ProjectionControl* control_{nullptr};
    QImage frame_{};
    std::vector<std::uint8_t> pending_{};
    DisplayState state_{DisplayState::unavailable};
    QString status_{QStringLiteral("unavailable")};
    QString error_{};
    std::uint64_t frames_painted_{0};
    std::uint64_t presentations_{};
    std::uint64_t pending_frame_{};
    std::uint64_t pending_timestamp_ns_{};
    bool presentation_pending_{};
    bool hud_visible_{true};
    bool touch_active_{false};
    std::uint32_t touch_x_{};
    std::uint32_t touch_y_{};
    FpsMeter fps_meter_{};
    bool fps_shown_{false};
    QTimer fps_expiry_{};
    std::uint16_t width_{};
    std::uint16_t height_{};
    std::uint8_t fps_cap_{};
};

} // namespace aa::display::qt_adapter
