// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <aa/core/Cancellation.hpp>

#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>

namespace aa::core {

// Single-owner serialized session thread. This is the one execution context in
// which session state (state machine, channel registry, active consumer) is
// read and mutated; every other thread (transport I/O, audio, IPC adapters)
// interacts with session state exclusively by posting tasks here.
//
// Threading contract:
//  - tasks run one at a time, in FIFO order, on the owner thread; never
//    concurrently, never re-entrantly from a task onto itself;
//  - post() is safe from any thread; a task dropped by request_stop() returns
//    false and never runs (cancellation semantics: queued work is dropped, not
//    drained);
//  - posted tasks must not throw: an escaping exception terminates the process;
//  - destruction (and join()) must not happen on the owner thread.
//
// Cancellation contract: request_stop() both drops queued work and fires the
// stop token. Transports and helper calls bound to token() observe the same
// cancellation as the session thread.
class SessionThread final {
public:
    SessionThread();
    ~SessionThread();

    SessionThread(const SessionThread&) = delete;
    SessionThread& operator=(const SessionThread&) = delete;
    SessionThread(SessionThread&&) = delete;
    SessionThread& operator=(SessionThread&&) = delete;

    [[nodiscard]] bool is_owner_thread() const noexcept;

    // Enqueues work for the owner thread. Returns false (and runs nothing) once
    // stop has been requested.
    [[nodiscard]] bool post(std::function<void()> task);

    void request_stop() noexcept;
    void join();

    [[nodiscard]] bool stopped() const noexcept;
    [[nodiscard]] CancellationToken token() const noexcept;

private:
    void run() noexcept;

    mutable std::mutex mutex_;
    std::condition_variable wakeup_;
    std::deque<std::function<void()>> queue_;
    CancellationSource stop_source_;
    bool stop_requested_{false};
    std::thread owner_;
};

} // namespace aa::core
