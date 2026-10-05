#include "metrics.hpp"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace aa::decode_probe {

void CpuSampler::start() {
    clock_gettime(CLOCK_MONOTONIC, &last_wall);
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &last_cpu);
    initialized = true;
}

double CpuSampler::sample_core_percent() {
    if (!initialized) {
        start();
        return 0.0;
    }
    struct timespec now_wall {};
    struct timespec now_cpu {};
    clock_gettime(CLOCK_MONOTONIC, &now_wall);
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &now_cpu);

    double wall_diff = (now_wall.tv_sec - last_wall.tv_sec) +
                       (now_wall.tv_nsec - last_wall.tv_nsec) * 1e-9;
    double cpu_diff = (now_cpu.tv_sec - last_cpu.tv_sec) +
                      (now_cpu.tv_nsec - last_cpu.tv_nsec) * 1e-9;

    last_wall = now_wall;
    last_cpu = now_cpu;

    if (wall_diff <= 1e-6) {
        return 0.0;
    }

    // Process CPU time / wall time * 100% = percentage of 1 single core
    return (cpu_diff / wall_diff) * 100.0;
}

void LatencyStats::calculate() {
    if (latencies_ms.empty()) {
        p95_ms = 0.0;
        max_ms = 0.0;
        avg_ms = 0.0;
        return;
    }

    std::vector<double> sorted = latencies_ms;
    std::sort(sorted.begin(), sorted.end());

    size_t idx = static_cast<size_t>(std::ceil(0.95 * sorted.size()));
    if (idx >= sorted.size()) {
        idx = sorted.size() - 1;
    }
    p95_ms = sorted[idx];
    max_ms = sorted.back();

    double sum = std::accumulate(sorted.begin(), sorted.end(), 0.0);
    avg_ms = sum / sorted.size();
}

} // namespace aa::decode_probe
