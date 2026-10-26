// SPDX-License-Identifier: GPL-3.0-or-later
#include "ProjectionWidget.hpp"

#include <QColor>
#include <QFont>
#include <QFontMetrics>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QPen>
#include <QRectF>

#include <algorithm>
#include <chrono>
#include <cstdio>

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

[[nodiscard]] int ui_unit(int widget_height) noexcept { return std::max(11, widget_height / 45); }

[[nodiscard]] QColor state_color(DisplayState state) noexcept {
    switch (state) {
    case DisplayState::unavailable: return {158, 158, 158};
    case DisplayState::idle: return {255, 179, 0};
    case DisplayState::projecting: return {67, 160, 71};
    case DisplayState::stop_failed: return {229, 57, 53};
    }
    return {158, 158, 158};
}

[[nodiscard]] QString state_hint(DisplayState state) {
    switch (state) {
    case DisplayState::unavailable:
        return QStringLiteral("Receiver not found on the session bus. Start the receiver, then relaunch.");
    case DisplayState::idle: return QStringLiteral("Press F5 to start projection");
    case DisplayState::projecting: return QStringLiteral("Waiting for video from the phone… (F6 stops)");
    case DisplayState::stop_failed: return QStringLiteral("Stop was not confirmed. Press F6 to retry.");
    }
    return {};
}

[[nodiscard]] std::int64_t steady_now_ns() noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

} // namespace

ProjectionWidget::ProjectionWidget(InputSink& input, protocol::VideoProfile caps, QWidget* parent)
    : QWidget(parent), input_(input), width_(caps.width), height_(caps.height), fps_cap_(caps.fps) {
    resize(static_cast<int>(caps.width), static_cast<int>(caps.height));
    setMinimumSize(std::max(1, static_cast<int>(caps.width) / 4),
                   std::max(1, static_cast<int>(caps.height) / 4));
    setAttribute(Qt::WA_OpaquePaintEvent);
    setFocusPolicy(Qt::StrongFocus);
    setCursor(Qt::CrossCursor);
}

void ProjectionWidget::on_frame(const consumer::DecodedFrame& frame) {
    if (frame.width != width_ || frame.height != height_) {
        return;
    }
    pending_ = frame.i420;
    pending_frame_ = frame.frame;
    pending_timestamp_ns_ = frame.timestamp_ns;
    presentation_pending_ = true;
    frame_ = QImage{};
    update();
}

void ProjectionWidget::set_status(DisplayState state) {
    state_ = state;
    const auto text = to_string(state);
    status_ = QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
    if (state == DisplayState::idle || state == DisplayState::unavailable) {
        pending_.clear();
        frame_ = QImage{};
        presentation_pending_ = false;
        fps_ = 0.0;
        fps_window_count_ = 0;
        fps_window_start_ns_ = 0;
    }
    if (isWindow()) {
        setWindowTitle(QStringLiteral("Android Auto dummy display (test only) - %1").arg(status_));
    }
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

bool ProjectionWidget::hud_visible() const noexcept { return hud_visible_; }

QRect ProjectionWidget::video_rect() const {
    if (width_ == 0 || height_ == 0) {
        return rect();
    }
    const QSize fitted = QSize(width_, height_).scaled(size(), Qt::KeepAspectRatio);
    return {QPoint((width() - fitted.width()) / 2, (height() - fitted.height()) / 2), fitted};
}

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
        paint_placeholder(painter);
    } else {
        const QRect area = video_rect();
        if (area != rect()) {
            painter.fillRect(rect(), Qt::black);
        }
        painter.drawImage(area, frame_);
        ++frames_painted_;
        if (hud_visible_) {
            paint_hud(painter);
        }
    }
    if (!error_.isEmpty()) {
        paint_error(painter);
    }
    painter.end();
    if (!frame_.isNull() && presentation_pending_) {
        const auto completed = steady_now_ns();
        note_presentation(completed);
        ++presentations_;
        std::printf("{\"event\":\"video_presented\",\"frame\":%llu,\"scheduled_ns\":%llu,"
                    "\"completed_ns\":%lld,\"count\":%llu}\n",
                    static_cast<unsigned long long>(pending_frame_),
                    static_cast<unsigned long long>(pending_timestamp_ns_),
                    static_cast<long long>(completed),
                    static_cast<unsigned long long>(presentations_));
        std::fflush(stdout);
        presentation_pending_ = false;
    }
}

