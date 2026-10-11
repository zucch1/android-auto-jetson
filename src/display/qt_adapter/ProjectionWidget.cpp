// SPDX-License-Identifier: GPL-3.0-or-later
#include "ProjectionWidget.hpp"

#include <QKeyEvent>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>

#include <algorithm>

namespace aa::display::qt_adapter {

namespace {

// Android keycodes (the todo-26 input shape's keycode space) for the keyboard
// and D-pad keys the dummy display normalizes; unmapped Qt keys are dropped.
[[nodiscard]] std::int32_t android_keycode(int qt_key) noexcept {
    switch (qt_key) {
    case Qt::Key_Up: return 19;
    case Qt::Key_Down: return 20;
    case Qt::Key_Left: return 21;
    case Qt::Key_Right: return 22;
    case Qt::Key_Return:
    case Qt::Key_Enter: return 23;
    case Qt::Key_Backspace: return 4;
    case Qt::Key_Escape: return 111;
    case Qt::Key_Space: return 62;
    default: break;
    }
    if (qt_key >= Qt::Key_0 && qt_key <= Qt::Key_9) {
        return 7 + (qt_key - Qt::Key_0);
    }
    if (qt_key >= Qt::Key_A && qt_key <= Qt::Key_Z) {
        return 29 + (qt_key - Qt::Key_A);
    }
    return 0;
}

[[nodiscard]] std::uint8_t clamp_channel(int value) noexcept {
    return static_cast<std::uint8_t>(std::clamp(value, 0, 255));
}

// I420 (tight) -> RGB32, full-range BT.601 approximation: smoke-render
// quality only, deterministic, no external converter dependency.
[[nodiscard]] QImage i420_to_image(const consumer::DecodedFrame& frame) {
    const auto width = static_cast<int>(frame.width);
    const auto height = static_cast<int>(frame.height);
    const std::size_t y_bytes = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    if (width <= 0 || height <= 0 || frame.i420.size() < y_bytes + y_bytes / 2) {
        return {};
    }
    const std::uint8_t* y_plane = frame.i420.data();
    const std::uint8_t* u_plane = y_plane + y_bytes;
    const std::uint8_t* v_plane = u_plane + y_bytes / 4;
    QImage image(width, height, QImage::Format_RGB32);
    const int half_width = width / 2;
    for (int row = 0; row < height; ++row) {
        auto* line = reinterpret_cast<QRgb*>(image.scanLine(row));
        for (int column = 0; column < width; ++column) {
            const int y = static_cast<int>(y_plane[static_cast<std::size_t>(row * width + column)]);
            const int u = static_cast<int>(u_plane[static_cast<std::size_t>(
                                      (row / 2) * half_width + column / 2)]) -
                          128;
            const int v = static_cast<int>(v_plane[static_cast<std::size_t>(
                                      (row / 2) * half_width + column / 2)]) -
                          128;
            line[column] = qRgb(clamp_channel(y + ((359 * v) >> 8)),
                                clamp_channel(y - ((88 * u + 183 * v) >> 8)),
                                clamp_channel(y + ((454 * u) >> 8)));
        }
    }
    return image;
}

} // namespace

ProjectionWidget::ProjectionWidget(InputSink& input, protocol::VideoProfile caps, QWidget* parent)
    : QWidget(parent), input_(input), width_(caps.width), height_(caps.height) {
    setFixedSize(static_cast<int>(caps.width), static_cast<int>(caps.height));
}

void ProjectionWidget::on_frame(const consumer::DecodedFrame& frame) {
    if (frame.width != width_ || frame.height != height_) {
        return;
    }
    pending_ = frame.i420;
    frame_ = QImage{};
    update();
}

void ProjectionWidget::set_status(DisplayState state) {
    const auto text = to_string(state);
    status_ = QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
    update();
}

void ProjectionWidget::set_error(std::string_view message) {
    error_ = QString::fromUtf8(message.data(), static_cast<qsizetype>(message.size()));
    update();
}

QString ProjectionWidget::error_text() const { return error_; }

void ProjectionWidget::set_projection_control(ProjectionControl* control) noexcept {
    control_ = control;
}

QString ProjectionWidget::status_text() const { return status_; }

std::uint64_t ProjectionWidget::frames_painted() const noexcept { return frames_painted_; }

void ProjectionWidget::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    if (frame_.isNull() && !pending_.empty()) {
        consumer::DecodedFrame frame{};
        frame.width = width_;
        frame.height = height_;
        frame.i420 = pending_;
        frame_ = i420_to_image(frame);
    }
    if (frame_.isNull()) {
        painter.fillRect(rect(), QColor(24, 24, 24));
    } else {
        painter.drawImage(rect(), frame_);
        ++frames_painted_;
    }
    painter.setPen(Qt::white);
    painter.drawText(rect().adjusted(6, 4, -6, -4), Qt::AlignLeft | Qt::AlignTop, status_);
    if (!error_.isEmpty()) {
        painter.setPen(Qt::red);
        painter.drawText(rect().adjusted(6, 4, -6, -4), Qt::AlignLeft | Qt::AlignBottom, error_);
    }
}

bool ProjectionWidget::handle_control_key(QKeyEvent* event) {
    if (control_ == nullptr) {
        return false;
    }
    if (event->key() == Qt::Key_F5 && event->type() == QEvent::KeyPress) {
        control_->on_start_projection();
        return true;
    }
    if (event->key() == Qt::Key_F6 && event->type() == QEvent::KeyPress) {
        control_->on_stop_projection();
        return true;
    }
    return false;
}

void ProjectionWidget::keyPressEvent(QKeyEvent* event) {
    if (handle_control_key(event)) {
        event->accept();
        return;
    }
    deliver_key(event, true);
    event->accept();
}

void ProjectionWidget::keyReleaseEvent(QKeyEvent* event) {
    if (handle_control_key(event)) {
        event->accept();
        return;
    }
    deliver_key(event, false);
    event->accept();
}

void ProjectionWidget::mousePressEvent(QMouseEvent* event) {
    deliver_touch(event, TouchAction::down);
    event->accept();
}

void ProjectionWidget::mouseMoveEvent(QMouseEvent* event) {
    deliver_touch(event, TouchAction::move);
    event->accept();
}

void ProjectionWidget::mouseReleaseEvent(QMouseEvent* event) {
    deliver_touch(event, TouchAction::up);
    event->accept();
}

void ProjectionWidget::deliver_key(QKeyEvent* event, bool pressed) {
    const std::int32_t keycode = android_keycode(event->key());
    if (keycode == 0) {
        return;
    }
    input_.on_input(KeyEvent{keycode, pressed});
}

void ProjectionWidget::deliver_touch(QMouseEvent* event, TouchAction action) {
    const QPointF position = event->position();
    const auto x = static_cast<std::uint32_t>(
        std::clamp(static_cast<int>(position.x()), 0, static_cast<int>(width_) - 1));
    const auto y = static_cast<std::uint32_t>(
        std::clamp(static_cast<int>(position.y()), 0, static_cast<int>(height_) - 1));
    input_.on_input(TouchEvent{x, y, action});
}

} // namespace aa::display::qt_adapter
