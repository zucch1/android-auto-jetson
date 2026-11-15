// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <aa/consumer/Decode.hpp>
#include <algorithm>
#include <string_view>
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
    std::string_view message;
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
inline std::optional<PipelineRefusal> check_pipeline(const PipelineOptions& o, const consumer::DecodeReport& host) {
    const bool nvidia_present = std::any_of(host.backends.begin(),host.backends.end(),[](const auto& b) {
        return b.element == "nvv4l2decoder" && b.available;
    });
    if (o.presentation != Presentation::cpu && !nvidia_present) {
        return PipelineRefusal{ErrorCode::channel_decode_unavailable,
            "presentation not available on this host: nvv4l2decoder absent; zerocopy requires --decoder hardware"};
    }
    if (o.presentation == Presentation::zerocopy && o.decoder != Decoder::hardware) {
        return PipelineRefusal{ErrorCode::config_invalid, "zerocopy requires --decoder hardware"};
    }
    if (o.decoder == Decoder::hardware && !o.legacy_decoder_backend) {
        const bool present = std::any_of(host.backends.begin(), host.backends.end(), [](const auto& b) {
            return b.element == "nvv4l2decoder" && b.available;
        });
        if (!present) { return PipelineRefusal{ErrorCode::channel_decode_unavailable, "hardware not available on this host: nvv4l2decoder is absent"}; }

    }
    if (o.presentation != Presentation::cpu) { return PipelineRefusal{ErrorCode::channel_unsupported_capability, "GPU/zerocopy presentation is not yet implemented"}; }
    if (o.media_thread != MediaThread::gui) { return PipelineRefusal{ErrorCode::channel_unsupported_capability, "worker media thread is not yet implemented"}; }
    if (!o.legacy_decoder_backend && (!host.software_path_available || !consumer::select_backend(host, o.decoder == Decoder::hardware ? consumer::DecodeBackend::nvidia : o.decoder == Decoder::automatic ? consumer::DecodeBackend::automatic : consumer::DecodeBackend::software).usable)) {
        return PipelineRefusal{ErrorCode::channel_decode_unavailable, "software H.264 path unavailable: requires appsrc, h264parse, openh264dec/avdec_h264, videoconvert and appsink"};
    }
    return {};
}
} // namespace aa::display
