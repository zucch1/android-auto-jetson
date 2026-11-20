// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <string_view>
#include <unistd.h>

namespace aa::tools {
// Process stdout contract for the receiver and the dummy display (see
// ARCHITECTURE.md, "Process stdout"). Fixes the Jetson hang from
// libv4l2_nvvideocodec.so puts("Opening in BLOCKING MODE").
//
// At startup the real stdout moves to a private descriptor, and fd 1 is
// pointed at /dev/null for the rest of the process:
//   * JSON is written only with raw write(2) on json_fd(), never through a
//     FILE*, so no stdio lock is ever on the JSON path.
//   * Library text sent to stdout, through C stdio or raw write(1), goes to
//     /dev/null. It never mixes into the JSON and never blocks on a consumer.
//     It is diagnostic only and deliberately discarded. To read it, run the
//     probe tool directly (gst-inspect-1.0 nvv4l2decoder).
//   * The C `stdout` pointer is never reassigned, so library threads never
//     race on it. Children inherit fd 1 = /dev/null.
// Isolation fails closed: if it cannot be installed, the tool writes one
// stderr line and exits with kStdoutIsolationFailed (4) before any probe.
// end_process() finishes with _Exit. The owner has already drained its JSON
// within its own bound, and stderr is unbuffered. Exit-time stdio cleanup is
// skipped on purpose: C stdout (bound to /dev/null) and any FILE a library
// opened are never flushed or locked, so a third-party writer stuck holding
// a FILE lock cannot stall exit. atexit handlers and static destructors are
// skipped as well.

// The isolated real stdout, or fd 1 when no isolation is active (unit tests).
inline std::atomic<int> active_json_fd{-1};
inline int json_fd() noexcept {
    const int fd = active_json_fd.load();
    return fd >= 0 ? fd : STDOUT_FILENO;
}

// Blocking best-effort write of a whole record. Retries EINTR; false on error.
inline bool write_all(int fd, std::string_view data) noexcept {
    while (!data.empty()) {
        const ssize_t n = ::write(fd, data.data(), data.size());
        if (n < 0 && errno == EINTR) { continue; }
        if (n <= 0) { return false; }
        data.remove_prefix(static_cast<std::size_t>(n));
    }
    return true;
}

// Exit status when isolation cannot be installed (fd 1 closed at start,
// EMFILE, no /dev/null). Distinct from the tools' 0-3.
inline constexpr int kStdoutIsolationFailed = 4;

class StdoutIsolation final {
public:
    // Fails closed: on any error nothing is changed, ok() is false and
    // error() holds the errno. The owner must then refuse to run, because
    // running unisolated would put library text back into the JSON stream.
    StdoutIsolation() noexcept {
        const int json = ::fcntl(STDOUT_FILENO, F_DUPFD_CLOEXEC, 3);
        if (json < 0) {
            error_ = errno;
            return;
        }
        const int null = ::open("/dev/null", O_WRONLY | O_CLOEXEC);
        if (null < 0 || ::dup2(null, STDOUT_FILENO) != STDOUT_FILENO) {
            error_ = errno;
            if (null >= 0) { ::close(null); }
            ::close(json);
            return;
        }
        ::close(null);
        json_ = json;
        active_json_fd.store(json_);
    }

    [[nodiscard]] bool ok() const noexcept { return json_ >= 0; }
    [[nodiscard]] int error() const noexcept { return error_; }

    // One stderr line naming the failure; the caller then exits with
    // kStdoutIsolationFailed before loading any library.
    void report_failure(const char *program) const noexcept {
        char line[256];
        const int size = std::snprintf(line, sizeof line, "%s: stdout_isolation_failed: %s (errno %d)\n",
                                       program, std::strerror(error_), error_);
        if (size > 0) {
            static_cast<void>(write_all(STDERR_FILENO, std::string_view{line, static_cast<std::size_t>(size)}));
        }
    }
    StdoutIsolation(const StdoutIsolation &) = delete;
    StdoutIsolation &operator=(const StdoutIsolation &) = delete;

    [[nodiscard]] int json() const noexcept { return json_ >= 0 ? json_ : STDOUT_FILENO; }

    // Production exit, after the owner's bounded JSON shutdown.
    [[noreturn]] static void end_process(int code) noexcept { std::_Exit(code); }

    // In-process tests only: hand fd 1 back to the test harness. Flushes C
    // stdout while it still points at /dev/null, so buffered library text never
    // reaches the restored descriptor.
    int release(int code) noexcept {
        if (json_ >= 0) {
            std::fflush(stdout);
            ::dup2(json_, STDOUT_FILENO);
            active_json_fd.store(-1);
            ::close(json_);
            json_ = -1;
        }
        return code;
    }

private:
    int json_ = -1;
    int error_ = 0;
};
} // namespace aa::tools
