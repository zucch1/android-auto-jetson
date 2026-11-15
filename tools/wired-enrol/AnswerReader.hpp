// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Bounded terminal answer for aa-wired-enrol (#141 B3). poll() followed by a
// blocking line read is not bounded: a writer that sends a partial line and
// keeps the descriptor open would block forever. This reader never blocks
// past one steady-clock deadline: it polls with the remaining budget before
// every read, reads whatever is available, and stops at the first newline.
//
// nullopt (a "no") on: the deadline, EOF before a newline, more than
// kMaxAnswerBytes without a newline, or a read/poll error. EINTR and EAGAIN
// are retried against the same deadline.

#include <aa/core/Time.hpp>

#include <poll.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <optional>
#include <string>

namespace aa::tools::enrol {

inline constexpr std::size_t kMaxAnswerBytes = 64;

[[nodiscard]] inline std::optional<std::string> read_answer_line(int fd, core::Milliseconds budget) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds{budget.count};
    std::string line;
    for (;;) {
        const auto left = std::chrono::ceil<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now()).count();
        if (left <= 0) return std::nullopt;
        ::pollfd input{fd, POLLIN, 0};
        const int ready = ::poll(&input, 1, static_cast<int>(left));
        if (ready < 0) {
            if (errno == EINTR) continue;
            return std::nullopt;
        }
        if (ready == 0) return std::nullopt;
        char chunk[kMaxAnswerBytes + 1];
        const ssize_t got = ::read(fd, chunk, sizeof chunk);
        if (got < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
            return std::nullopt;
        }
        if (got == 0) return std::nullopt;  // EOF before a newline
        for (ssize_t index = 0; index < got; ++index) {
            if (chunk[index] == '\n') return line;
            if (line.size() >= kMaxAnswerBytes) return std::nullopt;
            line.push_back(chunk[index]);
        }
    }
}

} // namespace aa::tools::enrol
