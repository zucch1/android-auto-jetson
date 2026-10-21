// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Bounded blocking socket reads/writes for the D-Bus client adapter (task 21).
// Private to src/ipc/dbus_adapter - not a public header. Poll-sliced with a
// fixed timeout so a dead bus fails with a typed error instead of hanging.

#include <aa/core/Result.hpp>

#include <cstddef>
#include <span>
#include <chrono>
#include <algorithm>
#include <limits>

#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>

namespace aa::ipc::dbus_adapter::bus_io {

inline constexpr int kIoTimeoutMs = 2000;
using Deadline = std::chrono::steady_clock::time_point;
inline Deadline deadline() { return std::chrono::steady_clock::now() + std::chrono::milliseconds(kIoTimeoutMs); }
inline int remaining(Deadline until) {
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(until - std::chrono::steady_clock::now()).count();
    return static_cast<int>(std::clamp<std::int64_t>(ms, 0, std::numeric_limits<int>::max()));
}

[[nodiscard]] inline core::Result<void> read_full(int fd, std::span<std::uint8_t> out, Deadline until = deadline()) {
    std::size_t filled = 0;
    while (filled < out.size()) {
        if (std::chrono::steady_clock::now() >= until) return Error{ErrorCode::timeout};
        ::pollfd descriptor{fd, POLLIN, 0};
        const int ready = ::poll(&descriptor, 1, remaining(until));
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            return Error{ErrorCode::transport_io};
        }
        if (ready == 0) {
            return Error{ErrorCode::transport_io};
        }
        const ssize_t count = ::recv(fd, out.data() + filled, out.size() - filled, MSG_DONTWAIT);
        if (count < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
                continue;
            }
            return Error{ErrorCode::transport_io};
        }
        if (count == 0) {
            return Error{ErrorCode::transport_closed};
        }
        filled += static_cast<std::size_t>(count);
    }
    return {};
}

[[nodiscard]] inline core::Result<void> write_full(int fd, std::span<const std::uint8_t> data, Deadline until = deadline()) {
    std::size_t sent = 0;
    while (sent < data.size()) {
        if (std::chrono::steady_clock::now() >= until) return Error{ErrorCode::timeout};
        ::pollfd descriptor{fd, POLLOUT, 0};
        const int ready = ::poll(&descriptor, 1, remaining(until));
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            return Error{ErrorCode::transport_io};
        }
        if (ready == 0) {
            return Error{ErrorCode::transport_io};
        }
        const ssize_t count =
             ::send(fd, data.data() + sent, data.size() - sent, MSG_NOSIGNAL | MSG_DONTWAIT);
        if (count < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
                continue;
            }
            return Error{ErrorCode::transport_io};
        }
        sent += static_cast<std::size_t>(count);
    }
    return {};
}

} // namespace aa::ipc::dbus_adapter::bus_io
