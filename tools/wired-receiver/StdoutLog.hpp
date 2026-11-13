// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <pthread.h>
#include <signal.h>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <unistd.h>
#include <utility>

namespace aa::wired_receiver {
class StdoutLog final {
public:
    static constexpr std::size_t byte_capacity = 1'048'576;
    static constexpr std::size_t entry_capacity = 4096;
    using Write = ssize_t (*)(int, const void *, std::size_t);

    explicit StdoutLog(std::atomic<bool> &running, Write write = ::write)
        : running_(running), write_(write), worker_([this] { run(); }) {}
    // Owners check finish explicitly; unwinding still must join the worker.
    ~StdoutLog() { [[maybe_unused]] const auto error = finish(); }
    StdoutLog(const StdoutLog &) = delete;
    StdoutLog &operator=(const StdoutLog &) = delete;

    std::error_code submit(std::string_view record) {
        const std::lock_guard producer(producer_);
        std::size_t offset = 0;
        for (;;) {
            const auto count = std::min(byte_capacity, record.size() - offset);
            const bool newline = count < byte_capacity && offset + count == record.size();
            const auto charge = count + static_cast<std::size_t>(newline);
            std::unique_lock lock(queue_mutex_);
#ifdef AA_WIRED_RECEIVER_SHUTDOWN_TEST
            waiting_ = !space(charge) && !error_ && !closed_;
            changed_.notify_all();
#endif
            changed_.wait(lock, [&] { return error_ || closed_ || space(charge); });
#ifdef AA_WIRED_RECEIVER_SHUTDOWN_TEST
            waiting_ = false;
#endif
            if (error_) { return error_; }
            if (closed_) { return {ESHUTDOWN, std::generic_category()}; }
            // Allocate only after capacity admission; the front stays charged during I/O.
            queue_.emplace_back(charge, '\n');
            record.substr(offset, count).copy(queue_.back().data(), count);
            bytes_ += charge;
            changed_.notify_all();
            offset += count;
            if (newline) { return {}; }
        }
    }
    std::error_code flush() {
        const std::lock_guard producer(producer_);
        std::unique_lock lock(queue_mutex_);
#ifdef AA_WIRED_RECEIVER_SHUTDOWN_TEST
        flushing_ = !queue_.empty() && !error_;
        changed_.notify_all();
#endif
        changed_.wait(lock, [&] { return queue_.empty() || error_; });
#ifdef AA_WIRED_RECEIVER_SHUTDOWN_TEST
        flushing_ = false;
#endif
        return error_;
    }
    std::error_code finish() {
        const std::lock_guard finishing(finishing_);
        const std::lock_guard producer(producer_);
        {
            const std::lock_guard lock(queue_mutex_);
            closed_ = true;
            changed_.notify_all();
        }
        if (worker_.joinable()) { worker_.join(); }
        const std::lock_guard lock(queue_mutex_);
        return error_;
    }
#ifdef AA_WIRED_RECEIVER_SHUTDOWN_TEST
    bool wait_for_capacity_wait() {
        std::unique_lock lock(queue_mutex_);
        return changed_.wait_for(lock, std::chrono::seconds{5}, [&] { return waiting_; });
    }
    std::pair<std::size_t, std::size_t> outstanding() {
        const std::lock_guard lock(queue_mutex_);
        return {bytes_, queue_.size()};
    }
    bool wait_for_barrier(bool finishing) {
        std::unique_lock lock(queue_mutex_);
        return changed_.wait_for(lock, std::chrono::seconds{5},
                                 [&] { return finishing ? closed_ : flushing_; });
    }
#endif
private:
    bool space(std::size_t charge) const {
        return charge <= byte_capacity - bytes_ && queue_.size() < entry_capacity;
    }
    void fail(int error) {
        {
            const std::lock_guard lock(queue_mutex_);
            if (error_) { return; }
            error_ = {error, std::generic_category()};
            running_.store(false);
            changed_.notify_all();
        }
        char message[64];
        const int size = std::snprintf(message, sizeof(message), "stdout_log errno=%d\n", error);
        if (size > 0) {
            // A failed/short best-effort diagnostic must not replace the latched stdout error.
            if (::write(STDERR_FILENO, message, static_cast<std::size_t>(size)) != size) { return; }
        }
    }
    void run() {
        sigset_t mask;
        ::sigemptyset(&mask);
        ::sigaddset(&mask, SIGPIPE);
        if (const int error = ::pthread_sigmask(SIG_BLOCK, &mask, nullptr); error != 0) {
            fail(error);
            return;
        }
        for (;;) {
            std::unique_lock lock(queue_mutex_);
            changed_.wait(lock, [&] { return closed_ || !queue_.empty(); });
            if (queue_.empty()) { return; }
            const std::string_view front = queue_.front();
            lock.unlock();
            std::size_t offset = 0;
            while (offset < front.size()) {
                const auto count = write_(STDOUT_FILENO, front.data() + offset, front.size() - offset);
                if (count < 0 && errno == EINTR) { continue; }
                if (count <= 0) { fail(count == 0 ? EIO : errno); return; }
                offset += static_cast<std::size_t>(count);
            }
            lock.lock();
            bytes_ -= front.size();
            queue_.pop_front();
            changed_.notify_all();
        }
    }
    std::atomic<bool> &running_;
    Write write_;
    std::mutex producer_, queue_mutex_, finishing_;
    std::condition_variable changed_;
    std::deque<std::string> queue_;
    std::size_t bytes_ = 0;
    std::error_code error_;
    bool closed_ = false;
#ifdef AA_WIRED_RECEIVER_SHUTDOWN_TEST
    bool waiting_ = false;
    bool flushing_ = false;
#endif
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
