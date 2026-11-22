// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <aa/diagnostics/JsonStdoutFd.hpp>
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

namespace aa::diagnostics {

// Formats a redacted diagnostics JSON object (`{"kind":...}`) into the
// receiver event envelope (`{"event":"diag","kind":...}`).
[[nodiscard]] inline std::string diag_event_json(std::string_view redacted_json) {
    if (!redacted_json.empty() && redacted_json.front() == '{') {
        return std::string{"{\"event\":\"diag\","} + std::string{redacted_json.substr(1)};
    }
    return std::string{redacted_json};
}

// Single-writer stdout log with a bounded, FIFO-exact queue.
//
//   * submit(): startup / shutdown lifecycle records. Lossless while running:
//     a full queue makes the producer wait (issue #60: the logger survives a
//     long stdout block without dropping or reordering these records).
//   * submit_runtime(): steady-state runtime, diagnostic, and control records
//     (issue #90). Never makes a media, audio, or control thread wait for a
//     full queue behind a stalled sink; records arriving at a full queue are
//     dropped and counted, and reported in-band as runtime_log_dropped (at most
//     once per media_summary_interval, and once at finish) and through
//     runtime_drops().
//   * submit_pending(): the one non-blocking lossless path (#90 submit_state
//     and #159 submit_pending, unified): one-shot lifecycle / state-transition
//     and control records that the projection collector and drive smoke key
//     state on (`active`, `err`, audio_stall_bypass begin/end, sink reopen
//     selection, idr_*). Never blocks the caller and is never dropped for
//     backpressure: a record that finds the queue full is parked in ONE bounded
//     FIFO slot (pending_lossless_capacity records, pending_lossless_byte_capacity
//     bytes) and promoted into the queue, in order and ahead of later records,
//     as soon as the drain frees space. Lossless submit() waits behind parked
//     records, so a transition is never overtaken by `stopping`.
//     Overflowing the slot fails closed twice: the lost record is counted as a
//     runtime drop and, separately, as runtime_log_dropped.state_overflow (the
//     field gate consumers fail closed on), AND the log latches ENOBUFS
//     (EMSGSIZE for a record that can never fit) and clears the owner's running
//     flag, so the receiver stops, never writes log_complete and exits non-zero
//     even if stdout drains later (latched_error()).
//     Droppable (runtime/media) records never overtake a parked state record,
//     so the parked head makes progress at the byte limit too (FIFO-fair).
//   * Drop summaries (media_telemetry_dropped, runtime_log_dropped) are written
//     by the worker itself between records and never occupy queue capacity a
//     real record needs (#90 review P3). They are written only at a record
//     boundary, never between the chunks of an oversize record (review 4).
//   * Completion (production, via StdoutLogScope): after a clean drain at
//     finish() the worker writes a final log_complete record with cumulative
//     loss counters and the worst submit-to-write delay. An abandoned or failed
//     sink never gets one, so gate consumers treat its absence as incomplete:
//     the worker re-checks abandonment and the error latch after the final
//     summaries, immediately before starting the completion write. The one
//     write it cannot recall is a completion write already in flight when
//     finish() times out: that line may still land later, and it is truthful
//     (it is started only after every queued record was written, and finish()
//     still reports ETIMEDOUT through the exit status). A record cut short by
//     shutdown mid-chunk is terminated with a newline (an unparseable line)
//     and counted in shutdown_dropped, so the completion line stays parseable
//     and the loss fails consumers closed.
//   * Records submitted through the receiver's non-blocking wrappers carry
//     "emit_ns" (with_emit_ns): their source emission time, so a late delivery
//     never moves an event in time.
//   * submit_media(): phone-driven media telemetry (audio completions, periodic
//     video events). Audit L13: a hostile or merely chatty phone can sustain
//     media traffic, so these records are rate-limited per kind with a token
//     bucket and NEVER make the media thread wait for the sink. Records over the
//     rate, or arriving at a full queue, are dropped and counted; the count is
//     reported in-band as a media_telemetry_dropped record (at most once a
//     second, and once at finish) and through media_drops().
//   * Shutdown (audit L13): once the owner's running flag is false, a lossless
//     submit() that finds no space waits at most `shutdown_wait` for the sink
//     (a healthy sink drains and the record is delivered). When the sink does
//     not make room in that window the record is dropped with ETIMEDOUT and every
//     later submit that finds no space drops immediately, so receiver teardown
//     (stopping, post-join audio_finish) can never wait on a stuck stdout. While
//     running stays true the lossless wait is unbounded (issue #60).
//   * finish(): audit L13: shutdown waits for the queue to drain only up to a
//     deadline. A sink that never returns (blocked pipe or terminal) is
//     abandoned: the worker is detached and owns the shared state, so nothing it
//     touches is freed under it (same pattern as display::TelemetryWriter).
class StdoutLog final {
public:
    static constexpr std::size_t byte_capacity = 1'048'576;
    static constexpr std::size_t entry_capacity = 4096;
    // Parked lossless records: low-rate gate inputs, sized for a 30 s stall with
    // recovery activity, bounded in count and bytes (hostile-phone memory bound).
    static constexpr std::size_t pending_lossless_capacity = 1024;
    static constexpr std::size_t pending_lossless_byte_capacity = 256 * 1024;
    static constexpr std::chrono::milliseconds default_finish_timeout{2000};
    static constexpr std::chrono::milliseconds media_summary_interval{1000};
    // How often a blocked lossless producer re-checks the running flag.
    static constexpr std::chrono::milliseconds shutdown_poll{20};
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
                       Now now = &Clock::now, MediaLimit limit = MediaLimit(),
                       std::chrono::milliseconds shutdown_wait = default_finish_timeout,
                       bool completion_record = false)
        : state_(std::make_shared<State>(running, write, now, limit, shutdown_wait, completion_record)),
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
            s.promote_parked();
#ifdef AA_WIRED_RECEIVER_SHUTDOWN_TEST
            s.waiting = !s.space(charge) && !s.error && !s.closed;
            s.changed.notify_all();
#endif
            const bool waited = !s.error && !s.closed && !s.space(charge);
            const bool admitted = s.wait_for_space(lock, charge);
#ifdef AA_WIRED_RECEIVER_SHUTDOWN_TEST
            s.waiting = false;
#endif
            if (s.error || s.closed || !admitted) {
                if (s.chunked && !admitted && !s.error && !s.closed) {
                    // The tail timed out (counted in shutdown_dropped): the worker
                    // ends the partial line once its queued chunks are out, so
                    // the next record never joins it (#90 review 5).
                    s.cut = true;
                    s.cut_end = s.pushed;
                    s.changed.notify_all();
                }
                s.chunked = false;
                if (!admitted && !s.error && !s.closed) { return shutdown_error(); }
                // A producer woken by an abandoning finish() always sees ESHUTDOWN;
                // the ETIMEDOUT latch is for the owner and for later arrivals.
                if (s.error && !(waited && s.abandoned)) { return s.error; }
                return closed_error();
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
        s.promote_parked();
        const auto charge = record.size() + 1;
        // A chunked oversize record must stay contiguous, a full queue must
        // never make a media thread wait for the sink, and a droppable record
        // never overtakes a parked state record (FIFO-fair progress, #90 P2).
        if (s.chunked || !s.parked.empty() || record.size() >= byte_capacity || !s.space(charge)) {
            ++s.drops.backpressure;
            return {};
        }
        s.push(record, charge);
        s.changed.notify_all();
        return {};
    }

