// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <aa/consumer/Decode.hpp>
#include <cstdio>
#include <string>
#include <string>

namespace aa::tools {
inline std::string unavailable_codec(protocol::VideoCodec codec, consumer::DecodeBackend backend,
                                     const consumer::DecodeReport& report) {
    const bool nvidia = std::any_of(report.backends.begin(),report.backends.end(),[](const auto& b) { return b.element == "nvv4l2decoder" && b.viable; });
    if (report.software_path_available && (backend == consumer::DecodeBackend::nvidia ? nvidia : consumer::select_backend(report,backend).usable)) { return {}; }
    std::string reason = "channel_decode_unavailable: " + std::string{protocol::codec_name(codec)}
        + " not available on this host; requires ";
    for (const auto& b : report.backends) {
        if (backend == consumer::DecodeBackend::nvidia && b.element != "nvv4l2decoder") { continue; }
        if ((backend == consumer::DecodeBackend::software && b.hardware)
            || (backend == consumer::DecodeBackend::nvidia && !b.hardware)) { continue; }
        reason += b.element + " (" + b.cause + "); ";
    }
    if (!report.software_path_available) { reason += report.software_path_cause; }
    if (backend == consumer::DecodeBackend::nvidia) { reason += "NVMM conversion: nvvidconv; "; }
    return reason;
}
inline std::string unavailable_profiles(std::span<const protocol::VideoProfile> profiles, consumer::DecodeBackend backend) {
    std::string errors;
    for (const auto codec : protocol::kVideoCodecs) {
        if (std::none_of(profiles.begin(),profiles.end(),[&](const auto& p) { return p.codec == codec; })) { continue; }
        auto reason = unavailable_codec(codec,backend,consumer::DecodeHelper::probe_backends(codec));
        if (!reason.empty()) { if (!errors.empty()) { errors += "\n"; } errors += reason; }
    }
    return errors;
}
inline bool parse_decoder(std::string_view value, consumer::DecodeBackend& out) {
    if (value == "software") { out = consumer::DecodeBackend::software; }
    else if (value == "hardware") { out = consumer::DecodeBackend::nvidia; }
    else if (value == "auto") { out = consumer::DecodeBackend::automatic; }
    else { return false; }
    return true;
}
// One JSON record per line; callers write it to their JSON descriptor.
inline std::string list_video_capabilities() {
    std::string out;
    for (const auto codec : protocol::kVideoCodecs) {
        const auto report = consumer::DecodeHelper::probe_backends(codec);
        const bool nvidia = std::any_of(report.backends.begin(),report.backends.end(),[](const auto& b) { return b.element == "nvv4l2decoder" && b.viable; });
        const bool software = report.software_path_available && consumer::select_backend(report,consumer::DecodeBackend::software).usable;
        out += std::string{"{\"codec\":\""} + std::string{protocol::codec_name(codec)} + "\",\"parser_path\":"
            + (report.software_path_available ? "true" : "false") + ",\"software\":" + (software ? "true" : "false")
            + ",\"hardware\":" + (report.software_path_available && nvidia ? "true" : "false") + ",\"elements\":[";
        bool first = true;
        for (const auto& b : report.backends) {
            out += std::string{first ? "" : ","} + "{\"name\":\"" + b.element + "\",\"installed\":"
                + (b.available ? "true" : "false") + ",\"usable\":" + (b.viable ? "true" : "false") + "}";
            first = false;
        }
        out += "]}\n";
    }
    return out;
}
} // namespace aa::tools
