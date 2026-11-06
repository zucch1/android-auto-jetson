// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdio>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>

namespace aa::display {

// Default line sink: writes to stdout and flushes.
inline bool default_stdout_sink(const std::string& line) {
    if (std::fputs(line.c_str(), stdout) == EOF) {
        return false;
    }
    return std::fflush(stdout) == 0;
}

// Single-owner bounded writer for line-oriented diagnostic telemetry. Producers
// only take the queue mutex; the sink is exclusively called by the worker, so a
// producer (the GUI thread) never waits on stdout. A full queue or failed write
// fails closed: later events are counted as undelivered, never written.
// Shutdown safety: the worker thread captures a shared_ptr to the internal state,
// so if close() times out and detaches the worker, no detached thread ever touches
// freed memory when the TelemetryWriter is destroyed.
class TelemetryWriter final {
public:
    using Sink = std::function<bool(const std::string&)>;

    explicit TelemetryWriter(Sink sink = default_stdout_sink, std::size_t capacity = 4096)
        : state_(std::make_shared<SharedState>(std::move(sink), capacity)) {
        worker_ = std::thread([state = state_] { run(state); });
    }

    ~TelemetryWriter() {
        static_cast<void>(close());
    }

    TelemetryWriter(const TelemetryWriter&) = delete;
    TelemetryWriter& operator=(const TelemetryWriter&) = delete;
    TelemetryWriter(TelemetryWriter&&) = delete;
    TelemetryWriter& operator=(TelemetryWriter&&) = delete;

    [[nodiscard]] bool enqueue(std::string line) {
        if (line.empty()) {
            return true;
        }
        if (line.back() != '\n') {
            line.push_back('\n');
        }
        std::lock_guard lock(state_->mutex);
        if (state_->closed || state_->failed || state_->queue.size() >= state_->capacity) {
            state_->failed = true;
            ++state_->undelivered_count;
            return false;
        }
        state_->queue.push_back(std::move(line));
        state_->ready.notify_one();
        return true;
    }

    // Allows deterministic tests to inject a blocked/failing sink and bounded
    // shutdown without making GUI-side producers wait for that sink.
    // Idempotent: after the first close() (clean or timed out and detached),
    // later calls, including the destructor's, return that result immediately.
    [[nodiscard]] bool close(std::chrono::milliseconds timeout = std::chrono::seconds{2}) {
        if (!state_) {
            return true;
        }
        if (close_result_.has_value()) {
            return *close_result_;
        }
        {
            std::lock_guard lock(state_->mutex);
            state_->closed = true;
            state_->ready.notify_all();
        }
        std::unique_lock lock(state_->mutex);
        const bool finished = state_->drained.wait_for(lock, timeout, [state = state_] {
            return state->exited;
        });
        if (!finished) {
            // A sink that never returns cannot be joined safely. Detach only
            // after the worker owns its own shared_ptr to the shared state.
            state_->failed = true;
            state_->undelivered_count += state_->queue.size() + 1;
            state_->queue.clear();
            lock.unlock();
            if (worker_.joinable()) {
                worker_.detach();
            }
            // No diagnostic write here: the stuck sink may share its
            // destination with stderr, and shutdown must stay bounded.
            // Callers learn of this through timed_out().
            timed_out_ = true;
            close_result_ = false;
            return false;
        }
        lock.unlock();
        if (worker_.joinable()) {
            worker_.join();
        }
        std::lock_guard final_lock(state_->mutex);
        close_result_ = !state_->failed && state_->queue.empty() && state_->undelivered_count == 0;
        return *close_result_;
    }

    // True once every accepted event has been handed to the sink AND the sink
    // returned: an event the worker already popped counts until its write ends.
    [[nodiscard]] bool drain(std::chrono::milliseconds timeout = std::chrono::seconds{2}) {
        if (!state_) {
            return true;
        }
        std::unique_lock lock(state_->mutex);
        return state_->drained.wait_for(lock, timeout, [state = state_] {
            return state->failed || (state->queue.empty() && !state->in_flight);
        }) && !state_->failed;
    }

    // True when close() gave up on a sink that never returned and detached
    // the worker: that destination is unusable for any further output.
    [[nodiscard]] bool timed_out() const noexcept { return timed_out_; }

    [[nodiscard]] bool failed() const {
        if (!state_) {
            return false;
        }
        std::lock_guard lock(state_->mutex);
        return state_->failed;
    }

    [[nodiscard]] std::size_t undelivered() const {
        if (!state_) {
            return 0;
        }
        std::lock_guard lock(state_->mutex);
        return state_->undelivered_count;
    }

private:
    struct SharedState final {
        Sink sink;
        const std::size_t capacity;
        std::mutex mutex;
        std::condition_variable ready;
        std::condition_variable drained;
        std::deque<std::string> queue;
        bool closed{false};
        bool failed{false};
        bool exited{false};
        bool in_flight{false};
        std::size_t undelivered_count{0};

        SharedState(Sink s, std::size_t cap)
            : sink(std::move(s)), capacity(cap) {}
    };

    static void run(const std::shared_ptr<SharedState>& state) {
        for (;;) {
            std::string line;
            {
                std::unique_lock lock(state->mutex);
                state->ready.wait(lock, [&] { return state->closed || !state->queue.empty(); });
                if (state->queue.empty() && state->closed) {
                    break;
                }
                if (!state->queue.empty()) {
                    line = std::move(state->queue.front());
                    state->queue.pop_front();
                    state->in_flight = true;
                }
            }
            if (!line.empty()) {
                if (!state->sink(line)) {
                    std::lock_guard lock(state->mutex);
                    state->in_flight = false;
                    state->failed = true;
                    state->undelivered_count += state->queue.size() + 1;
                    state->queue.clear();
                    break;
                }
            }
            {
                std::lock_guard lock(state->mutex);
                state->in_flight = false;
                if (state->queue.empty()) {
                    state->drained.notify_all();
                }
            }
        }
        {
            std::lock_guard lock(state->mutex);
            if (!state->queue.empty()) {
                state->failed = true;
                state->undelivered_count += state->queue.size();
                state->queue.clear();
            }
            state->exited = true;
            state->drained.notify_all();
        }
    }

    std::shared_ptr<SharedState> state_;
    std::thread worker_;
    std::optional<bool> close_result_{};
    bool timed_out_{false};
};

} // namespace aa::display
