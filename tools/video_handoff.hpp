// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Supervisor -> managed wired worker video-endpoint handoff over an inherited
// AF_UNIX SOCK_SEQPACKET socketpair (fd 3 in the managed child). The supervisor
// remains the sole owner of the consumer connection; each install message passes
// duplicated (client_fd, state_fd) descriptors via SCM_RIGHTS along with the
// negotiated limits so the child borrows the socket without SHUT_WR ownership
// and preserves monotonic frame/sequence counters across worker restarts.

#include <aa/core/Result.hpp>
#include <aa/core/Time.hpp>
#include <aa/ipc/VideoChannel.hpp>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <optional>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace aa::tools {

enum class VideoHandoffKind : std::uint32_t {
    install = 1,
    clear = 2,
};

struct VideoHandoffPacket final {
    std::uint32_t kind{};
    std::uint32_t max_access_unit_bytes{};
    std::uint32_t max_datagram_payload{};
    std::uint32_t socket_budget_bytes{};
};

struct VideoHandoffEvent final {
    VideoHandoffKind kind{VideoHandoffKind::clear};
    std::optional<ipc::VideoProducer> producer{};
};

[[nodiscard]] inline bool validate_video_handoff_fd(int fd) noexcept {
    int accepting{}, type{};
    sockaddr_un address{};
    socklen_t address_size = sizeof(address);
    socklen_t size = sizeof(type);
    return fd >= 0 &&
           ::getsockname(fd, reinterpret_cast<sockaddr*>(&address), &address_size) == 0 &&
           address.sun_family == AF_UNIX &&
           ::getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &size) == 0 &&
           type == SOCK_SEQPACKET &&
           ::getsockopt(fd, SOL_SOCKET, SO_ACCEPTCONN, &accepting, &size) == 0 && !accepting &&
           ::fcntl(fd, F_SETFD, FD_CLOEXEC) == 0;
}

[[nodiscard]] inline bool send_video_handoff_install(int channel_fd, int client_fd, int state_fd,
                                                     const ipc::NegotiatedLimits& limits) noexcept {
    if (channel_fd < 0 || client_fd < 0 || state_fd < 0) {
        return false;
    }
    VideoHandoffPacket packet{
        static_cast<std::uint32_t>(VideoHandoffKind::install),
        limits.max_access_unit_bytes,
        limits.max_datagram_payload,
        limits.socket_budget_bytes,
    };
    iovec iov{&packet, sizeof(packet)};
    alignas(cmsghdr) std::array<char, CMSG_SPACE(sizeof(int) * 2)> control{};
    msghdr msg{};
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = control.data();
    msg.msg_controllen = control.size();
    auto* cmsg = CMSG_FIRSTHDR(&msg);
    if (cmsg == nullptr) {
        return false;
    }
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN(sizeof(int) * 2);
    const std::array<int, 2> fds{client_fd, state_fd};
    std::memcpy(CMSG_DATA(cmsg), fds.data(), sizeof(fds));
    ssize_t sent;
    do {
        sent = ::sendmsg(channel_fd, &msg, MSG_NOSIGNAL | MSG_DONTWAIT);
    } while (sent < 0 && errno == EINTR);
    return sent == static_cast<ssize_t>(sizeof(packet));
}

[[nodiscard]] inline bool send_video_handoff_clear(int channel_fd) noexcept {
    if (channel_fd < 0) {
        return false;
    }
    VideoHandoffPacket packet{static_cast<std::uint32_t>(VideoHandoffKind::clear), 0, 0, 0};
    iovec iov{&packet, sizeof(packet)};
    msghdr msg{};
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    ssize_t sent;
    do {
        sent = ::sendmsg(channel_fd, &msg, MSG_NOSIGNAL | MSG_DONTWAIT);
    } while (sent < 0 && errno == EINTR);
    return sent == static_cast<ssize_t>(sizeof(packet));
}

