// SPDX-License-Identifier: GPL-3.0-or-later
#include "nm/Platform.hpp"
#include <aa/helper/BackendRouter.hpp>
#include <aa/helper/BluezBackend.hpp>
#include <aa/helper/BusService.hpp>
#include <csignal>
#include <iostream>
#include <string>
#include <unistd.h>

namespace {
volatile std::sig_atomic_t stopping = 0;
void stop(int) { stopping = 1; }
} // namespace
int main(int argc, char **argv) {
    const bool fixture =
        (argc == 4 || argc == 5 || (argc == 6 && std::string{argv[4]} == "--combined")) &&
        std::string{argv[1]} == "--private-bus" && ::getuid() != 0 && ::getuid() == ::geteuid();
    if (argc != 1 && !fixture)
        return 2;
    const std::string directory = fixture ? argv[3] : "/etc/android-auto-receiver";
    const auto policy = aa::helper::IdentityPolicy::load(
        directory + "/session.uid", directory + "/receiver.sha256", fixture ? ::getuid() : 0);
    if (!policy) {
        std::cerr << "system-helper: invalid provisioned identity policy\n";
        return 1;
    }
    const std::string bus_address = fixture ? argv[2] : "unix:path=/run/dbus/system_bus_socket";
    aa::helper::UnavailableBackend unavailable;
    std::unique_ptr<aa::helper::Backend> ap;
    std::unique_ptr<aa::helper::BluezBackend> bluetooth;
    // Explicit standalone fixture modes keep absent capabilities unavailable.
    // Production always constructs both real adapters.
    if (!fixture || argc >= 5) {
        const auto provision = aa::helper::nm::load_policy(directory, fixture ? ::getuid() : 0);
        if (!provision) {
            std::cerr << "system-helper: invalid ap.conf; explicitly confirm jurisdiction\n";
            return 1;
        }
        ap = std::make_unique<aa::helper::nm::Backend>(
            provision.value(), bus_address,
            fixture ? aa::helper::nm::private_commands(argv[argc - 1])
                    : aa::helper::nm::production_commands());
    }
    if (!fixture || argc != 5) {
        bluetooth = std::make_unique<aa::helper::BluezBackend>(bus_address);
        if (!bluetooth->available())
            return 1;
    }
    aa::helper::BackendRouter backend{{.ap = ap ? *ap : unavailable,
        .bluetooth = bluetooth ? static_cast<aa::helper::Backend&>(*bluetooth) : unavailable}};
    auto service = fixture ? aa::helper::BusService::private_bus(argv[2], policy.value(), backend)
                           : aa::helper::BusService::system(policy.value(), backend);
    if (!service)
        return 1;
    std::signal(SIGTERM, stop);
    std::signal(SIGINT, stop);
    while (!stopping) {
        if (!service.value().cycle())
            return 1;
    }
    return 0;
}
