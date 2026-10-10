// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <aa/ipc/VideoChannel.hpp>

#include <cstdlib>
#include <string>
#include <thread>
#include <utility>

namespace videotest {

inline std::string private_runtime_dir() {
    char pattern[] = "/tmp/aa-video-XXXXXX";
    const char* dir = ::mkdtemp(pattern);
    return dir == nullptr ? std::string{} : std::string{dir};
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