    // Non-blocking path for steady-state runtime, diagnostic, and control
    // records (issue #90). A full queue behind a stalled sink never blocks the
    // calling thread; dropped records are counted and reported in-band as
    // runtime_log_dropped.
    std::error_code submit_runtime(std::string_view record) {
        State &s = *state_;
        const std::lock_guard lock(s.mutex);
        if (s.error) { return s.error; }
        if (s.closed) { return closed_error(); }
        s.promote_parked();
        const auto charge = record.size() + 1;
        if (s.chunked || !s.parked.empty() || record.size() >= byte_capacity || !s.space(charge)) {
            ++s.runtime_dropped;
            return {};
        }
        s.push(record, charge);
        s.changed.notify_all();
        return {};
    }

    // Non-blocking, eventually-delivered lossless path (#90 submit_state, #159
    // submit_pending): takes only state_->mutex, never producer_. Enqueues
    // directly when the queue has room, is not mid-chunk and nothing is parked;
    // otherwise parks in the bounded pending_lossless FIFO that the worker
    // promotes as stdout makes room. Overflow fails closed (see class comment).
    std::error_code submit_pending(std::string_view record) {
        State &s = *state_;
        const std::lock_guard lock(s.mutex);
        if (s.error) { return s.error; }
        if (s.closed) { return closed_error(); }
        s.promote_parked();
        const auto charge = record.size() + 1;
        if (s.parked.empty() && !s.chunked && record.size() < byte_capacity && s.space(charge)) {
            s.push(record, charge);
            s.changed.notify_all();
            return {};
        }
        const bool unfit = record.size() >= byte_capacity || record.size() > pending_lossless_byte_capacity;
        if (unfit || s.parked.size() >= pending_lossless_capacity ||
            record.size() > pending_lossless_byte_capacity - s.parked_bytes) {
            // Gate-relevant loss: reported in-band as state_overflow so a
            // consumer fails closed (#90 review), and latched so the owner
            // stops and exits non-zero (#159).
            ++s.runtime_dropped;
            ++s.state_dropped;
            s.error = {unfit ? EMSGSIZE : ENOBUFS, std::generic_category()};
            if (s.running) { s.running->store(false); }
            s.changed.notify_all();
            return s.error;
        }
        s.parked.emplace_back(record);
        s.parked_at.push_back(s.now());
        s.parked_bytes += record.size();
        s.changed.notify_all();
        return {};
    }

