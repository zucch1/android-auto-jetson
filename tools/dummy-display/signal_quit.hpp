// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Graceful SIGTERM/SIGINT for the dummy display (#90 review 4). The QA
// runners stop the display with SIGTERM; without a handler the process dies
// before app.exec() returns, so the telemetry writer never drains and never
// writes its `telemetry_complete` proof, and every healthy run is BLOCKED.
// The handler only writes one byte to a self-pipe (async-signal-safe); a
// QSocketNotifier on the GUI thread turns it into QApplication::quit(), the
// same path as the test-control `quit`. A signal that arrives before the
// event loop runs stays in the pipe and quits it as soon as it starts. The
// runners still escalate to SIGKILL if shutdown ever wedges.

#include <QApplication>
#include <QSocketNotifier>

#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <memory>
#include <unistd.h>

namespace aa::display::signal_quit {

inline int g_pipe[2] = {-1, -1};

inline void on_signal(int) {
    const int saved = errno;
    const char byte = 1;
    [[maybe_unused]] const auto written = ::write(g_pipe[1], &byte, 1);
    errno = saved;
}

[[nodiscard]] inline std::unique_ptr<QSocketNotifier> install() {
    if (::pipe2(g_pipe, O_CLOEXEC | O_NONBLOCK) != 0) {
        return nullptr;
    }
    auto notifier = std::make_unique<QSocketNotifier>(g_pipe[0], QSocketNotifier::Read);
    QObject::connect(notifier.get(), &QSocketNotifier::activated, [] {
        char drain[16];
        while (::read(g_pipe[0], drain, sizeof drain) > 0) {
        }
        QApplication::quit();
    });
    struct sigaction action {};
    action.sa_handler = on_signal;
    sigemptyset(&action.sa_mask);
    action.sa_flags = SA_RESTART;
    ::sigaction(SIGTERM, &action, nullptr);
    ::sigaction(SIGINT, &action, nullptr);
    return notifier;
}

} // namespace aa::display::signal_quit