void ProjectionWidget::note_presentation(std::int64_t completed_ns) {
    if (fps_window_start_ns_ == 0) {
        fps_window_start_ns_ = completed_ns;
        return;
    }
    ++fps_window_count_;
    const auto elapsed_ns = completed_ns - fps_window_start_ns_;
    if (elapsed_ns >= 1'000'000'000) {
        fps_ = static_cast<double>(fps_window_count_) * 1e9 / static_cast<double>(elapsed_ns);
        fps_window_start_ns_ = completed_ns;
        fps_window_count_ = 0;
    }
}

void ProjectionWidget::paint_placeholder(QPainter& painter) const {
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setRenderHint(QPainter::TextAntialiasing);
    painter.fillRect(rect(), QColor(12, 13, 15));
    const QRect area = video_rect();
    painter.fillRect(area, QColor(24, 26, 30));
    const int unit = ui_unit(area.height());

    QFont title_font = font();
    title_font.setPixelSize(unit * 3);
    title_font.setBold(true);
    QFont body_font = font();
    body_font.setPixelSize(unit);
    QFont pill_font = font();
    pill_font.setPixelSize(unit);
    pill_font.setBold(true);
    pill_font.setLetterSpacing(QFont::PercentageSpacing, 110);
    QFont footer_font = font();
    footer_font.setPixelSize(std::max(10, unit * 4 / 5));

    const QString title = QStringLiteral("Android Auto");
    const QString subtitle = QStringLiteral("Dummy projection display · test consumer only");
    const QString pill = status_.toUpper().replace(QLatin1Char('_'), QLatin1Char(' '));
    const QString hint = state_hint(state_);

    const QFontMetrics title_metrics(title_font);
    const QFontMetrics body_metrics(body_font);
    const QFontMetrics pill_metrics(pill_font);
    const int pill_height = pill_metrics.height() + unit;
    const int pill_width = pill_metrics.horizontalAdvance(pill) + unit * 3;
    const int total = title_metrics.height() + unit / 2 + body_metrics.height() + unit * 2 +
                      pill_height + unit + body_metrics.height();
    int y = area.center().y() - total / 2;

    painter.setFont(title_font);
    painter.setPen(QColor(240, 242, 245));
    painter.drawText(QRect(area.left(), y, area.width(), title_metrics.height()), Qt::AlignCenter,
                     title);
    y += title_metrics.height() + unit / 2;

    painter.setFont(body_font);
    painter.setPen(QColor(154, 160, 166));
    painter.drawText(QRect(area.left(), y, area.width(), body_metrics.height()), Qt::AlignCenter,
                     subtitle);
    y += body_metrics.height() + unit * 2;

    const QColor accent = state_color(state_);
    const QRect pill_rect(area.center().x() - pill_width / 2, y, pill_width, pill_height);
    QColor pill_fill = accent;
    pill_fill.setAlpha(48);
    painter.setPen(QPen(accent, 1.5));
    painter.setBrush(pill_fill);
    painter.drawRoundedRect(pill_rect, pill_height / 2.0, pill_height / 2.0);
    painter.setFont(pill_font);
    painter.setPen(accent.lighter(130));
    painter.drawText(pill_rect, Qt::AlignCenter, pill);
    y += pill_height + unit;

    painter.setFont(body_font);
    painter.setPen(QColor(208, 212, 217));
    const QRect hint_rect(area.left() + unit, y, area.width() - unit * 2, body_metrics.height());
    painter.drawText(hint_rect, Qt::AlignCenter,
                     body_metrics.elidedText(hint, Qt::ElideRight, hint_rect.width()));

    painter.setFont(footer_font);
    painter.setPen(QColor(128, 134, 139));
    const QString footer =
        QStringLiteral("F5 start  ·  F6 stop  ·  F1 HUD  ·  F11 full screen        %1×%2 @ %3 fps")
            .arg(static_cast<int>(width_))
            .arg(static_cast<int>(height_))
            .arg(static_cast<int>(fps_cap_));
    painter.drawText(area.adjusted(unit, unit, -unit, -unit), Qt::AlignHCenter | Qt::AlignBottom,
                     footer);
    painter.restore();
}

