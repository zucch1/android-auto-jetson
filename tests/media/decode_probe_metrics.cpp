#include "metrics.hpp"
#include <iostream>

int main() {
    using namespace aa::decode_probe;
    int failures = 0;
    // Given 20 ordered latencies, when calculated, then nearest-rank p95 is the 19th value.
    LatencyStats stats;
    for (int i = 1; i <= 20; ++i) stats.latencies_ms.push_back(i);
    stats.calculate();
    if (stats.p95_ms != 19.0) {
        std::cerr << "FAIL nearest-rank p95: " << stats.p95_ms << '\n';
        ++failures;
    }
    // Given no samples, when calculated, then no latency is claimed.
    LatencyStats empty;
    empty.calculate();
    if (empty.p95_ms != 0.0) ++failures;
    // Given reordered output and a missing frame, when matched by PTS, then preserve exact identity.
    FrameCorrelation correlation;
    const FrameCorrelation::Time first{};
    const auto second = first + std::chrono::milliseconds(10);
    if (!correlation.record(10, first) || !correlation.record(20, second)) ++failures;
    if (correlation.record(10, second)) ++failures;
    if (correlation.take(20) != second) ++failures;
    if (correlation.take(20) || correlation.take(999)) ++failures;
    if (correlation.pending.size() != 1 || correlation.take(10) != first) ++failures;
    std::cout << "metric failures=" << failures << '\n';
    return failures == 0 ? 0 : 1;
}
