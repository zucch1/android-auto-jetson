// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "../video_handoff.hpp"

#include <aa/ipc/VideoSocket.hpp>
#include <aa/trust/Store.hpp>

#include <algorithm>

#include <cerrno>
#include <chrono>
#include <csignal>
#include <filesystem>
#include <fcntl.h>
#include <fstream>
#include <optional>
#include <spawn.h>
#include <string>
#include <sys/socket.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

extern char** environ;

namespace aa::wireless_receiver {

struct WiredCandidate final {
    core::PhoneId phone;
    std::string port;
};

inline std::string usb_attribute(const std::filesystem::path& device, const char* name) {
    std::ifstream file{device / name};
    std::string value;
    std::getline(file, value);
    return value;
}

// Discovery is read-only. A serial can find its phone on any port. The worker
// still performs the full AOA identity check with the current approved store.
// A phone already in accessory mode is matched on its enrolment port, on the
// port this supervisor last used for it, or on any port when its accessory
// identity is unique among the approved phones: after a supervisor restart the
// phone can still be in accessory mode on another port (#240 review P3-6).
// Candidates are ordered by phone id, then port.
inline std::vector<WiredCandidate> find_wired_candidates(
    const std::vector<trust::ApprovedPhone>& phones,
    const std::optional<WiredCandidate>& last_candidate = {},
    const std::filesystem::path& usb_devices = "/sys/bus/usb/devices") {
    const auto accessory_unique = [&](const trust::WiredIdentity& wired) {
        if (wired.aoa_serial.empty()) return false;
        return std::count_if(phones.begin(), phones.end(), [&](const auto& other) {
                   const auto* identity = std::get_if<trust::WiredIdentity>(&other.identity);
                   return identity && identity->aoa_serial == wired.aoa_serial &&
                          identity->aoa_model == wired.aoa_model &&
                          identity->aoa_manufacturer == wired.aoa_manufacturer;
               }) == 1;
    };
    std::error_code error;
    std::vector<WiredCandidate> found;
    for (std::filesystem::directory_iterator device{usb_devices, error}, end;
         !error && device != end; device.increment(error)) {
        const auto port = device->path().filename().string();
        if (port.find('-') == std::string::npos || port.find(':') != std::string::npos ||
            port.find_first_not_of("0123456789.-") != std::string::npos) {
            continue;
        }
        const auto serial = usb_attribute(device->path(), "serial");
        if (serial.empty()) continue;
        const auto vendor = usb_attribute(device->path(), "idVendor");
        const auto product = usb_attribute(device->path(), "idProduct");
        for (const auto& phone : phones) {
            const auto* wired = std::get_if<trust::WiredIdentity>(&phone.identity);
            if (!wired || wired->usb_serial.empty()) continue;
            const bool native = serial == wired->usb_serial &&
                                vendor == wired->usb_vendor_id && product == wired->usb_product_id;
            const bool known_port = port == wired->port_context ||
                                    (last_candidate && last_candidate->phone == phone.phone_id &&
                                     last_candidate->port == port);
            const bool accessory = vendor == "18d1" && (product == "2d00" || product == "2d01") &&
                                   serial == wired->aoa_serial &&
                                   (known_port || accessory_unique(*wired)) &&
                                   usb_attribute(device->path(), "product") == wired->aoa_model &&
                                   usb_attribute(device->path(), "manufacturer") == wired->aoa_manufacturer;
            if (native || accessory) found.push_back(WiredCandidate{phone.phone_id, port});
        }
    }
    std::sort(found.begin(), found.end(), [](const auto& a, const auto& b) {
        return a.phone.value != b.phone.value ? a.phone.value < b.phone.value : a.port < b.port;
    });
    return found;
}

inline std::optional<WiredCandidate> find_wired_candidate(
    const std::vector<trust::ApprovedPhone>& phones,
    const std::optional<WiredCandidate>& last_candidate = {},
    const std::filesystem::path& usb_devices = "/sys/bus/usb/devices") {
    auto found = find_wired_candidates(phones, last_candidate, usb_devices);
    if (found.empty()) return std::nullopt;
    return found.front();
}

class WiredWorker final {
public:
    WiredWorker() = default;
    WiredWorker(const WiredWorker&) = delete;
    WiredWorker& operator=(const WiredWorker&) = delete;
    ~WiredWorker() { stop(); }

