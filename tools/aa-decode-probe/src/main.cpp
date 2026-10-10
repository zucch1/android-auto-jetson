// SPDX-License-Identifier: GPL-3.0-or-later
#include "discovery.hpp"
#include "json_report.hpp"
#include "metrics.hpp"
#include "options.hpp"
#include "pipeline.hpp"

#include <fstream>
#include <iostream>
#include <chrono>
#include <cerrno>
#include <csignal>
#include <exception>
#include <thread>
#include <sys/wait.h>
#include <unistd.h>
#include <gst/gst.h>

static int emit_report(const aa::decode_probe::ProbeOptions& options,
                       const aa::decode_probe::DiscoveryResult& discovery,
                       const aa::decode_probe::ProbeMetrics& metrics) {
    auto result = metrics;
    auto json = aa::decode_probe::format_json_report(options, discovery, result);
    bool written = true;
    if (!options.output_json_path.empty()) {
        std::ofstream out(options.output_json_path);
        out << json;
        out.close();
        written = !out.fail();
        if (!written) {
            result.pass = false;
            result.smoke_pass = false;
            result.failure_reason = "Failed to write output JSON";
            json = aa::decode_probe::format_json_report(options, discovery, result);
            std::cerr << result.failure_reason << '\n';
        }
    }
    std::cout << json << std::flush;
    return written && result.pass ? 0 : 2;
}

int main(int argc, char* argv[]) {
    aa::decode_probe::ProbeOptions options;
    std::string parse_err;
    if (!aa::decode_probe::parse_options(argc, argv, options, parse_err)) {
        std::cerr << "Error: " << parse_err << "\n";
        aa::decode_probe::ProbeMetrics metrics;
        metrics.failure_reason = parse_err;
        emit_report(options, {}, metrics);
        return 1;
    }

    if (options.help) {
        aa::decode_probe::print_help(argv[0]);
        return 0;
    }

    // Isolate plugin calls: even NULL/READY state changes can block inside a driver.
    const pid_t worker = fork();
    if (worker == 0) {
        try {
            gst_init(nullptr, nullptr);
            const auto discovery = aa::decode_probe::discover_decoders(options.forced_decoder);
            const auto metrics = aa::decode_probe::run_decode_probe(options, discovery);
            _exit(emit_report(options, discovery, metrics));
        } catch (const std::exception& error) {
            aa::decode_probe::ProbeMetrics metrics;
            metrics.failure_reason = std::string("Probe exception: ") + error.what();
            _exit(emit_report(options, {}, metrics));
        } catch (...) {
            aa::decode_probe::ProbeMetrics metrics;
            metrics.failure_reason = "Unknown probe exception";
            _exit(emit_report(options, {}, metrics));
        }
    }
    aa::decode_probe::ProbeMetrics failure;
    if (worker < 0) {
        failure.failure_reason = "Failed to start isolated probe worker";
        return emit_report(options, {}, failure);
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(options.duration_s + 8);
    int status = 0;
    while (true) {
        const pid_t result = waitpid(worker, &status, WNOHANG);
        if (result == worker) {
            if (WIFEXITED(status)) return WEXITSTATUS(status);
            failure.failure_reason = "Probe worker terminated by signal";
            return emit_report(options, {}, failure);
        }
        if (result < 0 && errno != EINTR) {
            failure.failure_reason = "Failed to wait for probe worker";
            break;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            failure.failure_reason = "Probe worker deadline exceeded (duration + 8 seconds)";
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    kill(worker, SIGKILL);
    while (waitpid(worker, &status, 0) < 0 && errno == EINTR) {}
    return emit_report(options, {}, failure);
}
