// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// TEST-ONLY stdin control channel for the shipped aa-dummy-display (task 24).
// Enabled solely via the explicit --test-only-control-stdin flag; without it
// the channel never touches stdin and the app behaves exactly as before. It
// exists so executable-level tests can drive the SHIPPED binary's real event
// loop and real widget handlers (the same code paths a human keyboard/mouse
// would reach) and read observable status back - never a production control
// surface (no production UI/settings/app-host scope, task 44 owns that).

#include <aa/display/Display.hpp>

#include "ProjectionWidget.hpp"

#include <QApplication>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QTimer>
#include <QWidget>

#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <fcntl.h>
#include <string>
#include <string_view>
#include <unistd.h>
#include <utility>
#include <vector>

namespace aa::display::testcontrol {

struct Status final {
    DisplayState state{DisplayState::unavailable};
    std::uint64_t frames_presented{};
    std::uint64_t frames_painted{};
    bool helper_started{};
    std::uint64_t input_sent{};
    std::uint64_t input_dropped{};
    std::string input_last_error{};
    std::string last_error{};
    bool render_ok{};
    int render_width{};
    int render_height{};
};

class Channel final {
public:
    using StatusProbe = Status (*)();

    Channel(qt_adapter::ProjectionWidget& widget, StatusProbe probe)
        : widget_(widget), probe_(probe) {
        const int flags = ::fcntl(0, F_GETFL, 0);
        if (flags >= 0) {
            static_cast<void>(::fcntl(0, F_SETFL, flags | O_NONBLOCK));
        }
        QObject::connect(&poll_, &QTimer::timeout, [this] { drain(); });
        poll_.start(50);
        write_line("{\"event\":\"ready\"}");
    }

    Channel(const Channel&) = delete;
    Channel& operator=(const Channel&) = delete;

private:
    static constexpr std::size_t kMaxLine = 512;

    static int key_from_name(std::string_view name) {
        if (name.size() == 1) {
            const char c = name[0];
            if (c >= 'a' && c <= 'z') {
                return Qt::Key_A + (c - 'a');
            }
            if (c >= 'A' && c <= 'Z') {
                return Qt::Key_A + (c - 'A');
            }
            if (c >= '0' && c <= '9') {
                return Qt::Key_0 + (c - '0');
            }
        }
        if (name == "f5") return Qt::Key_F5;
        if (name == "f6") return Qt::Key_F6;
        if (name == "up") return Qt::Key_Up;
        if (name == "down") return Qt::Key_Down;
        if (name == "left") return Qt::Key_Left;
        if (name == "right") return Qt::Key_Right;
        if (name == "enter") return Qt::Key_Return;
        if (name == "backspace") return Qt::Key_Backspace;
        if (name == "escape") return Qt::Key_Escape;
        if (name == "space") return Qt::Key_Space;
        return 0;
    }

    static void write_line(const std::string& line) {
        const std::string out = line + "\n";
        std::size_t off = 0;
        while (off < out.size()) {
            const ssize_t n = ::write(1, out.data() + off, out.size() - off);
            if (n <= 0) {
                return;
            }
            off += static_cast<std::size_t>(n);
        }
    }

    void post_key(int key, bool press) {
        QKeyEvent event(press ? QEvent::KeyPress : QEvent::KeyRelease, key, Qt::NoModifier);
        QApplication::sendEvent(&widget_, &event);
    }

