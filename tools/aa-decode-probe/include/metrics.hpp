#pragma once

#include <cstdint>
#include <ctime>
#include <string>
#include <vector>

namespace aa::decode_probe {

struct CpuSampler {
    struct timespec last_wall {};
    struct timespec last_cpu {};
    bool initialized = false;

    void start();
    // Returns CPU usage of the process as percentage of 1 single core (0% to 100%+)
    double sample_core_percent();
};

struct LatencyStats {
    std::vector<double> latencies_ms;
    double p95_ms = 0.0;
    double max_ms = 0.0;
    double avg_ms = 0.0;

    void calculate();
};

struct ProbeMetrics {
    std::string decoder_used;
    bool is_hardware = false;
    double duration_s = 0.0;
    uint64_t frames_fed = 0;
    uint64_t frames_decoded = 0;
    uint64_t dropped_frames = 0;
    double p95_latency_ms = 0.0;
    double max_latency_ms = 0.0;
    double avg_latency_ms = 0.0;
    double cpu_percent = 0.0;
    bool caps_negotiated_correct = false;
    std::string negotiated_caps_str;
    bool pass = false;
    std::string failure_reason;
};

} // namespace aa::decode_probe
