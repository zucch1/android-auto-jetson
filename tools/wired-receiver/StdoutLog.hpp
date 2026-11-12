// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <memory>
#include <mutex>
#include <pthread.h>
#include <signal.h>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

namespace aa::wired_receiver {

// Single-writer stdout log with a bounded, FIFO-exact queue.
//
//   * submit(): control / lifecycle / error records. Lossless: a full queue makes
//     the producer wait (issue #60: the logger survives a long stdout block
//     without dropping or reordering these records).
//   * submit_media(): phone-driven media telemetry (audio completions, periodic
//     video events). Audit L13: a hostile or merely chatty phone can sustain
//     media traffic, so these records are rate-limited per kind with a token
//     bucket and NEVER make the media thread wait for the sink. Records over the
//     rate, or arriving at a full queue, are dropped and counted; the count is
//     reported in-band as a media_telemetry_dropped record (at most once a
//     second, and once at finish) and through media_drops().
//   * finish(): audit L13: shutdown waits for the queue to drain only up to a
//     deadline. A sink that never returns (blocked pipe or terminal) is
//     abandoned: the worker is detached and owns the shared state, so nothing it
//     touches is freed under it (same pattern as display::TelemetryWriter).
class StdoutLog final {
public:
    static constexpr std::size_t byte_capacity = 1'048'576;
    static constexpr std::size_t entry_capacity = 4096;
    static constexpr std::chrono::milliseconds default_finish_timeout{2000};
    static constexpr std::chrono::milliseconds media_summary_interval{1000};
    using Write = ssize_t (*)(int, const void *, std::size_t);
    using Clock = std::chrono::steady_clock;
    using Now = Clock::time_point (*)();

    struct MediaLimit {
        constexpr MediaLimit(double per_second = 200.0, double bucket_depth = 400.0)
            : events_per_second(per_second), burst(bucket_depth) {}
        double events_per_second; // sustained rate, per kind
        double burst;             // bucket depth, per kind
    };
    struct MediaDrops {
        std::uint64_t rate_limited;
        std::uint64_t backpressure;
    };

    explicit StdoutLog(std::atomic<bool> &running, Write write = ::write,
                       Now now = &Clock::now, MediaLimit limit = MediaLimit())
        : state_(std::make_shared<State>(running, write, now, limit)),
          worker_([state = state_] { run(state); }) {}
    // Owners check finish explicitly; unwinding still must join (or abandon) the worker.
    ~StdoutLog() { [[maybe_unused]] const auto error = finish(); }
    StdoutLog(const StdoutLog &) = delete;
    StdoutLog &operator=(const StdoutLog &) = delete;

    std::error_code submit(std::string_view record) {
        State &s = *state_;
        const std::lock_guard producer(producer_);
        const bool oversize = record.size() >= byte_capacity;
        std::size_t offset = 0;
        for (;;) {
            const auto count = std::min(byte_capacity, record.size() - offset);
            const bool newline = count < byte_capacity && offset + count == record.size();
            const auto charge = count + static_cast<std::size_t>(newline);
            std::unique_lock lock(s.mutex);
#ifdef AA_WIRED_RECEIVER_SHUTDOWN_TEST
            s.waiting = !s.space(charge) && !s.error && !s.closed;
            s.changed.notify_all();
#endif
            s.changed.wait(lock, [&] { return s.error || s.closed || s.space(charge); });
#ifdef AA_WIRED_RECEIVER_SHUTDOWN_TEST
            s.waiting = false;
#endif
            if (s.error || s.closed) {
                s.chunked = false;
                return s.error ? s.error : closed_error();
            }
            // Allocate only after capacity admission; the front stays charged during I/O.
            s.push(record.substr(offset, count), charge);
            s.chunked = oversize && !newline;
            s.changed.notify_all();
            offset += count;
            if (newline) { return {}; }
        }
    }

    // Non-blocking, rate-limited path for phone-driven media telemetry. `kind`
    // names the event class (a stable literal such as "audio_write_completed");
    // each kind has its own token bucket. Returns only a latched sink error or
    // ESHUTDOWN; a dropped record is not an error (see media_drops()).
    std::error_code submit_media(std::string_view kind, std::string_view record) {
        State &s = *state_;
        const std::lock_guard lock(s.mutex);
        if (s.error) { return s.error; }
        if (s.closed) { return closed_error(); }
        const auto at = s.now();
        if (!s.admit_media(kind, at)) {
            ++s.drops.rate_limited;
            return {};
        }
        s.report_drops_if_due(at, false);
        const auto charge = record.size() + 1;
        // A chunked oversize record must stay contiguous, and a full queue must
        // never make a media thread wait for the sink.
        if (s.chunked || record.size() >= byte_capacity || !s.space(charge)) {
            ++s.drops.backpressure;
            return {};
        }
        s.push(record, charge);
        s.changed.notify_all();
        return {};
    }

