#include "json_report.hpp"

#include <iomanip>
#include <sstream>

namespace aa::decode_probe {

std::string format_json_report(const ProbeOptions& options,
                               const DiscoveryResult& discovery,
                               const ProbeMetrics& metrics) {
    std::ostringstream ss;
    ss << std::fixed << std::setprecision(2);

    ss << "{\n";
    ss << "  \"schema\": \"aa-decode-probe/1\",\n";
    ss << "  \"profile\": \"" << options.profile << "\",\n";
    ss << "  \"bitrate\": \"" << options.bitrate << "\",\n";
    ss << "  \"configured_duration_s\": " << options.duration_s << ",\n";
    ss << "  \"decoder\": \"" << metrics.decoder_used << "\",\n";
    ss << "  \"is_hardware\": " << (metrics.is_hardware ? "true" : "false") << ",\n";
    ss << "  \"duration_s\": " << metrics.duration_s << ",\n";
    ss << "  \"frames_fed\": " << metrics.frames_fed << ",\n";
    ss << "  \"frames_decoded\": " << metrics.frames_decoded << ",\n";
    ss << "  \"dropped_frames\": " << metrics.dropped_frames << ",\n";
    ss << "  \"p95_latency_ms\": " << metrics.p95_latency_ms << ",\n";
    ss << "  \"max_latency_ms\": " << metrics.max_latency_ms << ",\n";
    ss << "  \"avg_latency_ms\": " << metrics.avg_latency_ms << ",\n";
    ss << "  \"cpu_percent\": " << metrics.cpu_percent << ",\n";
    ss << "  \"caps_negotiated\": \"" << metrics.negotiated_caps_str << "\",\n";
    ss << "  \"pass\": " << (metrics.pass ? "true" : "false") << ",\n";
    ss << "  \"status\": \"" << (metrics.pass ? "PASS" : "FAIL") << "\",\n";
    ss << "  \"failure_reason\": \"" << metrics.failure_reason << "\",\n";

    ss << "  \"discovered_elements\": [\n";
    for (size_t i = 0; i < discovery.candidates.size(); ++i) {
        const auto& c = discovery.candidates[i];
        ss << "    {\n";
        ss << "      \"name\": \"" << c.element_name << "\",\n";
        ss << "      \"is_hardware\": " << (c.is_hardware ? "true" : "false") << ",\n";
        ss << "      \"available\": " << (c.available ? "true" : "false") << ",\n";
        ss << "      \"viable\": " << (c.viable ? "true" : "false") << ",\n";
        ss << "      \"notes\": \"" << c.notes << "\"\n";
        ss << "    }" << (i + 1 < discovery.candidates.size() ? "," : "") << "\n";
    }
    ss << "  ]\n";
    ss << "}\n";

    return ss.str();
}

} // namespace aa::decode_probe
