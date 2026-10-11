// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <aa/ipc/VideoChannel.hpp>

#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

namespace videotest {

inline std::string private_runtime_dir() {
    char pattern[] = "/tmp/aa-video-XXXXXX";
    const char* dir = ::mkdtemp(pattern);
    return dir == nullptr ? std::string{} : std::string{dir};
}

// Binds (but never listens) a seqpacket socket node at `path` and closes the
// descriptor, leaving a verified-stale endpoint behind.
inline bool leave_stale_socket_node(const std::string& path) {
    const int fd = ::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        return false;
    }
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
    const bool bound = ::bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0;
    ::close(fd);
    return bound;
}

// One full handshake plus a single access unit round trip against a live
// server; the recovery assertion shared by the channel and ownership suites.
inline bool streams_one_unit(aa::ipc::VideoServer& server, const aa::ipc::VideoSocketPath& path) {
    aa::core::Result<aa::ipc::VideoConsumer> client{aa::ipc::VideoConsumer{}};
    std::thread connector([&] {
        client = aa::ipc::connect_video_consumer(
            path, aa::ipc::VideoClientOptions{{}, aa::ipc::current_uid(),
                                              aa::core::Milliseconds{2000}});
    });
    auto producer = server.accept_consumer(aa::core::Milliseconds{2000});
    connector.join();
    if (!producer || !client) {
        return false;
    }
    const std::vector<std::uint8_t> payload{0x5a};
    if (!producer.value().send_access_unit({1, 1, true, payload})) {
        return false;
    }
    const auto step = client.value().receive(aa::core::Milliseconds{2000});
    return step && step.value() == aa::ipc::ReceiveStatus::complete;
}

struct Pair final {
    aa::ipc::VideoServer server{};
    aa::ipc::VideoProducer producer{};
    aa::ipc::VideoConsumer consumer{};
    std::string runtime_dir{};

    ~Pair() {
        producer.close();
        consumer.close();
        server.close();
        if (!runtime_dir.empty()) {
            ::rmdir(runtime_dir.c_str());
        }
    }
    Pair(const Pair&) = delete;
    Pair& operator=(const Pair&) = delete;
    Pair(Pair&&) = default;
    Pair& operator=(Pair&&) = default;
    Pair() = default;
};

// Server accept and client connect both run the limits exchange, so the two
// halves run on opposite sides of one thread boundary.
inline aa::core::Result<Pair> make_pair(const aa::ipc::NegotiatedLimits& limits,
                                        std::uint32_t uid = aa::ipc::current_uid()) {
    Pair pair;
    pair.runtime_dir = private_runtime_dir();
    if (pair.runtime_dir.empty()) {
        return aa::Error{aa::ErrorCode::internal};
    }
    const auto path = aa::ipc::video_socket_path_in(pair.runtime_dir);
    auto server = aa::ipc::VideoServer::listen(path, aa::ipc::VideoServerOptions{limits, uid});
    if (!server) {
        return server.error();
    }
    aa::core::Result<aa::ipc::VideoConsumer> client{aa::ipc::VideoConsumer{}};
    std::thread connector([&] {
        client = aa::ipc::connect_video_consumer(
            path, aa::ipc::VideoClientOptions{limits, uid, aa::core::Milliseconds{5000}});
    });
    auto producer = server.value().accept_consumer(aa::core::Milliseconds{5000});
    connector.join();
    if (!producer) {
        return producer.error();
    }
    if (!client) {
        return client.error();
    }
    pair.server = std::move(server.value());
    pair.producer = std::move(producer.value());
    pair.consumer = std::move(client.value());
    return pair;
}

} // namespace videotest
