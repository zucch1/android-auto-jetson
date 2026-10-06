// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
namespace aa::ipc {
struct FailureReport {
    std::uint64_t drops{}, backpressure{}, resyncs{}, rejected{}, suppressed{};
    bool pass{};
};
FailureReport failure_scenario();
int benchmark(unsigned duration);
}
