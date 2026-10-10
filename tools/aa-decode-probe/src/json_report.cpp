// SPDX-License-Identifier: GPL-3.0-or-later
#include "json_report.hpp"

#include <iomanip>
#include <sstream>

namespace aa::decode_probe {

static std::string escape_json(const std::string& value) {
    std::ostringstream out;
    for (unsigned char c : value) {
        switch (c) {
        case '"': out << "\\\""; break;
        case '\\': out << "\\\\"; break;
        case '\n': out << "\\n"; break;
        case '\r': out << "\\r"; break;
        case '\t': out << "\\t"; break;
        case '\b': out << "\\b"; break;
        case '\f': out << "\\f"; break;
        default:
            if (c < 0x20 || c >= 0x80) {
                out << "\\u00" << std::hex << std::setw(2) << std::setfill('0') << static_cast<unsigned>(c);
            } else out << c;
        }
    }
    return out.str();
}

std::string format_json_report(const ProbeOptions& options,
                               const DiscoveryResult& discovery,
                               const ProbeMetrics& metrics) {
    std::ostringstream ss;
    ss << std::fixed << std::setprecision(2);

    ss << "{\n";
    ss << "  \"schema\": \"aa-decode-probe/2\",\n";
    ss << "  \"profile\": \"" << escape_json(options.profile) << "\",\n";
    ss << "  \"bitrate\": \"" << escape_json(options.bitrate) << "\",\n";
    ss << "  \"configured_duration_s\": " << options.duration_s << ",\n";
    ss << "  \"decoder\": \"" << escape_json(metrics.decoder_used) << "\",\n";
    ss << "  \"is_hardware\": " << (metrics.is_hardware ? "true" : "false") << ",\n";
    ss << "  \"duration_s\": " << metrics.duration_s << ",\n";
    ss << "  \"frames_fed\": " << metrics.frames_fed << ",\n";
    ss << "  \"frames_decoded\": " << metrics.frames_decoded << ",\n";
    ss << "  \"dropped_frames\": " << metrics.dropped_frames << ",\n";
    ss << "  \"requested_width\": " << options.width << ",\n";
    ss << "  \"requested_height\": " << options.height << ",\n";
    ss << "  \"requested_fps\": " << options.fps << ",\n";
    ss << "  \"requested_bitrate_bps\": " << options.bitrate_bps << ",\n";
    ss << "  \"observed_width\": " << metrics.observed_width << ",\n";
    ss << "  \"observed_height\": " << metrics.observed_height << ",\n";
    ss << "  \"observed_fps\": " << metrics.observed_fps << ",\n";
    ss << "  \"observed_bitrate_bps\": " << metrics.observed_bitrate_bps << ",\n";
    ss << "  \"bytes_fed\": " << metrics.bytes_fed << ",\n";
    ss << "  \"feed_duration_s\": " << metrics.feed_duration_s << ",\n";
    ss << "  \"latency_samples\": " << metrics.latency_samples << ",\n";
    ss << "  \"unmatched_frames\": " << metrics.unmatched_frames << ",\n";
    ss << "  \"smoke_pass\": " << (metrics.smoke_pass ? "true" : "false") << ",\n";
    ss << "  \"requested_workload_met\": " << (metrics.requested_workload_met ? "true" : "false") << ",\n";
    ss << "  \"target_qualified\": false,\n";
    ss << "  \"p95_latency_ms\": " << metrics.p95_latency_ms << ",\n";
    ss << "  \"max_latency_ms\": " << metrics.max_latency_ms << ",\n";
    ss << "  \"avg_latency_ms\": " << metrics.avg_latency_ms << ",\n";
    ss << "  \"cpu_percent\": " << metrics.cpu_percent << ",\n";
    ss << "  \"caps_negotiated\": \"" << escape_json(metrics.negotiated_caps_str) << "\",\n";
    ss << "  \"pass\": " << (metrics.pass ? "true" : "false") << ",\n";
    ss << "  \"status\": \"" << (metrics.pass ? "PASS" : "FAIL") << "\",\n";
    ss << "  \"failure_reason\": \"" << escape_json(metrics.failure_reason) << "\",\n";

    ss << "  \"discovered_elements\": [\n";
    for (size_t i = 0; i < discovery.candidates.size(); ++i) {
        const auto& c = discovery.candidates[i];
        ss << "    {\n";
        ss << "      \"name\": \"" << escape_json(c.element_name) << "\",\n";
        ss << "      \"is_hardware\": " << (c.is_hardware ? "true" : "false") << ",\n";
        ss << "      \"available\": " << (c.available ? "true" : "false") << ",\n";
        ss << "      \"viable\": " << (c.viable ? "true" : "false") << ",\n";
        ss << "      \"notes\": \"" << escape_json(c.notes) << "\"\n";
        ss << "    }" << (i + 1 < discovery.candidates.size() ? "," : "") << "\n";
    }
    ss << "  ]\n";
    ss << "}\n";

    return ss.str();
}

} // namespace aa::decode_probe
