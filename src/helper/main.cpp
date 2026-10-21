// SPDX-License-Identifier: GPL-3.0-or-later
#include <aa/helper/BusService.hpp>
#include <csignal>
#include <iostream>
#include <string>
#include <unistd.h>

namespace {
volatile std::sig_atomic_t stopping = 0;
void stop(int) { stopping = 1; }
}
int main(int argc, char** argv) {
    const bool fixture = argc == 4 && std::string{argv[1]} == "--private-bus" &&
                         ::getuid() != 0 && ::getuid() == ::geteuid();
    if (argc != 1 && !fixture) return 2;
    const std::string directory = fixture ? argv[3] : "/etc/android-auto-receiver";
    const auto policy = aa::helper::IdentityPolicy::load(
        directory + "/session.uid", directory + "/receiver.sha256", fixture ? ::getuid() : 0);
    if (!policy) {
        std::cerr << "system-helper: invalid provisioned identity policy\n";
        return 1;
    }
    aa::helper::UnavailableBackend backend;
    auto service = fixture ? aa::helper::BusService::private_bus(argv[2], policy.value(), backend)
                           : aa::helper::BusService::system(policy.value(), backend);
    if (!service) return 1;
    std::signal(SIGTERM, stop);
    std::signal(SIGINT, stop);
    while (!stopping) {
        if (!service.value().cycle()) return 1;
    }
    return 0;
}
