// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Bounded blocking socket reads/writes for the D-Bus client adapter (task 21).
// Private to src/ipc/dbus_adapter - not a public header. Poll-sliced with a
// fixed timeout so a dead bus fails with a typed error instead of hanging.

#include <aa/core/Result.hpp>

#include <cstddef>
#include <span>

#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>

namespace aa::ipc::dbus_adapter::bus_io {

inline constexpr int kIoTimeoutMs = 2000;

[[nodiscard]] inline core::Result<void> read_full(int fd, std::span<std::uint8_t> out) {
    std::size_t filled = 0;
    while (filled < out.size()) {
        ::pollfd descriptor{fd, POLLIN, 0};
        const int ready = ::poll(&descriptor, 1, kIoTimeoutMs);
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            return Error{ErrorCode::transport_io};
        }
        if (ready == 0) {
            return Error{ErrorCode::transport_io};
        }
        const ssize_t count = ::recv(fd, out.data() + filled, out.size() - filled, 0);
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

[[nodiscard]] inline core::Result<void> write_full(int fd, std::span<const std::uint8_t> data) {
    std::size_t sent = 0;
    while (sent < data.size()) {
        ::pollfd descriptor{fd, POLLOUT, 0};
        const int ready = ::poll(&descriptor, 1, kIoTimeoutMs);
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
            ::send(fd, data.data() + sent, data.size() - sent, MSG_NOSIGNAL);
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
