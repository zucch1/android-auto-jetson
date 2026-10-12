// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <aa/ipc/InputChannel.hpp>
#include <aa/ipc/VideoChannel.hpp>

#include <cstdlib>
#include <string>
#include <utility>

namespace inputtest {

inline std::string private_runtime_dir() {
    char pattern[] = "/tmp/aa-input-XXXXXX";
    const char* dir = ::mkdtemp(pattern);
    return dir == nullptr ? std::string{} : std::string{dir};
}

struct Pair final {
    aa::ipc::InputServer server{};
    aa::ipc::InputSender sender{};
    std::string runtime_dir{};

    ~Pair() {
        sender.close();
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

inline aa::core::Result<Pair> make_pair(std::uint32_t uid = aa::ipc::current_uid()) {
    Pair pair;
    pair.runtime_dir = private_runtime_dir();
    if (pair.runtime_dir.empty()) {
        return aa::Error{aa::ErrorCode::internal};
    }
    const auto path = aa::ipc::input_socket_path_in(pair.runtime_dir);
    auto server = aa::ipc::InputServer::listen(path, aa::ipc::InputServerOptions{uid});
    if (!server) {
        return server.error();
    }
    pair.server = std::move(server.value());
    pair.sender.attach(path, uid);
    return pair;
}

} // namespace inputtest