    [[nodiscard]] std::size_t pending_lossless() const {
        const std::lock_guard lock(state_->mutex);
        return state_->parked.size();
    }

    // The first latched error (sink failure, pending_lossless overflow) or none.
    [[nodiscard]] std::error_code latched_error() const {
        const std::lock_guard lock(state_->mutex);
        return state_->error;
    }

    // Lossless records dropped because the sink made no room during shutdown.
    [[nodiscard]] std::uint64_t shutdown_drops() const {
        const std::lock_guard lock(state_->mutex);
        return state_->shutdown_dropped;
    }

    [[nodiscard]] std::uint64_t runtime_drops() const {
        const std::lock_guard lock(state_->mutex);
        return state_->runtime_dropped;
    }

    // Lossless records lost because pending_lossless overflowed (subset of runtime_drops()).
    [[nodiscard]] std::uint64_t state_overflow_drops() const {
        const std::lock_guard lock(state_->mutex);
        return state_->state_dropped;
    }

    // Worst delay between a record's submission and its write to stdout.
    [[nodiscard]] Clock::duration max_delivery_lag() const {
        const std::lock_guard lock(state_->mutex);
        return state_->max_lag;
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
    // Queued plus parked records left behind when finish() abandoned a stalled sink.
    [[nodiscard]] std::size_t undelivered() const {
        const std::lock_guard lock(state_->mutex);
        return state_->undelivered;
    }

    std::error_code flush() {
        State &s = *state_;
        const std::lock_guard producer(producer_);
        std::unique_lock lock(s.mutex);
#ifdef AA_WIRED_RECEIVER_SHUTDOWN_TEST
        s.flushing = (!s.queue.empty() || !s.parked.empty()) && !s.error;
        s.changed.notify_all();
#endif
        s.changed.wait(lock, [&] {
            return (s.queue.empty() && s.parked.empty() && !s.writing_summary &&
                    !s.summary_due(s.now(), false)) ||
                   s.error;
        });
#ifdef AA_WIRED_RECEIVER_SHUTDOWN_TEST
        s.flushing = false;
#endif
        return s.error;
    }

    // Close admission and wait for the queue to drain, but only up to `timeout`.
    // On timeout the worker (stuck in the sink) is detached and the call returns
    // ETIMEDOUT; records still queued are counted as undelivered. Shutdown drops
    // also return ETIMEDOUT, even if the sink recovers and drains the queue.
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
        s.promote_parked();
        s.closed = true;
        s.changed.notify_all();
        const bool done = s.changed.wait_until(lock, deadline, [&] { return s.exited; });
        if (!done) {
            // The queue is left in the shared state: the stuck worker may still
            // be reading its front entry.
            s.abandoned = true;
            s.running = nullptr;
            s.undelivered = s.queue.size() + s.parked.size();
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
    bool wait_for_worker_exit() {
        std::unique_lock lock(state_->mutex);
        return state_->changed.wait_for(lock, std::chrono::seconds{5}, [&] { return state_->exited; });
    }
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
        State(std::atomic<bool> &r, Write w, Now n, MediaLimit l, std::chrono::milliseconds wait,
              bool completion_record)
            : running(&r), write(w), now(n), limit(l), shutdown_wait(wait),
              completion(completion_record) {}
        std::atomic<bool> *running; // cleared when finish() abandons the worker
        Write write;
        Now now;
        MediaLimit limit;
        std::chrono::milliseconds shutdown_wait;
        bool completion; // write log_complete after a clean drain (production)
        mutable std::mutex mutex;
        std::condition_variable changed;
        std::deque<std::string> queue;
        std::deque<std::string> parked; // state records awaiting queue space
        std::size_t parked_bytes = 0;
        std::deque<Clock::time_point> parked_at; // submission time of each parked record
        std::deque<Clock::time_point> enqueued;  // submission time of each queued entry
        Clock::duration max_lag{};              // worst submit-to-written delay
        bool writing_summary = false;   // the worker is writing summary lines
        std::size_t bytes = 0;
        std::error_code error;
        bool closed = false;
        bool exited = false;
        bool abandoned = false;
        bool chunked = false; // an oversize record is mid-admission
        bool mid_record = false; // the worker wrote a chunk that did not end its record
        bool cut = false;          // a timed-out oversize record must be ended by the worker
        std::uint64_t cut_end = 0; // ...after this many entries were written
        std::uint64_t pushed = 0;  // entries ever queued
        std::uint64_t popped = 0;  // entries ever written
        std::size_t undelivered = 0;
        bool shutdown_expired = false; // a shutdown-time wait for space ran out
        std::uint64_t shutdown_dropped = 0;
        std::uint64_t runtime_dropped = 0;
        std::uint64_t runtime_reported = 0;
        std::uint64_t state_dropped = 0;
        std::uint64_t state_reported = 0;
        Clock::time_point last_runtime_report{};
        bool runtime_reported_once = false;
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
        bool stopping() const { return running && !running->load(); }
        bool cut_ready() const { return cut && popped == cut_end; }
        // Waits for room for `charge`. Unbounded while the owner is running
        // (issue #60); bounded by `shutdown_wait` once it is not. Returns false
        // only when the shutdown bound expired with the queue still full.
        bool wait_for_space(std::unique_lock<std::mutex> &lock, std::size_t charge) {
            // A fresh record queues behind parked state records; the tail of an
            // oversize record already mid-admission (chunked) must stay contiguous.
            const auto ready = [&] {
                return error || closed || ((chunked || parked.empty()) && space(charge));
            };
            Clock::time_point deadline{};
            bool armed = false;
            while (!ready()) {
                if (!armed && stopping()) {
                    if (shutdown_expired) {
                        ++shutdown_dropped;
                        return false;
                    }
                    armed = true;
                    deadline = Clock::now() + shutdown_wait;
                }
                if (armed && Clock::now() >= deadline) {
                    shutdown_expired = true;
                    ++shutdown_dropped;
                    return false;
                }
                // Poll the running flag: stopping it does not notify this condition.
                auto wake = Clock::now() + shutdown_poll;
                if (armed && deadline < wake) { wake = deadline; }
                changed.wait_until(lock, wake);
            }
            return true;
        }
        // Moves parked state records into the queue, in order, while they fit.
        void promote_parked() {
            bool moved = false;
            while (!parked.empty() && !chunked && space(parked.front().size() + 1)) {
                push(parked.front(), parked.front().size() + 1, parked_at.front());
                parked_bytes -= parked.front().size();
                parked.pop_front();
                parked_at.pop_front();
                moved = true;
            }
            if (moved) { changed.notify_all(); }
        }
        void push(std::string_view record, std::size_t charge) { push(record, charge, now()); }
        void push(std::string_view record, std::size_t charge, Clock::time_point at) {
            enqueued.push_back(at);
            ++pushed;
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
        // Drop summaries are bookkeeping written by the worker itself, between
        // records, so they never consume queue capacity a real record needs
        // (#90 review P3). At most one of each kind per media_summary_interval
        // while running; unconditionally at shutdown.
        bool media_summary_due(Clock::time_point at, bool final) const {
            const auto pending = (drops.rate_limited - reported.rate_limited) +
                                 (drops.backpressure - reported.backpressure);
            return pending != 0 && (final || !reported_once || at - last_report >= media_summary_interval);
        }
        bool runtime_summary_due(Clock::time_point at, bool final) const {
            return runtime_dropped != runtime_reported &&
                   (final || !runtime_reported_once ||
                    at - last_runtime_report >= media_summary_interval);
        }
        bool summary_due(Clock::time_point at, bool final) const {
            return media_summary_due(at, final) || runtime_summary_due(at, final);
        }
        bool summary_pending() const {
            return drops.rate_limited != reported.rate_limited ||
                   drops.backpressure != reported.backpressure || runtime_dropped != runtime_reported;
        }
        // Formats (and marks reported) every due summary.
        std::string take_summaries(Clock::time_point at, bool final) {
            std::string out;
            if (media_summary_due(at, final)) {
                out += "{\"event\":\"media_telemetry_dropped\",\"rate_limited\":" +
                       std::to_string(drops.rate_limited - reported.rate_limited) +
                       ",\"backpressure\":" +
                       std::to_string(drops.backpressure - reported.backpressure) + "}\n";
                reported = drops;
                last_report = at;
                reported_once = true;
            }
            if (runtime_summary_due(at, final)) {
                out += "{\"event\":\"runtime_log_dropped\",\"backpressure\":" +
                       std::to_string(runtime_dropped - runtime_reported) + ",\"state_overflow\":" +
                       std::to_string(state_dropped - state_reported) + "}\n";
                runtime_reported = runtime_dropped;
                state_reported = state_dropped;
                last_runtime_report = at;
                runtime_reported_once = true;
            }
            return out;
        }
        // The last line of a cleanly drained log (#90 review): cumulative loss
        // counters. Consumers treat its absence as an incomplete log.
        std::string completion_line() const {
            return "{\"event\":\"log_complete\",\"runtime_dropped\":" + std::to_string(runtime_dropped) +
                   ",\"state_overflow\":" + std::to_string(state_dropped) +
                   ",\"media_rate_limited\":" + std::to_string(drops.rate_limited) +
                   ",\"media_backpressure\":" + std::to_string(drops.backpressure) +
                   ",\"shutdown_dropped\":" + std::to_string(shutdown_dropped) +
                   ",\"max_delivery_lag_ms\":" +
                   std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(max_lag).count()) +
                   "}\n";
        }
    };

    static std::error_code closed_error() { return {ESHUTDOWN, std::generic_category()}; }
    static std::error_code shutdown_error() { return {ETIMEDOUT, std::generic_category()}; }
    std::error_code error_snapshot() const {
        const std::lock_guard lock(state_->mutex);
        // Keep shutdown loss out of the sink-error latch so admitted records
        // can still drain and a real sink error retains precedence.
        if (state_->error) { return state_->error; }
        return state_->shutdown_dropped ? shutdown_error() : std::error_code{};
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
    // Writes `text` fully; false after latching a sink error.
    static bool write_all(const std::shared_ptr<State> &state, std::string_view text) {
        std::size_t offset = 0;
        while (offset < text.size()) {
            const auto count = state->write(json_fd(), text.data() + offset, text.size() - offset);
            if (count < 0 && errno == EINTR) { continue; }
            if (count <= 0) { fail(state, count == 0 ? EIO : errno); return false; }
            offset += static_cast<std::size_t>(count);
        }
        return true;
    }
    // Writes due summaries outside the lock; `lock` is held on entry and exit.
    static bool write_summaries(const std::shared_ptr<State> &state, std::unique_lock<std::mutex> &lock,
                                bool final) {
        // Never between the chunks of one record: the summary would split it.
        if (state->mid_record) { return true; }
        auto lines = state->take_summaries(state->now(), final);
        if (lines.empty()) { return true; }
        state->writing_summary = true;
        lock.unlock();
        const bool ok = write_all(state, lines);
        lock.lock();
        state->writing_summary = false;
        state->changed.notify_all();
        return ok;
    }
    static void drain(const std::shared_ptr<State> &state) {
        for (;;) {
            std::unique_lock lock(state->mutex);
            while (!state->closed && state->queue.empty() && !state->cut_ready()) {
                // A record parked behind a chunked admission that has since
                // ended (or timed out) must not be stranded on an idle queue.
                state->promote_parked();
                if (!state->queue.empty()) { break; }
                if (!write_summaries(state, lock, false)) { return; }
                if (state->closed || !state->queue.empty() || state->cut_ready()) { break; }
                // A throttled summary is retried once its interval can elapse.
                if (state->summary_pending()) {
                    state->changed.wait_for(lock, media_summary_interval);
                } else {
                    state->changed.wait(lock);
                }
            }
            if (state->cut_ready()) {
                // Every chunk of a record cut short by a timed-out tail is out:
                // end its line (its loss is already counted) before anything else.
                state->cut = false;
                if (state->mid_record) {
                    state->mid_record = false;
                    lock.unlock();
                    if (!write_all(state, "\n")) { return; }
                }
                continue;
            }
            state->promote_parked();
            if (state->queue.empty()) {
                // Closed and drained: final summaries, then the completion record.
                if (state->mid_record) {
                    // Shutdown cut an oversize record short: end the partial line
                    // so the lines after it parse, and count the record as lost.
                    ++state->shutdown_dropped;
                    state->mid_record = false;
                    lock.unlock();
                    const bool ended = write_all(state, "\n");
                    lock.lock();
                    if (!ended) { return; }
                }
                if (!write_summaries(state, lock, true)) { return; }
                // finish() may have given up while the summaries were written.
                if (!state->completion || state->abandoned || state->error) { return; }
                const auto complete = state->completion_line();
                lock.unlock();
                (void)write_all(state, complete);
                return;
            }
            // The deque keeps element addresses stable; the front stays charged
            // (and queued) while it is being written.
            const std::string_view front = state->queue.front();
            lock.unlock();
            if (!write_all(state, front)) { return; }
            lock.lock();
            state->mid_record = front.empty() || front.back() != '\n';
            state->bytes -= front.size();
            state->queue.pop_front();
            ++state->popped;
            state->max_lag = std::max(state->max_lag, state->now() - state->enqueued.front());
            state->enqueued.pop_front();
            if (state->abandoned) {
                state->changed.notify_all();
                return;
            }
            state->promote_parked();
            state->changed.notify_all();
            if (!write_summaries(state, lock, false)) { return; }
            if (state->abandoned) { return; }
        }
    }

    std::shared_ptr<State> state_;
    std::timed_mutex producer_;
    std::mutex finishing_;
    std::thread worker_;
};

inline std::atomic<StdoutLog *> active_stdout_log{nullptr};

// Appends the emission time ("emit_ns": steady_clock, i.e. CLOCK_MONOTONIC on
// Linux, the clock the QA runners stamp with) to a JSON object record.
inline std::string with_emit_ns(std::string_view json,
                                StdoutLog::Clock::time_point at = StdoutLog::Clock::now()) {
    if (json.size() < 2 || json.front() != '{' || json.back() != '}') { return std::string{json}; }
    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(at.time_since_epoch()).count();
    std::string out{json.substr(0, json.size() - 1)};
    if (json.size() > 2) { out += ','; }
    out += "\"emit_ns\":" + std::to_string(ns) + "}";
    return out;
}

class StdoutLogScope final {
public:
    // The production receiver's log always ends with its completion record.
    explicit StdoutLogScope(std::atomic<bool> &running)
        : log(running, ::write, &StdoutLog::Clock::now, StdoutLog::MediaLimit(),
              StdoutLog::default_finish_timeout, /*completion_record=*/true) {
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
} // namespace aa::diagnostics
