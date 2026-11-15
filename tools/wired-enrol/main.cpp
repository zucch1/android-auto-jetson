// SPDX-License-Identifier: GPL-3.0-or-later
// aa-wired-enrol (#133): minimal reference consumer for wired enrolment, for
// tests and owner bring-up. Registers as the receiver's D-Bus consumer, asks
// for the phone the receiver last refused, shows its redacted label and asks
// y/N on the terminal. Only an explicit "y" approves. The production consumer
// is the infotainment UI, which drives the same contract calls.
//
// Usage: aa-wired-enrol [--consumer-name NAME]
//   needs aa-wired-receiver --wired-enrolment running on the same session bus
//   and as the same Unix user. exit: 0 approved, 1 declined, 2 usage,
//   3 failed (no receiver, no refused phone, refused approval).
#include "EnrolFlow.hpp"

#include <aa/ipc/ControlBusClient.hpp>

#include <poll.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>

namespace {

std::optional<std::string> read_line(aa::core::Milliseconds budget) {
    ::pollfd input{STDIN_FILENO, POLLIN, 0};
    const int ready = ::poll(&input, 1, static_cast<int>(budget.count));
    if (ready <= 0) return std::nullopt;
    std::string line;
    if (!std::getline(std::cin, line)) return std::nullopt;
    return line;
}

} // namespace

int main(int argc, char** argv) {
    aa::ipc::BusName consumer_name{"org.custom.WiredEnrol1"};
    for (int index = 1; index < argc; ++index) {
        if (std::strcmp(argv[index], "--consumer-name") == 0 && index + 1 < argc) {
            consumer_name = aa::ipc::BusName{argv[++index]};
        } else {
            std::fputs("usage: aa-wired-enrol [--consumer-name NAME]\n", stderr);
            return 2;
        }
    }
    auto client = aa::ipc::ControlBusClient::connect();
    if (!client) {
        std::fprintf(stderr, "error: session bus unavailable (%s)\n",
                     std::string{aa::to_string(client.error().code())}.c_str());
        return 3;
    }
    const auto owner = client.value().receiver_owner();
    if (!owner || !owner.value()) {
        std::fputs("error: no receiver on the session bus (start aa-wired-receiver --wired-enrolment)\n",
                   stderr);
        return 3;
    }
    return static_cast<int>(aa::tools::enrol::run(client.value(), consumer_name, std::cout, read_line));
}