    void post_touch(std::string_view action, int x, int y) {
        const QPointF at(static_cast<qreal>(x), static_cast<qreal>(y));
        QEvent::Type type = QEvent::MouseMove;
        Qt::MouseButton button = Qt::NoButton;
        if (action == "down") {
            type = QEvent::MouseButtonPress;
            button = Qt::LeftButton;
        } else if (action == "up") {
            type = QEvent::MouseButtonRelease;
            button = Qt::LeftButton;
        }
        QMouseEvent event(type, at, at, button, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(&widget_, &event);
    }

    std::string status_json() const {
        const Status s = probe_ == nullptr ? Status{} : probe_();
        const auto state = to_string(s.state);
        std::string out = "{\"event\":\"status\",\"state\":\"";
        out.append(state);
        out += "\",\"frames_presented\":" + std::to_string(s.frames_presented);
        out += ",\"frames_painted\":" + std::to_string(s.frames_painted);
        out += ",\"helper_started\":";
        out += s.helper_started ? "true" : "false";
        out += ",\"input_sent\":" + std::to_string(s.input_sent);
        out += ",\"input_dropped\":" + std::to_string(s.input_dropped);
        out += ",\"input_last_error\":\"" + s.input_last_error + "\"";
        out += ",\"last_error\":\"" + s.last_error + "\"";
        out += ",\"render_ok\":";
        out += s.render_ok ? "true" : "false";
        out += ",\"render_width\":" + std::to_string(s.render_width);
        out += ",\"render_height\":" + std::to_string(s.render_height);
        out += "}";
        return out;
    }

    void dispatch(const std::string& line) {
        const std::vector<std::string> tok = split(line);
        if (tok.empty()) {
            return;
        }
        if (tok[0] == "status") {
            write_line(status_json());
            return;
        }
        if (tok[0] == "key" && tok.size() == 3) {
            const int key = key_from_name(tok[1]);
            if (key == 0 || (tok[2] != "press" && tok[2] != "release")) {
                write_line("{\"event\":\"err\",\"cmd\":\"key\"}");
                return;
            }
            post_key(key, tok[2] == "press");
            write_line("{\"event\":\"ok\",\"cmd\":\"key\"}");
            return;
        }
        if (tok[0] == "touch" && tok.size() == 4) {
            const int x = std::atoi(tok[2].c_str());
            const int y = std::atoi(tok[3].c_str());
            post_touch(tok[1], x, y);
            write_line("{\"event\":\"ok\",\"cmd\":\"touch\"}");
            return;
        }
        if (tok[0] == "start" && tok.size() == 1) {
            post_key(Qt::Key_F5, true);
            post_key(Qt::Key_F5, false);
            write_line("{\"event\":\"ok\",\"cmd\":\"start\"}");
            return;
        }
        if (tok[0] == "stop" && tok.size() == 1) {
            post_key(Qt::Key_F6, true);
            post_key(Qt::Key_F6, false);
            write_line("{\"event\":\"ok\",\"cmd\":\"stop\"}");
            return;
        }
        if (tok[0] == "quit" && tok.size() == 1) {
            write_line("{\"event\":\"ok\",\"cmd\":\"quit\"}");
            QApplication::quit();
            return;
        }
        write_line("{\"event\":\"err\",\"cmd\":\"unknown\"}");
    }

    static std::vector<std::string> split(const std::string& line) {
        std::vector<std::string> tok;
        std::string cur;
        for (const char c : line) {
            if (c == ' ' || c == '\t' || c == '\r') {
                if (!cur.empty()) {
                    tok.push_back(cur);
                    cur.clear();
                }
            } else {
                cur += c;
            }
        }
        if (!cur.empty()) {
            tok.push_back(cur);
        }
        return tok;
    }

    void drain() {
        std::array<char, 256> chunk{};
        for (;;) {
            const ssize_t n = ::read(0, chunk.data(), chunk.size());
            if (n <= 0) {
                break;
            }
            buffer_.append(chunk.data(), static_cast<std::size_t>(n));
        }
        std::size_t cut = 0;
        for (;;) {
            const auto nl = buffer_.find('\n', cut);
            if (nl == std::string::npos) {
                break;
            }
            std::string line = buffer_.substr(cut, nl - cut);
            cut = nl + 1;
            if (line.size() > kMaxLine) {
                write_line("{\"event\":\"err\",\"cmd\":\"oversize\"}");
                continue;
            }
            dispatch(line);
        }
        if (cut > 0) {
            buffer_.erase(0, cut);
        }
        if (buffer_.size() > kMaxLine) {
            buffer_.clear();
            write_line("{\"event\":\"err\",\"cmd\":\"oversize\"}");
        }
    }

    qt_adapter::ProjectionWidget& widget_;
    StatusProbe probe_{nullptr};
    QTimer poll_{};
    std::string buffer_{};
};

} // namespace aa::display::testcontrol