void ProjectionWidget::paint_hud(QPainter& painter) const {
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing);
    const QRect area = video_rect();
    const int unit = ui_unit(area.height());
    QFont hud_font = font();
    hud_font.setPixelSize(unit);
    const QFontMetrics metrics(hud_font);
    const QString fps_text =
        fps_ > 0.0 ? QString::number(fps_, 'f', 1) : QStringLiteral("--");
    const QString text = QStringLiteral("%1   %2 fps   %3×%4   painted %5")
                             .arg(status_)
                             .arg(fps_text)
                             .arg(static_cast<int>(width_))
                             .arg(static_cast<int>(height_))
                             .arg(static_cast<qulonglong>(frames_painted_));
    const int pad = unit / 2;
    const int dot = unit * 2 / 3;
    const QRect box(area.left() + pad, area.top() + pad,
                    pad * 3 + dot + metrics.horizontalAdvance(text), metrics.height() + pad);
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(0, 0, 0, 160));
    painter.drawRoundedRect(box, pad, pad);
    painter.setBrush(state_color(state_));
    painter.drawEllipse(QRect(box.left() + pad, box.center().y() - dot / 2, dot, dot));
    painter.setFont(hud_font);
    painter.setPen(Qt::white);
    painter.drawText(box.adjusted(pad * 2 + dot, 0, -pad, 0), Qt::AlignLeft | Qt::AlignVCenter,
                     text);
    painter.restore();
}

void ProjectionWidget::paint_error(QPainter& painter) const {
    painter.save();
    const int unit = ui_unit(height());
    QFont error_font = font();
    error_font.setPixelSize(unit);
    const QFontMetrics metrics(error_font);
    const int bar_height = metrics.height() + unit;
    const QRect bar(0, height() - bar_height, width(), bar_height);
    painter.fillRect(bar, QColor(183, 28, 28, 225));
    painter.setFont(error_font);
    painter.setPen(Qt::white);
    const QRect text_rect = bar.adjusted(unit, 0, -unit, 0);
    painter.drawText(text_rect, Qt::AlignLeft | Qt::AlignVCenter,
                     metrics.elidedText(error_, Qt::ElideRight, text_rect.width()));
    painter.restore();
}

bool ProjectionWidget::handle_view_key(QKeyEvent* event) {
    const int key = event->key();
    if (key != Qt::Key_F1 && key != Qt::Key_F11) {
        return false;
    }
    if (event->type() == QEvent::KeyPress && !event->isAutoRepeat()) {
        if (key == Qt::Key_F1) {
            hud_visible_ = !hud_visible_;
            update();
        } else if (isWindow()) {
            if (isFullScreen()) {
                showNormal();
            } else {
                showFullScreen();
            }
        }
    }
    return true;
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
    if (handle_control_key(event) || handle_view_key(event)) {
        event->accept();
        return;
    }
    deliver_key(event, true);
    event->accept();
}

void ProjectionWidget::keyReleaseEvent(QKeyEvent* event) {
    if (handle_control_key(event) || handle_view_key(event)) {
        event->accept();
        return;
    }
    deliver_key(event, false);
    event->accept();
}

void ProjectionWidget::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton && !touch_active_ &&
        QRectF(video_rect()).contains(event->position())) {
        touch_active_ = true;
        deliver_touch(event, TouchAction::down);
    }
    event->accept();
}

void ProjectionWidget::mouseMoveEvent(QMouseEvent* event) {
    if (touch_active_) {
        deliver_touch(event, TouchAction::move);
    }
    event->accept();
}

void ProjectionWidget::mouseReleaseEvent(QMouseEvent* event) {
    if (touch_active_ && event->button() == Qt::LeftButton) {
        touch_active_ = false;
        deliver_touch(event, TouchAction::up);
    }
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
    const QRect area = video_rect();
    if (area.isEmpty() || width_ == 0 || height_ == 0) {
        return;
    }
    const QPointF local = event->position() - QPointF(area.topLeft());
    const QPointF position(local.x() * static_cast<double>(width_) / static_cast<double>(area.width()),
                           local.y() * static_cast<double>(height_) /
                               static_cast<double>(area.height()));
    const auto x = static_cast<std::uint32_t>(
        std::clamp(static_cast<int>(position.x()), 0, static_cast<int>(width_) - 1));
    const auto y = static_cast<std::uint32_t>(
        std::clamp(static_cast<int>(position.y()), 0, static_cast<int>(height_) - 1));
    input_.on_input(TouchEvent{x, y, action});
}

} // namespace aa::display::qt_adapter
