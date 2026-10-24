// SPDX-License-Identifier: GPL-3.0-or-later
// Real TCP transport (task 16): complete AAP frames over a TCP socket, with the
// socket hidden behind a private impl. The frame stream is delimited and bounded
// by the shared AAP frame codec, so malformed or oversize inbound frames are
// typed errors that close the channel safely.

#include <aa/transport/TcpTransport.hpp>

#include "FrameStreamIO.hpp"
#include "socket_util.hpp"
#include "TcpIo.hpp"

#include <utility>
#include <fcntl.h>

namespace aa::transport {

namespace su = socket_util;

struct TcpTransport::Impl final {
    int fd;
    TcpIo bounded;
    FrameStreamIO io;

    explicit Impl(int socket_fd) noexcept
        : fd(socket_fd),
          io([this](std::span<std::byte> buffer, const core::CancellationToken& token) {
              return bounded.read(this->fd, buffer, token);
          },
             [this](std::span<const std::byte> data, const core::CancellationToken& token) {
                  return bounded.write(this->fd, data, token);
             }) {}
};

TcpTransport::TcpTransport(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

TcpTransport::~TcpTransport() { close(); }

void TcpTransport::set_io_deadline(core::Deadline::Clock::time_point until) noexcept {
    impl_->bounded.until = until;
}
void TcpTransport::set_io_checkpoint(std::function<void()> checkpoint) {
    impl_->bounded.checkpoint = std::move(checkpoint);
}

core::Result<std::unique_ptr<TcpTransport>>
TcpTransport::connect(const char* host, std::uint16_t port) {
    const int fd = su::connect_socket(host, port);
    if (fd < 0) {
        return Error{ErrorCode::transport_unavailable};
    }
    return std::unique_ptr<TcpTransport>(new TcpTransport(std::make_unique<Impl>(fd)));
}

core::Result<void> TcpTransport::open(core::CancellationToken cancellation) {
    if (!impl_ || impl_->fd < 0) {
        return Error{ErrorCode::transport_closed};
    }
    token_ = std::move(cancellation);
    return {};
}

core::Result<void> TcpTransport::send(std::span<const std::byte> frame) {
    if (!impl_) {
        return Error{ErrorCode::transport_closed};
    }
    auto sent = impl_->io.send_frame(frame, token_);
    if (!sent) {
        close();
    }
    return sent;
}

core::Result<std::vector<std::byte>> TcpTransport::receive() {
    if (!impl_) {
        return Error{ErrorCode::transport_closed};
    }
    auto got = impl_->io.receive_frame(token_);
    if (!got) {
        close();
    }
    return got;
}

void TcpTransport::close() noexcept {
    if (impl_) {
        impl_->io.close();
        su::close_fd(impl_->fd);
    }
}

struct TcpListener::Impl final {
    int fd;
    explicit Impl(int socket_fd) noexcept : fd(socket_fd) {}
};

TcpListener::TcpListener(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

TcpListener::~TcpListener() { close(); }

core::Result<std::unique_ptr<TcpListener>>
TcpListener::listen(const char* host, std::uint16_t port) {
    const int fd = su::listen_socket(host, port);
    if (fd < 0) {
        return Error{ErrorCode::transport_unavailable};
    }
    const int flags = ::fcntl(fd, F_GETFL);
    if (flags < 0 || ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        ::close(fd);
        return Error{ErrorCode::transport_unavailable};
    }
    return std::unique_ptr<TcpListener>(new TcpListener(std::make_unique<Impl>(fd)));
}

std::uint16_t TcpListener::port() const noexcept {
    return impl_ ? su::bound_port(impl_->fd) : 0;
}

core::Result<std::unique_ptr<TcpTransport>> TcpListener::accept() {
    return accept(core::Deadline{});
}

core::Result<std::unique_ptr<TcpTransport>>
TcpListener::accept(const core::Deadline& deadline, std::function<void()> checkpoint) {
    if (!impl_ || impl_->fd < 0) {
        return Error{ErrorCode::transport_closed};
    }
    const TcpIo bounded{deadline.until, std::move(checkpoint)};
    for (;;) {
        if (auto ready = bounded.wait(impl_->fd, POLLIN, deadline.cancellation); !ready) return ready.error();
        const int fd = ::accept4(impl_->fd, nullptr, nullptr, SOCK_CLOEXEC | SOCK_NONBLOCK);
        if (fd >= 0) {
            return std::unique_ptr<TcpTransport>(new TcpTransport(std::make_unique<TcpTransport::Impl>(fd)));
        }
        if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK) return Error{ErrorCode::transport_io};
    }
}

void TcpListener::close() noexcept {
    if (impl_) {
        su::close_fd(impl_->fd);
    }
}

core::Result<TcpLoopbackPair> make_tcp_loopback() {
    auto listener = TcpListener::listen("127.0.0.1", 0);
    if (!listener) {
        return listener.error();
    }
    const std::uint16_t port = listener.value()->port();
    if (port == 0) {
        return Error{ErrorCode::transport_unavailable};
    }
    auto client = TcpTransport::connect("127.0.0.1", port);
    if (!client) {
        return client.error();
    }
    auto server = listener.value()->accept();
    if (!server) {
        return server.error();
    }
    TcpLoopbackPair pair;
    pair.server = std::move(server).value();
    pair.client = std::move(client).value();
    return pair;
}

} // namespace aa::transport
