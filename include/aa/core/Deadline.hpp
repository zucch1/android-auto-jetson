// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <aa/core/Cancellation.hpp>
#include <aa/core/Result.hpp>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>

namespace aa::core {

struct Deadline final {
    using Clock = std::chrono::steady_clock;
    Clock::time_point until{Clock::now() + std::chrono::seconds{15}};
    CancellationToken cancellation{};

    [[nodiscard]] Result<void> check() const {
        if (cancellation.stop_requested()) {
            return Error{ErrorCode::cancelled};
        }
        if (Clock::now() >= until) {
            return Error{ErrorCode::timeout};
        }
        return {};
    }

    [[nodiscard]] unsigned slice_ms(unsigned maximum) const {
        if (until == Clock::time_point::max()) {
            return maximum;
        }
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                              until - Clock::now()).count();
        return static_cast<unsigned>(std::clamp<std::int64_t>(left, 1, maximum));
    }
};

// Only requests cancellation; transport close/destruction stays on the I/O owner.
class DeadlineCancellation final {
  public:
    explicit DeadlineCancellation(const Deadline &deadline)
        : parent_(deadline.cancellation, [this] { source_.request_stop(); }) {
        if (deadline.until != Deadline::Clock::time_point::max()) {
            timer_ = std::jthread([this, until = deadline.until](const std::stop_token &stop) {
                std::unique_lock lock(mutex_);
                cv_.wait_until(lock, stop, until, [] { return false; });
                if (!stop.stop_requested()) {
                    source_.request_stop();
                }
            });
        }
    }
    [[nodiscard]] CancellationToken token() const noexcept { return source_.get_token(); }

  private:
    CancellationSource source_;
    std::stop_callback<std::function<void()>> parent_;
    std::mutex mutex_;
    std::condition_variable_any cv_;
    std::jthread timer_;
};

} // namespace aa::core
