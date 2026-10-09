#pragma once

#include <cstdint>
#include <chrono>
#include <map>
#include <optional>
#include <ctime>
#include <string>
#include <vector>

namespace aa::decode_probe {

struct FrameCorrelation {
    using Time = std::chrono::steady_clock::time_point;
    std::map<uint64_t, Time> pending;
    bool record(uint64_t pts, Time sent);
    std::optional<Time> take(uint64_t pts);
};

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
    uint64_t bytes_fed = 0;
    uint64_t latency_samples = 0;
    uint64_t unmatched_frames = 0;
    uint32_t observed_width = 0;
    uint32_t observed_height = 0;
    double feed_duration_s = 0.0;
    double observed_fps = 0.0;
    double observed_bitrate_bps = 0.0;
    bool smoke_pass = false;
    bool requested_workload_met = false;
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
