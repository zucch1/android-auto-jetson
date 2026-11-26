// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <aa/trust/Store.hpp>

#include <cerrno>
#include <chrono>
#include <csignal>
#include <filesystem>
#include <fcntl.h>
#include <fstream>
#include <optional>
#include <spawn.h>
#include <string>
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
inline std::optional<WiredCandidate> find_wired_candidate(
    const std::vector<trust::ApprovedPhone>& phones,
    const std::optional<WiredCandidate>& last_candidate = {},
    const std::filesystem::path& usb_devices = "/sys/bus/usb/devices") {
    std::error_code error;
    std::optional<WiredCandidate> selected;
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
            const bool accessory = known_port && vendor == "18d1" &&
                                   (product == "2d00" || product == "2d01") &&
                                   serial == wired->aoa_serial &&
                                   usb_attribute(device->path(), "product") == wired->aoa_model &&
                                   usb_attribute(device->path(), "manufacturer") == wired->aoa_manufacturer;
            if ((native || accessory) && (!selected || phone.phone_id.value < selected->phone.value ||
                (phone.phone_id == selected->phone && port < selected->port))) {
                selected = WiredCandidate{phone.phone_id, port};
            }
        }
    }
    return selected;
}

class WiredWorker final {
public:
    WiredWorker() = default;
    WiredWorker(const WiredWorker&) = delete;
    WiredWorker& operator=(const WiredWorker&) = delete;
    ~WiredWorker() { stop(); }

    [[nodiscard]] bool start(const std::filesystem::path& executable,
                             const WiredCandidate& candidate,
                             const std::filesystem::path& store,
                             int video, int input) {
        if (pid_ > 0) return false;
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
        if (::posix_spawnattr_init(&attributes) != 0) return false;
        const int video_source = ::fcntl(video, F_DUPFD_CLOEXEC, 5);
        const int input_source = ::fcntl(input, F_DUPFD_CLOEXEC, 5);
        posix_spawn_file_actions_t actions;
        const bool actions_ready = ::posix_spawn_file_actions_init(&actions) == 0;
        sigset_t mask;
        ::sigemptyset(&mask);
        ::sigaddset(&mask, SIGUSR1);
        pid_t child{};
        const bool started = actions_ready && video_source >= 0 && input_source >= 0 &&
                             ::posix_spawn_file_actions_adddup2(&actions, video_source, 3) == 0 &&
                             ::posix_spawn_file_actions_adddup2(&actions, input_source, 4) == 0 &&
                             ::posix_spawnattr_setsigmask(&attributes, &mask) == 0 &&
                             ::posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETSIGMASK) == 0 &&
                             ::posix_spawn(&child, executable.c_str(), &actions, &attributes,
                                           argv.data(), environ) == 0;
        if (actions_ready) ::posix_spawn_file_actions_destroy(&actions);
        if (video_source >= 0) ::close(video_source);
        if (input_source >= 0) ::close(input_source);
        ::posix_spawnattr_destroy(&attributes);
        if (started) pid_ = child;
        return started;
    }

    bool running() {
        if (pid_ <= 0) return false;
        const auto result = ::waitpid(pid_, nullptr, WNOHANG);
        if (result == pid_ || (result < 0 && errno == ECHILD)) pid_ = 0;
        return pid_ > 0;
    }

    void request_idr() const {
        if (pid_ > 0) (void)::kill(pid_, SIGUSR1);
    }

    void stop() {
        if (!running()) return;
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
    pid_t pid_{};
};

} // namespace aa::wireless_receiver
