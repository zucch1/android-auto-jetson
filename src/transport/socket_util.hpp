// SPDX-License-Identifier: GPL-3.0-or-later
// Shared POSIX socket helpers for the TCP and TLS transports (task 16). Private
// to src/transport/ - not a public header. No external library type here, only
// POSIX sockets.
#pragma once

#include <aa/core/Cancellation.hpp>
#include <aa/core/Result.hpp>

#include <cstddef>
#include <span>
#include <string>

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>

namespace aa::transport::socket_util {

namespace detail {
// Short poll slice so a blocked read observes cancellation promptly without a
// dedicated wake-up descriptor.
inline constexpr int kPollSliceMs = 50;
} // namespace

[[nodiscard]] inline core::Result<std::size_t>
read_fd(int fd, std::span<std::byte> buffer, const core::CancellationToken& token) {
    for (;;) {
        if (token.stop_requested()) {
            return Error{ErrorCode::cancelled};
        }
        ::pollfd descriptor{fd, POLLIN, 0};
        const int ready = ::poll(&descriptor, 1, detail::kPollSliceMs);
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            return Error{ErrorCode::transport_io};
        }
        if (ready == 0) {
            continue;
        }
        if ((descriptor.revents & POLLNVAL) != 0) {
            return Error{ErrorCode::transport_io};
        }
        if ((descriptor.revents & POLLERR) != 0) {
            return Error{ErrorCode::transport_io};
        }
        if ((descriptor.revents & POLLHUP) != 0 && (descriptor.revents & POLLIN) == 0) {
            return std::size_t{0}; // peer closed: end-of-stream
        }
        // MSG_DONTWAIT: never block after poll; a concurrent drain returns
        // EAGAIN and the loop re-polls instead of hanging under backpressure.
        const ssize_t count = ::recv(fd, buffer.data(), buffer.size(), MSG_DONTWAIT);
        if (count < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
                continue;
            }
            return Error{ErrorCode::transport_io};
        }
        return static_cast<std::size_t>(count);
    }
}

[[nodiscard]] inline core::Result<std::size_t>
write_fd(int fd, std::span<const std::byte> data, const core::CancellationToken& token) {
    for (;;) {
        if (token.stop_requested()) {
            return Error{ErrorCode::cancelled};
        }
        ::pollfd descriptor{fd, POLLOUT, 0};
        const int ready = ::poll(&descriptor, 1, detail::kPollSliceMs);
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            return Error{ErrorCode::transport_io};
        }
        if (ready == 0) {
            continue;
        }
        if ((descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
            // Peer is gone or the fd is dead: surface a typed error instead of
            // spinning on a permanently-writable-with-error descriptor.
            return Error{ErrorCode::transport_io};
        }
        // MSG_DONTWAIT: never block after poll under partial space/backpressure
        // (the send is interruptible). MSG_NOSIGNAL: a write to a closed peer
        // returns EPIPE instead of raising SIGPIPE and killing the process.
        const ssize_t count = ::send(fd, data.data(), data.size(), MSG_DONTWAIT | MSG_NOSIGNAL);
        if (count < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
                continue;
            }
            return Error{ErrorCode::transport_io};
        }
        return static_cast<std::size_t>(count);
    }
}

inline void close_fd(int& fd) noexcept {
    if (fd >= 0) {
        ::close(fd);
        fd = -1;
    }
}

// Resolve host:port and connect a TCP socket. Returns the connected fd or -1.
[[nodiscard]] inline int connect_socket(const char* host, std::uint16_t port) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* result = nullptr;
    const std::string service = std::to_string(port);
    if (::getaddrinfo(host, service.c_str(), &hints, &result) != 0) {
        return -1;
    }
    int fd = -1;
    for (addrinfo* ai = result; ai != nullptr; ai = ai->ai_next) {
        fd = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) {
            continue;
        }
        if (::connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) {
            break;
        }
        ::close(fd);
        fd = -1;
    }
    ::freeaddrinfo(result);
    return fd;
}

// Bind + listen on host:port (port 0 for ephemeral). Returns the listening fd or -1.
[[nodiscard]] inline int listen_socket(const char* host, std::uint16_t port) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;
    addrinfo* result = nullptr;
    const std::string service = std::to_string(port);
    if (::getaddrinfo(host, service.c_str(), &hints, &result) != 0) {
        return -1;
    }
    int fd = -1;
    for (addrinfo* ai = result; ai != nullptr; ai = ai->ai_next) {
        fd = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) {
            continue;
        }
        int reuse = 1;
        ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
        if (::bind(fd, ai->ai_addr, ai->ai_addrlen) == 0 && ::listen(fd, 4) == 0) {
            break;
        }
        ::close(fd);
        fd = -1;
    }
    ::freeaddrinfo(result);
    return fd;
}

// The port a listening socket is bound to (0 on failure).
[[nodiscard]] inline std::uint16_t bound_port(int fd) noexcept {
    if (fd < 0) {
        return 0;
    }
    sockaddr_storage address{};
    socklen_t length = sizeof(address);
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&address), &length) != 0) {
        return 0;
    }
    if (address.ss_family == AF_INET) {
        return ntohs(reinterpret_cast<sockaddr_in*>(&address)->sin_port);
    }
    if (address.ss_family == AF_INET6) {
        return ntohs(reinterpret_cast<sockaddr_in6*>(&address)->sin6_port);
    }
    return 0;
}

// Accept one connection on a listening fd. Returns the connected fd or -1.
[[nodiscard]] inline int accept_socket(int listen_fd) {
    if (listen_fd < 0) {
        return -1;
    }
    for (;;) {
        const int fd = ::accept(listen_fd, nullptr, nullptr);
        if (fd < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        return fd;
    }
}

} // namespace aa::transport::socket_util
