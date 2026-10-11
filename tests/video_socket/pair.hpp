// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <aa/ipc/VideoChannel.hpp>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <string>
#include <sys/file.h>
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

// Test-only reap of the startup lock file beside `socket_path`. Production
// never deletes that file (the stable inode is the lock); a test owns its
// whole runtime dir and may remove it at teardown, but only while holding
// the flock and only once every starter thread has been joined (callers
// guarantee no waiter remains).
inline void remove_startup_lock(const std::string& socket_path) {
    const std::string lock_path = socket_path + ".lock";
    const int fd = ::open(lock_path.c_str(), O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        return;
    }
    (void)::flock(fd, LOCK_EX);
    (void)::unlink(lock_path.c_str());
    ::close(fd);
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

// RAII SIGUSR1 storm for the deadline regressions: installs an empty
// handler WITHOUT SA_RESTART (syscalls must observe EINTR) and
// pthread_kill()s the waiting thread every 5 ms from a helper thread. The
// previous disposition and the target thread's signal mask are restored
// after the helper is stopped and joined, so signal state never leaks
// between tests. A HARD 2500 ms cutoff stops the storm independently of
// operation completion: a defective deadline loop that only ends when the
// signals stop must fail the elapsed bound instead of hanging the binary.
// Signal disposition is process-global - storm tests run sequentially.
struct SignalStorm final {
    static constexpr std::chrono::milliseconds kCutoff{2500};

    explicit SignalStorm(pthread_t target) : target_(target) {
        ::sigemptyset(&previous_mask_);
        struct sigaction action {};
        action.sa_handler = +[](int) {};
        ::sigemptyset(&action.sa_mask);
        if (::sigaction(SIGUSR1, &action, &previous_) != 0) {
            return;
        }
        installed_ = true;
        (void)::pthread_sigmask(SIG_SETMASK, nullptr, &previous_mask_);
        sigset_t unblock{};
        ::sigemptyset(&unblock);
        ::sigaddset(&unblock, SIGUSR1);
        (void)::pthread_sigmask(SIG_UNBLOCK, &unblock, nullptr);
        worker_ = std::thread([this] {
            const auto start = std::chrono::steady_clock::now();
            while (storming_.load(std::memory_order_relaxed) &&
                   std::chrono::steady_clock::now() - start < kCutoff) {
                (void)::pthread_kill(target_, SIGUSR1);
                std::this_thread::sleep_for(std::chrono::milliseconds{5});
            }
        });
    }

    ~SignalStorm() { stop(); }

    void stop() noexcept {
        storming_.store(false, std::memory_order_relaxed);
        if (worker_.joinable()) {
            worker_.join();
        }
        if (installed_) {
            (void)::sigaction(SIGUSR1, &previous_, nullptr);
            (void)::pthread_sigmask(SIG_SETMASK, &previous_mask_, nullptr);
            installed_ = false;
        }
    }

    [[nodiscard]] bool valid() const noexcept { return installed_; }

    SignalStorm(const SignalStorm&) = delete;
    SignalStorm& operator=(const SignalStorm&) = delete;
    SignalStorm(SignalStorm&&) = delete;
    SignalStorm& operator=(SignalStorm&&) = delete;

private:
    pthread_t target_{};
    struct sigaction previous_ {};
    sigset_t previous_mask_{};
    std::atomic<bool> storming_{true};
    std::thread worker_{};
    bool installed_{false};
};

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
            const auto path = aa::ipc::video_socket_path_in(runtime_dir);
            remove_startup_lock(path.value);
            ::rmdir((runtime_dir + "/android-auto-receiver").c_str());
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
