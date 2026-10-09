// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <chrono>
#include <fstream>
#include <future>
#include <string>
#include <thread>
#include <type_traits>
#include <poll.h>
#include <sys/syscall.h>
#include <unistd.h>

// Linux host evidence: observe the worker inside poll/futex, not merely a flag
// set before it calls receive. Deadlines bound failures; no scheduling sleeps.
template<class Result>
class PendingIo final {
public:
    template<class Function>
    explicit PendingIo(Function function) {
        std::promise<long> started;
        auto identity = started.get_future();
        result = std::async(std::launch::async, [&started, function = std::move(function)] {
            started.set_value(::syscall(SYS_gettid));
            return function();
        });
        tid_ = identity.get();
    }

    bool blocked(long expected, int backpressured_fd = -1) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (std::chrono::steady_clock::now() < deadline) {
            if (result.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
                return false;
            }
            std::ifstream state("/proc/self/task/" + std::to_string(tid_) + "/syscall");
            long actual = -1;
            state >> actual;
            ::pollfd descriptor{backpressured_fd, POLLOUT, 0};
            if (actual == expected && (backpressured_fd < 0 || ::poll(&descriptor, 1, 0) == 0)) {
                return true;
            }
            std::this_thread::yield();
        }
        return false;
    }

    std::future<Result> result;

private:
    long tid_{-1};
};

template<class Function>
PendingIo(Function) -> PendingIo<std::invoke_result_t<Function>>;
