// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
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

class StdoutIsolation final {
public:
    StdoutIsolation() noexcept {
        const int json = ::fcntl(STDOUT_FILENO, F_DUPFD_CLOEXEC, 3);
        const int null = ::open("/dev/null", O_WRONLY | O_CLOEXEC);
        if (json >= 0 && null >= 0 && ::dup2(null, STDOUT_FILENO) == STDOUT_FILENO) {
            json_ = json;
            active_json_fd.store(json_);
        } else if (json >= 0) {
            ::close(json); // Unisolated rather than broken: JSON stays on fd 1.
        }
        if (null >= 0) { ::close(null); }
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
};
} // namespace aa::tools