    [[nodiscard]] MediaDrops media_drops() const {
        const std::lock_guard lock(state_->mutex);
        return state_->drops;
    }
    // True once finish() gave up on a sink that never returned.
    [[nodiscard]] bool timed_out() const {
        const std::lock_guard lock(state_->mutex);
        return state_->abandoned;
    }

    std::error_code flush() {
        State &s = *state_;
        const std::lock_guard producer(producer_);
        std::unique_lock lock(s.mutex);
#ifdef AA_WIRED_RECEIVER_SHUTDOWN_TEST
        s.flushing = !s.queue.empty() && !s.error;
        s.changed.notify_all();
#endif
        s.changed.wait(lock, [&] { return s.queue.empty() || s.error; });
#ifdef AA_WIRED_RECEIVER_SHUTDOWN_TEST
        s.flushing = false;
#endif
        return s.error;
    }

    // Close admission and wait for the queue to drain, but only up to `timeout`.
    // On timeout the worker (stuck in the sink) is detached and the call returns
    // ETIMEDOUT; records still queued are counted as undelivered.
    std::error_code finish(std::chrono::milliseconds timeout = default_finish_timeout) {
        State &s = *state_;
        const std::lock_guard finishing(finishing_);
        if (!worker_.joinable()) { return error_snapshot(); }
        const auto deadline = Clock::now() + timeout;
        // A producer blocked on a full queue holds producer_; do not wait for it
        // past the deadline (closing below wakes it with ESHUTDOWN).
        std::unique_lock producer(producer_, std::defer_lock);
        [[maybe_unused]] const bool owned = producer.try_lock_until(deadline);
        std::unique_lock lock(s.mutex);
        s.report_drops_if_due(s.now(), true);
        s.closed = true;
        s.changed.notify_all();
        const bool done = s.changed.wait_until(lock, deadline, [&] { return s.exited; });
        if (!done) {
            // The queue is left in the shared state: the stuck worker may still
            // be reading its front entry.
            s.abandoned = true;
            s.running = nullptr;
            s.undelivered = s.queue.size();
            if (!s.error) { s.error = {ETIMEDOUT, std::generic_category()}; }
            s.changed.notify_all();
            lock.unlock();
            worker_.detach();
            return error_snapshot();
        }
        lock.unlock();
        worker_.join();
        return error_snapshot();
    }
#ifdef AA_WIRED_RECEIVER_SHUTDOWN_TEST
    bool wait_for_capacity_wait() {
        State &s = *state_;
        std::unique_lock lock(s.mutex);
        return s.changed.wait_for(lock, std::chrono::seconds{5}, [&] { return s.waiting; });
    }
    std::pair<std::size_t, std::size_t> outstanding() {
        const std::lock_guard lock(state_->mutex);
        return {state_->bytes, state_->queue.size()};
    }
    bool wait_for_barrier(bool finishing) {
        State &s = *state_;
        std::unique_lock lock(s.mutex);
        return s.changed.wait_for(lock, std::chrono::seconds{5},
                                  [&] { return finishing ? s.closed : s.flushing; });
    }
#endif
private:
    struct Bucket {
        std::string kind;
        double tokens{};
        Clock::time_point refilled{};
    };
    struct State {
        State(std::atomic<bool> &r, Write w, Now n, MediaLimit l)
            : running(&r), write(w), now(n), limit(l) {}
        std::atomic<bool> *running; // cleared when finish() abandons the worker
        Write write;
        Now now;
        MediaLimit limit;
        mutable std::mutex mutex;
        std::condition_variable changed;
        std::deque<std::string> queue;
        std::size_t bytes = 0;
        std::error_code error;
        bool closed = false;
        bool exited = false;
        bool abandoned = false;
        bool chunked = false; // an oversize record is mid-admission
        std::size_t undelivered = 0;
        MediaDrops drops{0, 0};
        MediaDrops reported{0, 0};
        Clock::time_point last_report{};
        bool reported_once = false;
        std::vector<Bucket> buckets;
#ifdef AA_WIRED_RECEIVER_SHUTDOWN_TEST
        bool waiting = false;
        bool flushing = false;
#endif
        bool space(std::size_t charge) const {
            return charge <= byte_capacity - bytes && queue.size() < entry_capacity;
        }
        void push(std::string_view record, std::size_t charge) {
            queue.emplace_back(charge, '\n');
            record.copy(queue.back().data(), record.size());
            bytes += charge;
        }
        bool admit_media(std::string_view kind, Clock::time_point at) {
            constexpr std::size_t max_kinds = 32;
            Bucket *bucket = nullptr;
            for (auto &candidate : buckets) {
                if (candidate.kind == kind) { bucket = &candidate; break; }
            }
            if (!bucket) {
                // Past the cap, unknown kinds share the last bucket instead of
                // growing without bound.
                if (buckets.size() >= max_kinds) {
                    bucket = &buckets.back();
                } else {
                    buckets.push_back({std::string{kind}, limit.burst, at});
                    bucket = &buckets.back();
                }
            }
            const double elapsed = std::chrono::duration<double>(at - bucket->refilled).count();
            if (elapsed > 0) {
                bucket->tokens = std::min(limit.burst, bucket->tokens + elapsed * limit.events_per_second);
                bucket->refilled = at;
            }
            if (bucket->tokens < 1.0) { return false; }
            bucket->tokens -= 1.0;
            return true;
        }
        // Report drops accumulated since the last report as one in-band record,
        // at most once per media_summary_interval (or unconditionally at finish).
        void report_drops_if_due(Clock::time_point at, bool final) {
            const auto rate = drops.rate_limited - reported.rate_limited;
            const auto pressure = drops.backpressure - reported.backpressure;
            if (rate + pressure == 0) { return; }
            if (!final && reported_once && at - last_report < media_summary_interval) { return; }
            const std::string line = "{\"event\":\"media_telemetry_dropped\",\"rate_limited\":" +
                                     std::to_string(rate) + ",\"backpressure\":" +
                                     std::to_string(pressure) + "}";
            if (chunked || !space(line.size() + 1)) { return; }
            push(line, line.size() + 1);
            reported = drops;
            last_report = at;
            reported_once = true;
            changed.notify_all();
        }
    };

