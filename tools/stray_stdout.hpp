// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <mutex>
#include <pthread.h>
#include <signal.h>
#include <string>
#include <sys/types.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

namespace aa::tools {
// Keeps third-party stdio chatter off a tool's machine-readable stdout, and
// never lets that chatter block the thread that printed it.
//
// The receiver, the dummy display and the capability listing write JSON to
// stdout. Libraries they load write free text through C stdio: the Jetson
// NVIDIA V4L2 codec (libv4l2_nvvideocodec.so) puts() "Opening in BLOCKING
// MODE" whenever nvv4l2decoder opens, and GLib's g_print() goes the same way.
// Left alone, that text lands as non-JSON lines in the consumer's stream, and
// the flush at exit() blocks with no bound when the consumer stops reading.
//
// While a guard is alive, the C stdio `stdout` stream (a plain assignable
// variable in glibc) is a private stream whose writes only copy into a bounded
// buffer. Text that does not fit is dropped and counted. A drain thread
// forwards the buffer to fd 2. Only that thread can block on a stalled stderr,
// even when fd 1 and fd 2 share one pipe. fd 2 is never made O_NONBLOCK: that
// flag belongs to the open file description shared with the parent and other
// processes. File descriptor 1 is untouched: native-fd writers (StdoutLog,
// the display test channel) still own it, and json() is the original stream
// for deliberate stdio JSON.
//
// Construct a guard before any library loads. Destruction restores `stdout`
// and waits at most `drain_budget` for the drain thread, then leaves it to
// finish or fail on its own. The private stream is never closed (see the
// destructor), so a library that kept it stays safe. The guard does not catch a library
// that write()s to fd 1 directly, or one that captured the stdout stream
// earlier (std::cout). None on the supported hosts does.
class StrayStdoutGuard final {
public:
    static constexpr std::size_t kDefaultCapacity = 64 * 1024;
    static constexpr std::chrono::milliseconds kDefaultDrainBudget{500};

    explicit StrayStdoutGuard(std::size_t capacity = kDefaultCapacity,
                              std::chrono::milliseconds drain_budget = kDefaultDrainBudget)
        : state_(std::make_shared<State>(capacity)), drain_budget_(drain_budget), json_(stdout) {
        std::fflush(json_);
        cookie_io_functions_t io{};
        io.write = &State::cookie_write;
        stray_ = ::fopencookie(state_.get(), "w", io);
        if (stray_ == nullptr) {
            return; // Unguarded rather than broken: stdout stays the real stream.
        }
        std::setvbuf(stray_, nullptr, _IOLBF, 0);
        drainer_ = std::thread([state = state_] { State::drain(state); });
        stdout = stray_;
    }

    ~StrayStdoutGuard() {
        if (stray_ == nullptr) {
            return;
        }
        stdout = json_;
        std::fflush(stray_); // Into the buffer; never blocks.
        // Never fclose(): a library may still hold this stream (it read `stdout`
        // while the guard was alive) and use it later, even from an exit-time
        // handler. The stream and its state stay valid for the process lifetime
        // and drop, with a count, whatever reaches them after close.
        retire(stray_, state_);
        {
            std::unique_lock lock{state_->mutex};
            state_->closing = true;
            state_->wake.notify_all();
            state_->drained.wait_for(lock, drain_budget_, [&] { return state_->done; });
        }
        // A drain thread stuck on stderr keeps the shared state alive and ends
        // when that write returns (EPIPE included: SIGPIPE is blocked there).
        drainer_.detach();
    }

    StrayStdoutGuard(const StrayStdoutGuard &) = delete;
    StrayStdoutGuard &operator=(const StrayStdoutGuard &) = delete;

    // The original stdio stdout, for output that belongs on the JSON channel.
    [[nodiscard]] std::FILE *json() const { return json_; }

    // Stray text dropped so far: buffer full, or stderr failed.
    [[nodiscard]] std::uint64_t dropped_bytes() const {
        std::lock_guard lock{state_->mutex};
        return state_->dropped;
    }

    // Report line for the JSON channel, or empty when nothing was dropped.
    [[nodiscard]] std::string drop_report() const {
        const auto dropped = dropped_bytes();
        return dropped == 0 ? std::string{}
                            : "{\"event\":\"library_stdout_dropped\",\"bytes\":" +
                                  std::to_string(dropped) + "}";
    }

private:
    struct State {
        explicit State(std::size_t c) : capacity(c) {}

        static ssize_t cookie_write(void *cookie, const char *data, std::size_t size) {
            auto &state = *static_cast<State *>(cookie);
            std::lock_guard lock{state.mutex};
            const std::size_t room =
                state.failed || state.closing ? 0 : state.capacity - std::min(state.capacity, state.pending.size());
            const std::size_t kept = std::min(room, size);
            state.pending.append(data, kept);
            state.dropped += size - kept;
            if (kept > 0) { state.wake.notify_one(); }
            return static_cast<ssize_t>(size); // Never a short write: stdio must not retry.
        }

        static void drain(const std::shared_ptr<State> &state) {
            sigset_t mask;
            ::sigemptyset(&mask);
            ::sigaddset(&mask, SIGPIPE);
            ::pthread_sigmask(SIG_BLOCK, &mask, nullptr);
            std::unique_lock lock{state->mutex};
            for (;;) {
                state->wake.wait(lock, [&] { return !state->pending.empty() || state->closing; });
                if (state->pending.empty()) { break; }
                std::string chunk;
                chunk.swap(state->pending);
                lock.unlock();
                std::size_t written = 0;
                bool failed = false;
                while (written < chunk.size()) {
                    const ssize_t n = ::write(STDERR_FILENO, chunk.data() + written, chunk.size() - written);
                    if (n < 0 && errno == EINTR) { continue; }
                    if (n <= 0) { failed = true; break; }
                    written += static_cast<std::size_t>(n);
                }
                lock.lock();
                if (failed) {
                    state->failed = true;
                    state->dropped += chunk.size() - written + state->pending.size();
                    state->pending.clear();
                }
            }
            state->done = true;
            state->drained.notify_all();
        }

        const std::size_t capacity;
        mutable std::mutex mutex;
        std::condition_variable wake;
        std::condition_variable drained;
        std::string pending;
        std::uint64_t dropped = 0;
        bool failed = false;
        bool closing = false;
        bool done = false;
    };

    // Deliberately leaked holder: reachable to the end, so never torn down by a
    // static destructor while exit handlers may still write to a stream.
    static void retire(std::FILE *stream, std::shared_ptr<State> state) {
        static std::mutex mutex;
        static auto *const streams = new std::vector<std::pair<std::FILE *, std::shared_ptr<State>>>;
        std::lock_guard lock{mutex};
        streams->emplace_back(stream, std::move(state));
    }

    std::shared_ptr<State> state_;
    std::chrono::milliseconds drain_budget_;
    std::FILE *json_;
    std::FILE *stray_ = nullptr;
    std::thread drainer_;
};
} // namespace aa::tools