[[nodiscard]] inline core::Result<VideoHandoffEvent> receive_video_handoff(
    int channel_fd, core::Milliseconds timeout) {
    if (channel_fd < 0) {
        return Error{ErrorCode::transport_closed};
    }
    pollfd ready{channel_fd, static_cast<short>(POLLIN | POLLRDHUP), 0};
    if (timeout.count < 0) {
        for (;;) {
            const int status = ::poll(&ready, 1, -1);
            if (status < 0 && errno == EINTR) continue;
            if (status < 0) return Error{ErrorCode::transport_io};
            break;
        }
    } else {
        const auto bounded = timeout.count > 60000 ? 60000 : timeout.count;
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::milliseconds{bounded};
        for (bool initial = true;; initial = false) {
            if (!initial && std::chrono::steady_clock::now() >= deadline) {
                return Error{ErrorCode::timeout};
            }
            int wait = 0;
            if (bounded > 0) {
                const auto remaining = deadline - std::chrono::steady_clock::now();
                const auto nanos =
                    std::chrono::duration_cast<std::chrono::nanoseconds>(remaining).count();
                wait = static_cast<int>(std::clamp<std::int64_t>((nanos + 999'999) / 1'000'000, 1, 60'000));
            }
            const int status = ::poll(&ready, 1, wait);
            if (status < 0 && errno == EINTR) continue;
            if (status < 0) return Error{ErrorCode::transport_io};
            if (status == 0) return Error{ErrorCode::timeout};
            break;
        }
    }

    VideoHandoffPacket packet{};
    iovec iov{&packet, sizeof(packet)};
    alignas(cmsghdr) std::array<char, CMSG_SPACE(sizeof(int) * 4)> control{};
    msghdr msg{};
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = control.data();
    msg.msg_controllen = control.size();
    ssize_t received;
    do {
        received = ::recvmsg(channel_fd, &msg, MSG_CMSG_CLOEXEC);
    } while (received < 0 && errno == EINTR);

    std::array<int, 4> received_fds{-1, -1, -1, -1};
    std::size_t fd_count = 0;
    for (auto* cmsg = CMSG_FIRSTHDR(&msg); cmsg != nullptr; cmsg = CMSG_NXTHDR(&msg, cmsg)) {
        if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS) {
            const std::size_t bytes =
                cmsg->cmsg_len >= CMSG_LEN(0) ? (cmsg->cmsg_len - CMSG_LEN(0)) : 0;
            const std::size_t count = bytes / sizeof(int);
            const auto* src = reinterpret_cast<const int*>(CMSG_DATA(cmsg));
            for (std::size_t i = 0; i < count; ++i) {
                if (fd_count < received_fds.size()) {
                    received_fds[fd_count++] = src[i];
                } else {
                    ::close(src[i]);
                }
            }
        }
    }
    const auto close_all = [&] {
        for (std::size_t i = 0; i < fd_count; ++i) {
            if (received_fds[i] >= 0) {
                ::close(received_fds[i]);
                received_fds[i] = -1;
            }
        }
    };

    if (received < 0) {
        close_all();
        return Error{ErrorCode::transport_io};
    }
    if (received == 0) {
        close_all();
        return Error{ErrorCode::transport_closed};
    }
    if ((msg.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) != 0 ||
        received != static_cast<ssize_t>(sizeof(packet))) {
        close_all();
        return Error{ErrorCode::transport_io};
    }
    if (packet.kind == static_cast<std::uint32_t>(VideoHandoffKind::clear)) {
        close_all();
        return VideoHandoffEvent{VideoHandoffKind::clear, std::nullopt};
    }
    if (packet.kind != static_cast<std::uint32_t>(VideoHandoffKind::install) || fd_count != 2) {
        close_all();
        return Error{ErrorCode::transport_io};
    }
    const ipc::NegotiatedLimits limits{
        packet.max_access_unit_bytes,
        packet.max_datagram_payload,
        packet.socket_budget_bytes,
    };
    auto adopted = ipc::VideoProducer::adopt_accepted(received_fds[0], limits, received_fds[1]);
    if (!adopted) {
        close_all();
        return adopted.error();
    }
    return VideoHandoffEvent{VideoHandoffKind::install, std::move(adopted.value())};
}

} // namespace aa::tools
