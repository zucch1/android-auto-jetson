// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <aa/display/Display.hpp>

#include <QImage>
#include <QString>
#include <QWidget>

#include <cstdint>
#include <string_view>
#include <vector>

namespace aa::display::qt_adapter {

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

protected:
    void paintEvent(QPaintEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void keyReleaseEvent(QKeyEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;

private:
    void deliver_key(QKeyEvent* event, bool pressed);
    void deliver_touch(QMouseEvent* event, TouchAction action);
    bool handle_control_key(QKeyEvent* event);

    InputSink& input_;
    ProjectionControl* control_{nullptr};
    QImage frame_{};
    std::vector<std::uint8_t> pending_{};
    QString status_{QStringLiteral("unavailable")};
    QString error_{};
    std::uint64_t frames_painted_{0};
    std::uint16_t width_{};
    std::uint16_t height_{};
};

} // namespace aa::display::qt_adapter
