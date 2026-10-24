// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <aa/core/Deadline.hpp>
#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <functional>
#include <poll.h>
#include <span>
#include <sys/socket.h>

namespace aa::transport {
struct TcpIo final {
    core::Deadline::Clock::time_point until{core::Deadline::Clock::time_point::max()};
    std::function<void()> checkpoint;

    core::Result<void> wait(int fd, short events, const core::CancellationToken& token) const {
        for (;;) {
            if (checkpoint) checkpoint();
            const core::Deadline deadline{until, token};
            if (auto checked = deadline.check(); !checked) return checked.error();
            const auto left = std::chrono::ceil<std::chrono::milliseconds>(
                std::min(until - core::Deadline::Clock::now(),
                         std::chrono::duration_cast<core::Deadline::Clock::duration>(
                             std::chrono::milliseconds{25})));
            pollfd descriptor{fd, events, 0};
            const int ready = ::poll(&descriptor, 1, static_cast<int>(std::max<std::int64_t>(0, left.count())));
            if (ready < 0) {
                if (errno == EINTR) continue;
                return Error{ErrorCode::transport_io};
            }
            if (ready == 0) continue;
            if (checkpoint) checkpoint();
            if (auto checked = deadline.check(); !checked) return checked.error();
            if ((descriptor.revents & POLLNVAL) != 0) return Error{ErrorCode::transport_closed};
            return {};
        }
    }

    core::Result<std::size_t> read(int fd, std::span<std::byte> bytes, const core::CancellationToken& token) const {
        for (;;) {
            if (auto ready = wait(fd, POLLIN, token); !ready) return ready.error();
            const auto count = ::recv(fd, bytes.data(), bytes.size(), MSG_DONTWAIT);
            if (count >= 0) return static_cast<std::size_t>(count);
            if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK) return Error{ErrorCode::transport_io};
        }
    }

    core::Result<std::size_t> write(int fd, std::span<const std::byte> bytes, const core::CancellationToken& token) const {
        for (;;) {
            if (auto ready = wait(fd, POLLOUT, token); !ready) return ready.error();
            const auto count = ::send(fd, bytes.data(), bytes.size(), MSG_DONTWAIT | MSG_NOSIGNAL);
            if (count >= 0) return static_cast<std::size_t>(count);
            if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK) return Error{ErrorCode::transport_io};
        }
    }
};
} // namespace aa::transport
