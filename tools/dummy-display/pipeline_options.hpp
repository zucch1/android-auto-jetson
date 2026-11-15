// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <aa/consumer/Decode.hpp>
#include <algorithm>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
namespace aa::display {
enum class Decoder { software, hardware, automatic };
enum class Presentation { cpu, gpu, zerocopy };
enum class MediaThread { gui, worker };
struct PipelineOptions final {
    Decoder decoder{Decoder::software};
    Presentation presentation{Presentation::cpu};
    MediaThread media_thread{MediaThread::gui};
    // The original flag deliberately defers backend admission until projection.
    bool legacy_decoder_backend{};
};
struct PipelineRefusal final {
    ErrorCode code;
    std::string message;
};
inline bool parse_pipeline_option(PipelineOptions& o, std::string_view option, std::string_view value) {
    if (option == "--decoder" || option == "--decoder-backend") {
        o.legacy_decoder_backend = option == "--decoder-backend";
        if (value == "software") { o.decoder = Decoder::software; }
        else if (value == "hardware" || (option == "--decoder-backend" && value == "nvidia")) { o.decoder = Decoder::hardware; }
        else if (value == "auto" || (option == "--decoder-backend" && value == "automatic")) { o.decoder = Decoder::automatic; }
        else { return false; }
    } else if (option == "--presentation") {
        if (value == "cpu") { o.presentation = Presentation::cpu; }
        else if (value == "gpu") { o.presentation = Presentation::gpu; }
        else if (value == "zerocopy") { o.presentation = Presentation::zerocopy; }
        else { return false; }
    } else if (option == "--media-thread") {
        if (value == "gui") { o.media_thread = MediaThread::gui; }
        else if (value == "worker") { o.media_thread = MediaThread::worker; }
        else { return false; }
    } else { return false; }
    return true;
}
namespace detail {
inline std::optional<protocol::VideoCodec> infer_codec(const consumer::DecodeReport& host) {
    if (host.codec) { return *host.codec; }
    for (const auto& b : host.backends) {
        if (b.element == "avdec_h265") { return protocol::VideoCodec::h265; }
        if (b.element == "vp9dec" || b.element == "avdec_vp9") { return protocol::VideoCodec::vp9; }
        if (b.element == "dav1ddec" || b.element == "av1dec") { return protocol::VideoCodec::av1; }
        if (b.element == "openh264dec" || b.element == "avdec_h264" || b.element == "nvh264dec") {
            return protocol::VideoCodec::h264;
        }
    }
    if (host.software_path_cause.find("h265parse") != std::string::npos) { return protocol::VideoCodec::h265; }
    if (host.software_path_cause.find("vp9parse") != std::string::npos) { return protocol::VideoCodec::vp9; }
    if (host.software_path_cause.find("av1parse") != std::string::npos) { return protocol::VideoCodec::av1; }
    if (host.software_path_cause.find("h264parse") != std::string::npos) { return protocol::VideoCodec::h264; }
    return {};
}
inline std::string_view parser_for(std::optional<protocol::VideoCodec> codec) {
    if (!codec) { return "parser"; }
    switch (*codec) {
    case protocol::VideoCodec::h264: return "h264parse";
    case protocol::VideoCodec::h265: return "h265parse";
    case protocol::VideoCodec::vp9: return "vp9parse";
    case protocol::VideoCodec::av1: return "av1parse";
    }
    return "parser";
}
inline std::string format_unavailable_codec(const PipelineOptions& o,
                                            std::optional<protocol::VideoCodec> codec,
                                            consumer::DecodeBackend backend,
                                            const consumer::DecodeReport& host) {
    const std::string_view mode = o.decoder == Decoder::hardware ? "hardware"
        : o.decoder == Decoder::automatic ? "auto" : "software";
    const std::string_view codec_label = codec ? std::string_view{protocol::codec_name(*codec)} : std::string_view{"video"};
    std::string details;
    if (!host.software_path_available) {
        if (!host.software_path_cause.empty()) {
            details += host.software_path_cause;
        } else {
            details += "requires appsrc, ";
            details += parser_for(codec);
            details += ", videoconvert and appsink; ";
        }
    }
    if (!consumer::select_backend(host, backend).usable) {
        bool matched = false;
        for (const auto& b : host.backends) {
            if ((backend == consumer::DecodeBackend::software && b.hardware)
                || (backend == consumer::DecodeBackend::nvidia && !b.hardware)) {
                continue;
            }
            matched = true;
            if (!b.viable) {
                details += b.element + " (" + (b.cause.empty() ? "unavailable" : b.cause) + "); ";
                if (b.requires_nvmm_conversion && !host.nvmm_conversion_available
                    && b.cause.find("nvvidconv") == std::string::npos) {
                    details += "nvvidconv (" + (host.nvmm_conversion_cause.empty() ? "unavailable" : host.nvmm_conversion_cause) + "); ";
                }
            }
        }
        if (!matched) {
            details += "no usable decoder element; ";
        }
    }
    while (details.size() >= 2 && details.compare(details.size() - 2, 2, "; ") == 0) {
        details.resize(details.size() - 2);
    }
    if (details.empty()) {
        details = "decoder path unavailable";
    }
    std::string message;
    message.reserve(mode.size() + 1 + codec_label.size() + 19 + details.size());
    message.append(mode).append(" ").append(codec_label).append(" path unavailable: ").append(details);
    return message;
}
} // namespace detail

inline std::optional<PipelineRefusal> check_pipeline(
    const PipelineOptions& o,
    std::span<const std::pair<std::optional<protocol::VideoCodec>, const consumer::DecodeReport*>> hosts) {
    if (o.presentation == Presentation::zerocopy && o.decoder != Decoder::hardware) {
        return PipelineRefusal{ErrorCode::config_invalid, "zerocopy requires --decoder hardware"};
    }
    const bool nvidia_present = std::any_of(hosts.begin(), hosts.end(), [](const auto& entry) {
        return std::any_of(entry.second->backends.begin(), entry.second->backends.end(), [](const auto& b) {
            return b.element == "nvv4l2decoder" && b.available;
        });
    });
    if (o.presentation != Presentation::cpu && !nvidia_present) {
        return PipelineRefusal{ErrorCode::channel_decode_unavailable,
            "presentation not available on this host: nvv4l2decoder absent"};
    }
    if (o.decoder == Decoder::hardware && !o.legacy_decoder_backend && !nvidia_present) {
        return PipelineRefusal{ErrorCode::channel_decode_unavailable,
            "hardware not available on this host: nvv4l2decoder is absent"};
    }
    if (o.presentation != Presentation::cpu) {
        return PipelineRefusal{ErrorCode::channel_unsupported_capability,
            "GPU/zerocopy presentation is not yet implemented"};
    }
    if (o.media_thread != MediaThread::gui) {
        return PipelineRefusal{ErrorCode::channel_unsupported_capability,
            "worker media thread is not yet implemented"};
    }
    if (!o.legacy_decoder_backend) {
        const auto backend = o.decoder == Decoder::hardware ? consumer::DecodeBackend::nvidia
            : o.decoder == Decoder::automatic ? consumer::DecodeBackend::automatic
                                              : consumer::DecodeBackend::software;
        std::string unavailable;
        for (const auto& [codec, host] : hosts) {
            if (!host->software_path_available || !consumer::select_backend(*host, backend).usable) {
                if (!unavailable.empty()) { unavailable += "; "; }
                unavailable += detail::format_unavailable_codec(
                    o, codec ? codec : detail::infer_codec(*host), backend, *host);
            }
        }
        if (!unavailable.empty()) {
            return PipelineRefusal{ErrorCode::channel_decode_unavailable, std::move(unavailable)};
        }
    }
    return {};
}

inline std::optional<PipelineRefusal> check_pipeline(
    const PipelineOptions& o,
    std::span<const std::pair<protocol::VideoCodec, consumer::DecodeReport>> hosts) {
    std::vector<std::pair<std::optional<protocol::VideoCodec>, const consumer::DecodeReport*>> views;
    views.reserve(hosts.size());
    for (const auto& [codec, report] : hosts) {
        views.emplace_back(codec, &report);
    }
    return check_pipeline(o, views);
}

inline std::optional<PipelineRefusal> check_pipeline(
    const PipelineOptions& o, const consumer::DecodeReport& host,
    std::optional<protocol::VideoCodec> codec = std::nullopt) {
    const std::pair<std::optional<protocol::VideoCodec>, const consumer::DecodeReport*> single[] = {
        {codec, &host}};
    return check_pipeline(o, single);
}
} // namespace aa::display
