// SPDX-License-Identifier: GPL-3.0-or-later
#include <aa/core/SessionThread.hpp>

#include <utility>

namespace aa::core {

SessionThread::SessionThread() : owner_([this] { run(); }) {}

SessionThread::~SessionThread() {
    request_stop();
    join();
}

bool SessionThread::is_owner_thread() const noexcept {
    return std::this_thread::get_id() == owner_.get_id();
}

bool SessionThread::post(std::function<void()> task) {
    {
        std::scoped_lock lock{mutex_};
        if (stop_requested_) {
            return false;
        }
        queue_.push_back(std::move(task));
    }
    wakeup_.notify_one();
    return true;
}

void SessionThread::request_stop() noexcept {
    {
        std::scoped_lock lock{mutex_};
        stop_requested_ = true;
    }
    stop_source_.request_stop();
    wakeup_.notify_all();
}

void SessionThread::join() {
    if (owner_.joinable()) {
        owner_.join();
    }
}

bool SessionThread::stopped() const noexcept {
    std::scoped_lock lock{mutex_};
    return stop_requested_;
}

CancellationToken SessionThread::token() const noexcept {
    return stop_source_.get_token();
}

void SessionThread::run() noexcept {
    std::unique_lock lock{mutex_};
    while (true) {
        wakeup_.wait(lock, [this] { return stop_requested_ || !queue_.empty(); });
        if (stop_requested_) {
            return;
        }
        auto task = std::move(queue_.front());
        queue_.pop_front();
        lock.unlock();
        task();
        lock.lock();
    }
}

} // namespace aa::core