    // Spawns the managed wired worker with:
    //   fd 3: supervisor -> worker video handoff AF_UNIX SOCK_SEQPACKET channel
    //   fd 4: inherited input listener
    // The supervisor remains the sole acceptor on video.sock so the worker
    // never owns the consumer connection or closes it on startup failure.
    [[nodiscard]] bool start(const std::filesystem::path& executable,
                             const WiredCandidate& candidate,
                             const std::filesystem::path& store,
                             int input) {
        if (pid_ > 0) return false;
        int handoff[2]{-1, -1};
        if (::socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, handoff) != 0) {
            return false;
        }
        std::vector<std::string> arguments{
            executable.string(), "--no-bus", "--managed-by", std::to_string(::getpid()),
            "--port", candidate.port, "--managed-phone-id", std::to_string(candidate.phone.value),
            "--store", store.string(), "--require-video-consumer"};
        std::vector<char*> argv;
        for (auto& argument : arguments) argv.push_back(argument.data());
        argv.push_back(nullptr);

        // The worker installs its handler before unblocking SIGUSR1. A
        // request sent during startup cannot terminate it via the default action.
        posix_spawnattr_t attributes;
        if (::posix_spawnattr_init(&attributes) != 0) {
            ::close(handoff[0]);
            ::close(handoff[1]);
            return false;
        }
        const int handoff_source = ::fcntl(handoff[1], F_DUPFD_CLOEXEC, 5);
        const int input_source = ::fcntl(input, F_DUPFD_CLOEXEC, 5);
        posix_spawn_file_actions_t actions;
        const bool actions_ready = ::posix_spawn_file_actions_init(&actions) == 0;
        sigset_t mask;
        ::sigemptyset(&mask);
        ::sigaddset(&mask, SIGUSR1);
        pid_t child{};
        const bool started = actions_ready && handoff_source >= 0 && input_source >= 0 &&
                             ::posix_spawn_file_actions_adddup2(&actions, handoff_source, 3) == 0 &&
                             ::posix_spawn_file_actions_adddup2(&actions, input_source, 4) == 0 &&
                             ::posix_spawnattr_setsigmask(&attributes, &mask) == 0 &&
                             ::posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETSIGMASK) == 0 &&
                             ::posix_spawn(&child, executable.c_str(), &actions, &attributes,
                                           argv.data(), environ) == 0;
        if (actions_ready) ::posix_spawn_file_actions_destroy(&actions);
        if (handoff_source >= 0) ::close(handoff_source);
        if (input_source >= 0) ::close(input_source);
        ::close(handoff[1]);
        ::posix_spawnattr_destroy(&attributes);
        if (started) {
            pid_ = child;
            video_handoff_fd_ = handoff[0];
        } else {
            ::close(handoff[0]);
        }
        return started;
    }

    [[nodiscard]] bool send_video_client(int client_fd, int state_fd,
                                         const ipc::NegotiatedLimits& limits) const noexcept {
        if (pid_ <= 0 || video_handoff_fd_ < 0) return false;
        return tools::send_video_handoff_install(video_handoff_fd_, client_fd, state_fd, limits);
    }

    [[nodiscard]] bool clear_video_client() const noexcept {
        if (pid_ <= 0 || video_handoff_fd_ < 0) return false;
        return tools::send_video_handoff_clear(video_handoff_fd_);
    }

    bool running() {
        if (pid_ <= 0) return false;
        const auto result = ::waitpid(pid_, nullptr, WNOHANG);
        if (result == pid_ || (result < 0 && errno == ECHILD)) {
            pid_ = 0;
            close_handoff();
        }
        return pid_ > 0;
    }

    // Blocking reap for synchronized tests without timed polling loops.
    [[nodiscard]] bool wait_exit() {
        if (pid_ <= 0) return true;
        int status = 0;
        pid_t result;
        do {
            result = ::waitpid(pid_, &status, 0);
        } while (result < 0 && errno == EINTR);
        pid_ = 0;
        close_handoff();
        return result > 0;
    }

    void request_idr() const {
        if (pid_ > 0) (void)::kill(pid_, SIGUSR1);
    }

    void stop() {
        if (!running()) {
            close_handoff();
            return;
        }
        close_handoff();
        (void)::kill(pid_, SIGTERM);
        using namespace std::chrono_literals;
        const auto until = std::chrono::steady_clock::now() + 750ms;
        while (running() && std::chrono::steady_clock::now() < until) {
            std::this_thread::sleep_for(10ms);
        }
        if (pid_ > 0) {
            (void)::kill(pid_, SIGKILL);
            while (::waitpid(pid_, nullptr, 0) < 0 && errno == EINTR) {}
            pid_ = 0;
        }
    }

private:
    void close_handoff() noexcept {
        if (video_handoff_fd_ >= 0) {
            ::close(video_handoff_fd_);
            video_handoff_fd_ = -1;
        }
    }

    pid_t pid_{};
    int video_handoff_fd_{-1};
};

} // namespace aa::wireless_receiver
