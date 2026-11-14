// SPDX-License-Identifier: GPL-3.0-or-later
#include "Platform.hpp"
#include <array>
#include <csignal>
#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sstream>
#include <sys/socket.h>
#include <sys/mman.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

namespace aa::helper::nm {
namespace {
class Process final : public Commands {
public:
    core::Result<std::string> run(const std::vector<std::string>& args, const std::string& input) override {
        if (args.empty() || input.size() > 4096 || (args[0] != "/usr/sbin/iw" && args[0] != "/usr/sbin/nft"))
            return Error{ErrorCode::helper_rejected};
        int out[2]{};
        const int input_fd = ::memfd_create("aa-helper-command", MFD_CLOEXEC);
        if (input_fd < 0) return Error{ErrorCode::helper_unavailable};
        if (::write(input_fd, input.data(), input.size()) != static_cast<ssize_t>(input.size()) ||
            ::lseek(input_fd, 0, SEEK_SET) < 0 || ::pipe2(out, O_CLOEXEC)) {
            ::close(input_fd); return Error{ErrorCode::helper_unavailable};
        }
        (void)::fcntl(out[0], F_SETFL, O_NONBLOCK);
        posix_spawn_file_actions_t actions;
        ::posix_spawn_file_actions_init(&actions);
        ::posix_spawn_file_actions_adddup2(&actions, input_fd, STDIN_FILENO);
        ::posix_spawn_file_actions_adddup2(&actions, out[1], STDOUT_FILENO);
        ::posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
        std::vector<char*> argv;
        argv.reserve(args.size() + 1);
        for (const auto& arg : args) argv.push_back(const_cast<char*>(arg.c_str()));
        argv.push_back(nullptr);
        char locale[] = "LC_ALL=C", path[] = "PATH=/usr/sbin:/usr/bin";
        char* env[]{locale, path, nullptr};
        pid_t pid{};
        const int spawned = ::posix_spawn(&pid, argv[0], &actions, nullptr, argv.data(), env);
        ::posix_spawn_file_actions_destroy(&actions);
        ::close(input_fd); ::close(out[1]);
        bool okay = spawned == 0;
        std::string result;
        const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
        int status{};
        bool exited = false;
        while (okay && std::chrono::steady_clock::now() < until) {
            std::array<char, 4096> data{};
            const auto n = ::read(out[0], data.data(), data.size());
            if (n > 0) result.append(data.data(), static_cast<std::size_t>(n));
            if (result.size() > 65536) { okay = false; break; }
            if (!exited) exited = ::waitpid(pid, &status, WNOHANG) == pid;
            if (exited && n == 0) break;
            ::pollfd fd{out[0], POLLIN, 0};
            (void)::poll(&fd, 1, 5);
        }
        ::close(out[0]);
        if (spawned == 0 && !exited) { ::kill(pid, SIGKILL); (void)::waitpid(pid, &status, 0); }
        if (!okay || !exited || !WIFEXITED(status) || WEXITSTATUS(status)) return Error{ErrorCode::helper_unavailable};
        return result;
    }
};
class Socket final : public Commands {
public:
    explicit Socket(std::string path) : path_(std::move(path)) {}
    core::Result<std::string> run(const std::vector<std::string>& args, const std::string& input) override {
        sockaddr_un address{};
        address.sun_family = AF_UNIX;
        if (path_.size() >= sizeof(address.sun_path)) return Error{ErrorCode::config_invalid};
        std::copy(path_.begin(), path_.end(), address.sun_path);
        const int fd = ::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
        if (fd < 0) return Error{ErrorCode::helper_unavailable};
        std::string request;
        for (const auto& arg : args) { request += arg; request += '\n'; }
        request += '\n'; request += input;
        const int connected = ::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address));
        bool okay = connected == 0 && ::send(fd, request.data(), request.size(), MSG_NOSIGNAL) == static_cast<ssize_t>(request.size());
        ::pollfd event{fd, POLLIN, 0};
        okay = okay && ::poll(&event, 1, 500) == 1;
        std::array<char, 65537> response{};
        const auto n = okay ? ::recv(fd, response.data(), response.size(), MSG_TRUNC) : -1;
        ::close(fd);
        if (n < 1 || n > 65536 || response[0] != '0') return Error{ErrorCode::helper_unavailable};
        return std::string{response.data() + 1, static_cast<std::size_t>(n - 1)};
    }
private:
    std::string path_;
};
}
std::unique_ptr<Commands> production_commands() { return std::make_unique<Process>(); }
std::unique_ptr<Commands> private_commands(const std::string& path) { return std::make_unique<Socket>(path); }
bool eligible(const Policy& policy, Commands& commands) {
    const auto info = commands.run({"/usr/sbin/iw", "dev", policy.interface, "info"});
    const auto interfaces = commands.run({"/usr/sbin/iw", "dev"});
    const auto reg = commands.run({"/usr/sbin/iw", "reg", "get"});
    if (!info || !reg || !interfaces) return false;
    std::istringstream lines{info.value()};
    std::string line, phy;
    while (std::getline(lines, line)) {
        std::istringstream fields{line};
        std::string key, value;
        if (fields >> key >> value && key == "wiphy" && value.find_first_not_of("0123456789") == std::string::npos)
            phy = "phy" + value;
    }
    if (phy.empty()) return false;
    std::istringstream devices{interfaces.value()};
    unsigned count{};
    while (std::getline(devices, line)) {
        std::istringstream fields{line};
        std::string key, value;
        if (fields >> key >> value && key == "Interface") {
            if (value != policy.interface) return false;
            ++count;
        }
    }
    if (count != 1) return false;
    std::istringstream countries{reg.value()};
    bool confirmed = false;
    while (std::getline(countries, line)) {
        const auto pos = line.find("country ");
        if (pos == std::string::npos) continue;
        if (line.substr(pos + 8, 3) != policy.country + ":") return false;
        confirmed = true;
    }
    if (!confirmed) return false;
    const auto capabilities = commands.run({"/usr/sbin/iw", "phy", phy, "info"});
    const auto frequency = std::to_string(ap_frequency_mhz(policy.channel));
    const auto channel = "[" + std::to_string(policy.channel) + "]";
    if (!capabilities) return false;
    std::istringstream frequencies{capabilities.value()};
    while (std::getline(frequencies, line)) {
        if (line.find(frequency + " MHz " + channel) == std::string::npos &&
            line.find(frequency + ".0 MHz " + channel) == std::string::npos) continue;
        return line.find("disabled") == std::string::npos && line.find("no IR") == std::string::npos &&
               line.find("radar") == std::string::npos && line.find("NO-IR") == std::string::npos;
    }
    return false;
}
bool actual_channel(const Policy& policy, Commands& commands) {
    const auto info = commands.run({"/usr/sbin/iw", "dev", policy.interface, "info"});
    return info && info.value().find("type AP\n") != std::string::npos &&
           info.value().find("channel " + std::to_string(policy.channel) + " (" +
                             std::to_string(ap_frequency_mhz(policy.channel)) + " MHz)") != std::string::npos;
}
std::string firewall(const Policy& policy) {
    const auto match = "iifname \"" + policy.interface + "\" ";
    return "create table inet aa_receiver_ap { comment \"aa-system-helper-v1\"; }\n"
           "add chain inet aa_receiver_ap input { type filter hook input priority -200; policy accept; }\n"
           "add chain inet aa_receiver_ap forward { type filter hook forward priority -200; policy accept; }\n"
           // The projection port is reachable only from stations of this AP.
           "add rule inet aa_receiver_ap input iifname != \"" + policy.interface + "\" tcp dport " +
               std::to_string(policy.port) + " drop\n"
           "add rule inet aa_receiver_ap input " + match + "udp sport 68 udp dport 67 accept\n"
           "add rule inet aa_receiver_ap input " + match + "tcp dport " + std::to_string(policy.port) + " accept\n"
           "add rule inet aa_receiver_ap input " + match + "drop\n"
           "add rule inet aa_receiver_ap forward " + match + "drop\n"
           "add rule inet aa_receiver_ap forward oifname \"" + policy.interface + "\" drop\n";
}
}
