// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <aa/core/Cancellation.hpp>
#include <aa/core/Result.hpp>
#include <aa/transport/Transport.hpp>

#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace aa::transport {

// Real TCP transport (task 16): moves complete AAP frames over a TCP socket.
// The socket and all POSIX headers live behind a private impl; this public
// header exposes no external type. The frame stream is delimited and bounded by
// the shared AAP frame codec, so a malformed or oversize inbound frame is a
// typed error that closes the channel safely.
//
// A TCP server listens with TcpListener and accepts one peer; a client connects
// with TcpTransport::connect. make_tcp_loopback() opens a connected pair over a
// real 127.0.0.1 socket for tests.
class TcpTransport final : public Transport {
public:
    // Connect to a TCP peer. The returned transport owns the socket.
    [[nodiscard]] static core::Result<std::unique_ptr<TcpTransport>>
    connect(const char* host, std::uint16_t port);

    ~TcpTransport() override;

    [[nodiscard]] Kind kind() const noexcept override { return Kind::tcp; }

    core::Result<void> open(core::CancellationToken cancellation) override;
    core::Result<void> send(std::span<const std::byte> frame) override;
    [[nodiscard]] core::Result<std::vector<std::byte>> receive() override;
    void close() noexcept override;

private:
    friend class TcpListener;

    struct Impl;
    explicit TcpTransport(std::unique_ptr<Impl> impl) noexcept;

    std::unique_ptr<Impl> impl_;
    core::CancellationToken token_{};
};

// A TCP listener bound to host:port (use port 0 for an ephemeral bound port).
class TcpListener final {
public:
    [[nodiscard]] static core::Result<std::unique_ptr<TcpListener>>
    listen(const char* host, std::uint16_t port);

    ~TcpListener();

    [[nodiscard]] std::uint16_t port() const noexcept;
    [[nodiscard]] core::Result<std::unique_ptr<TcpTransport>> accept();
    void close() noexcept;

private:
    struct Impl;
    explicit TcpListener(std::unique_ptr<Impl> impl) noexcept;

    std::unique_ptr<Impl> impl_;
};

// A connected pair of TCP transports over a real 127.0.0.1 loopback socket.
struct TcpLoopbackPair final {
    std::unique_ptr<TcpTransport> server;
    std::unique_ptr<TcpTransport> client;
};

[[nodiscard]] core::Result<TcpLoopbackPair> make_tcp_loopback();

} // namespace aa::transport