    static std::error_code closed_error() { return {ESHUTDOWN, std::generic_category()}; }
    std::error_code error_snapshot() const {
        const std::lock_guard lock(state_->mutex);
        return state_->error;
    }

    static void fail(const std::shared_ptr<State> &state, int error) {
        {
            const std::lock_guard lock(state->mutex);
            if (state->error) { return; }
            state->error = {error, std::generic_category()};
            // After an abandoned finish() the owner (and its running flag) may
            // be gone; never touch it from the detached worker.
            if (state->running) { state->running->store(false); }
            state->changed.notify_all();
        }
        char message[64];
        const int size = std::snprintf(message, sizeof(message), "stdout_log errno=%d\n", error);
        if (size > 0) {
            // A failed/short best-effort diagnostic must not replace the latched stdout error.
            if (::write(STDERR_FILENO, message, static_cast<std::size_t>(size)) != size) { return; }
        }
    }
    static void run(std::shared_ptr<State> state) {
        sigset_t mask;
        ::sigemptyset(&mask);
        ::sigaddset(&mask, SIGPIPE);
        if (const int error = ::pthread_sigmask(SIG_BLOCK, &mask, nullptr); error != 0) {
            fail(state, error);
        } else {
            drain(state);
        }
        const std::lock_guard lock(state->mutex);
        state->exited = true;
        state->changed.notify_all();
    }
    static void drain(const std::shared_ptr<State> &state) {
        for (;;) {
            std::unique_lock lock(state->mutex);
            state->changed.wait(lock, [&] { return state->closed || !state->queue.empty(); });
            if (state->queue.empty()) { return; }
            // The deque keeps element addresses stable; the front stays charged
            // (and queued) while it is being written.
            const std::string_view front = state->queue.front();
            lock.unlock();
            std::size_t offset = 0;
            while (offset < front.size()) {
                const auto count = state->write(STDOUT_FILENO, front.data() + offset, front.size() - offset);
                if (count < 0 && errno == EINTR) { continue; }
                if (count <= 0) { fail(state, count == 0 ? EIO : errno); return; }
                offset += static_cast<std::size_t>(count);
            }
            lock.lock();
            state->bytes -= front.size();
            state->queue.pop_front();
            state->changed.notify_all();
            if (state->abandoned) { return; }
        }
    }

    std::shared_ptr<State> state_;
    std::timed_mutex producer_;
    std::mutex finishing_;
    std::thread worker_;
};

inline std::atomic<StdoutLog *> active_stdout_log{nullptr};

class StdoutLogScope final {
public:
    explicit StdoutLogScope(std::atomic<bool> &running) : log(running) {
        StdoutLog *empty = nullptr;
        if (!active_stdout_log.compare_exchange_strong(empty, &log)) { std::abort(); }
    }
    ~StdoutLogScope() {
        [[maybe_unused]] const auto error = log.finish();
        active_stdout_log.store(nullptr);
    }
    StdoutLogScope(const StdoutLogScope &) = delete;
    StdoutLogScope &operator=(const StdoutLogScope &) = delete;
    StdoutLog log;
};
} // namespace aa::wired_receiver
