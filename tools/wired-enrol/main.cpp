// SPDX-License-Identifier: GPL-3.0-or-later
// aa-wired-enrol (#133): minimal reference consumer for wired enrolment, for
// tests and owner bring-up. Registers as the receiver's D-Bus consumer, asks
// for the phone the receiver last refused, shows its redacted label and asks
// y/N on the terminal. Only an explicit "y" approves. The production consumer
// is the infotainment UI, which drives the same contract calls.
//
// Usage: aa-wired-enrol [--consumer-name NAME] [--answer-budget-ms N]
//   needs aa-wired-receiver --wired-enrolment running on the same session bus
//   and as the same Unix user. --answer-budget-ms (1..50000, default 50000)
//   bounds the y/N answer. exit: 0 approved, 1 declined (or no complete answer
//   line in time), 2 usage, 3 failed (no receiver, no refused phone, refused
//   or not-durable approval).
#include "AnswerReader.hpp"
#include "EnrolFlow.hpp"

#include <aa/ipc/ControlBusClient.hpp>

#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>

namespace {

constexpr const char* kUsage = "usage: aa-wired-enrol [--consumer-name NAME] [--answer-budget-ms N]\n";

} // namespace

int main(int argc, char** argv) {
    aa::ipc::BusName consumer_name{"org.custom.WiredEnrol1"};
    aa::core::Milliseconds budget = aa::tools::enrol::kAnswerBudget;
    for (int index = 1; index < argc; ++index) {
        if (std::strcmp(argv[index], "--consumer-name") == 0 && index + 1 < argc) {
            consumer_name = aa::ipc::BusName{argv[++index]};
        } else if (std::strcmp(argv[index], "--answer-budget-ms") == 0 && index + 1 < argc) {
            char* end = nullptr;
            errno = 0;
            const long long value = std::strtoll(argv[++index], &end, 10);
            if (errno != 0 || end == argv[index] || *end != '\0' || value < 1 ||
                value > aa::tools::enrol::kAnswerBudget.count) {
                std::fputs(kUsage, stderr);
                return 2;
            }
            budget = aa::core::Milliseconds{static_cast<std::int64_t>(value)};
        } else {
            std::fputs(kUsage, stderr);
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
    const auto read_line = [](aa::core::Milliseconds left) {
        return aa::tools::enrol::read_answer_line(STDIN_FILENO, left);
    };
    return static_cast<int>(
        aa::tools::enrol::run(client.value(), consumer_name, std::cout, read_line, budget));
}
