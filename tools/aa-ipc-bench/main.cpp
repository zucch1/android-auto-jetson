// SPDX-License-Identifier: GPL-3.0-or-later
#include "benchmark.hpp"
#include <charconv>
#include <iostream>
#include <string_view>

int main(int argc, char** argv) {
    unsigned duration = 600;
    bool failure = false;
    for (int i = 1; i < argc; ++i) {
        const std::string_view argument(argv[i]);
        if (argument == "--help") {
            std::cout << "aa-ipc-bench --profile 1280x720@30 --duration 1..3600 [--failure-scenario]\n";
            return 0;
        }
        if (argument == "--failure-scenario") { failure = true; continue; }
        if (i + 1 >= argc) { std::cerr << "missing option value\n"; return 2; }
        const std::string_view value(argv[++i]);
        if (argument == "--profile" && value == "1280x720@30") continue;
        if (argument == "--duration") {
            const auto result = std::from_chars(value.data(), value.data() + value.size(), duration);
            if (result.ec == std::errc{} && result.ptr == value.data() + value.size() && duration > 0 && duration <= 3600) continue;
        }
        std::cerr << "invalid option or value\n"; return 2;
    }
    try {
        if (!failure) return aa::ipc::benchmark(duration);
        const auto report = aa::ipc::failure_scenario();
        std::cout << "drops=" << report.drops << " backpressure=" << report.backpressure
                  << " resyncs=" << report.resyncs << " rejected=" << report.rejected << '\n';
        return report.pass ? 0 : 1;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
