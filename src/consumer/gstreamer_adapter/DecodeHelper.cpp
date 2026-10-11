// SPDX-License-Identifier: GPL-3.0-or-later
// Task 23 consumer-side GStreamer decode helper, adapter-zone implementation.
// This directory (src/consumer/gstreamer_adapter/) is the only place in the
// consumer library that may include GStreamer headers (tests/architecture
// boundary_scan.py adapter zone); include/aa/consumer stays GStreamer-free.
#include <aa/consumer/Decode.hpp>
#include <aa/protocol/Messages.hpp> // protocol::valid_video_profile (budget profile)

#include <gst/app/gstappsink.h>
#include <gst/app/gstappsrc.h>
#include <gst/gst.h>
#include <gst/video/video.h>

#include <algorithm>
#include <cstring>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <utility>

namespace aa::consumer {
namespace {

void ensure_gstreamer_initialized() {
    static std::once_flag flag;
    std::call_once(flag, [] { gst_init(nullptr, nullptr); });
}

// Known decoder table and selection policy mirror tools/aa-decode-probe
// (src/discovery.cpp) exactly; consumer_decode_probe cross-checks both at
// runtime. Viability is measured (factory + instantiate + READY transition),
// never assumed from an installed JetPack. requires_nvmm_conversion marks
// NVMM-only decoder output: nvv4l2decoder emits only
// video/x-raw(memory:NVMM) (task-9 element inspection), so its usable path
// additionally needs the runtime-detected NVMM conversion element.
struct KnownDecoder final {
    const char* element;
    bool hardware;
    bool requires_nvmm_conversion;
};

constexpr KnownDecoder kKnownDecoders[] = {
    {"nvv4l2decoder", true, true},
    {"openh264dec", false, false},
    {"avdec_h264", false, false},
    {"nvh264dec", true, false},
};

constexpr const char* kNvmmConversionElement = "nvvidconv";

constexpr std::size_t kMaxPendingCorrelations = 512;
constexpr std::uint64_t kMaxFeedBacklogBytes = 4ULL * 1024 * 1024;

[[nodiscard]] BackendCandidate probe_candidate(const KnownDecoder& known) {
    BackendCandidate candidate;
    candidate.element = known.element;
    candidate.hardware = known.hardware;
    candidate.requires_nvmm_conversion = known.requires_nvmm_conversion;

    GstElementFactory* factory = gst_element_factory_find(known.element);
    if (factory == nullptr) {
        candidate.available = false;
        candidate.viable = false;
        candidate.cause = "Element not found in GStreamer registry";
        return candidate;
    }
    gst_object_unref(factory);
    candidate.available = true;

    GstElement* element = gst_element_factory_make(known.element, "aa_consumer_probe");
    if (element == nullptr) {
        candidate.viable = false;
        candidate.cause = "Failed to instantiate element";
        return candidate;
    }
    const GstStateChangeReturn state = gst_element_set_state(element, GST_STATE_READY);
    if (state == GST_STATE_CHANGE_FAILURE) {
        candidate.viable = false;
        candidate.cause = "Failed to transition to GST_STATE_READY";
        gst_element_set_state(element, GST_STATE_NULL);
        gst_object_unref(element);
        return candidate;
    }
    gst_element_set_state(element, GST_STATE_NULL);
    gst_object_unref(element);
    candidate.viable = true;
    candidate.cause = "Instantiated and transitioned to READY successfully";
    return candidate;
}

[[nodiscard]] std::string nvidia_disable_cause(const std::vector<BackendCandidate>& backends) {
    std::string cause = "disabled:";
    bool first = true;
    for (const auto& candidate : backends) {
        if (!candidate.hardware) {
            continue;
        }
        if (!first) {
            cause += ";";
        }
        first = false;
        cause += " " + candidate.element + " " + (candidate.available ? "not viable" : "absent")
               + " (" + candidate.cause + ")";
    }
    return cause;
}

} // namespace

void apply_output_path_policy(BackendCandidate& candidate, bool nvmm_conversion_available) {
    if (!candidate.viable || !candidate.requires_nvmm_conversion || nvmm_conversion_available) {
        return;
    }
    candidate.viable = false;
    candidate.cause = "NVMM output path incomplete: " + std::string{kNvmmConversionElement}
        + " unavailable; decoder READY-viable but unusable without the NVMM conversion";
}

BackendSelection select_backend(const DecodeReport& report, DecodeBackend request) {
    BackendSelection selection;
    const BackendCandidate* chosen = nullptr;
    if (request != DecodeBackend::software) {
        for (const auto& candidate : report.backends) {
            if (candidate.hardware && candidate.viable) {
                chosen = &candidate;
                break;
            }
        }
    }
    if (chosen == nullptr && request != DecodeBackend::nvidia) {
        for (const auto& candidate : report.backends) {
            if (!candidate.hardware && candidate.viable) {
                chosen = &candidate;
                break;
            }
        }
    }
    if (chosen == nullptr) {
        selection.usable = false;
        selection.nvidia_cause = (request == DecodeBackend::software)
            ? "disabled: software backend explicitly requested"
            : nvidia_disable_cause(report.backends);
        return selection;
    }
    selection.element = chosen->element;
    selection.hardware = chosen->hardware;
    selection.nvidia_enabled = chosen->hardware;
    if (chosen->hardware) {
        selection.nvidia_cause = "enabled: " + chosen->element
            + " runtime-detected with a complete usable output path (task-9 reduced scope: no "
              "decode latency budget claim attached)";
    } else if (request == DecodeBackend::software) {
        selection.nvidia_cause = "disabled: software backend explicitly requested";
    } else {
        selection.nvidia_cause = nvidia_disable_cause(report.backends);
    }
    return selection;
}

struct DecodeHelper::Impl final {
    DecodeReport report{};
    protocol::VideoProfile caps{};
    FrameCallback on_frame{};
    std::uint64_t frame_duration_ns{};

    GstElement* pipeline{nullptr};
    GstElement* appsrc{nullptr};
    GstElement* appsink{nullptr};
    GstBus* bus{nullptr};

    struct PendingMeta final {
        std::uint64_t frame{};
        std::uint64_t sequence{};
        std::uint64_t timestamp_ns{};
    };
    std::map<std::uint64_t, PendingMeta> pending{};
    bool eos_sent{false};

    ~Impl() { release(); }

    void release() noexcept {
        if (pipeline != nullptr) {
            gst_element_set_state(pipeline, GST_STATE_NULL);
        }
        if (bus != nullptr) {
            gst_object_unref(bus);
            bus = nullptr;
        }
        if (pipeline != nullptr) {
            gst_object_unref(pipeline);
            pipeline = nullptr;
        }
        appsrc = nullptr;
        appsink = nullptr;
        pending.clear();
    }

    [[nodiscard]] core::Result<void> bus_error() const {
        if (bus == nullptr) {
            return core::Result<void>{};
        }
        GstMessage* message = gst_bus_pop_filtered(bus, GST_MESSAGE_ERROR);
        if (message == nullptr) {
            return core::Result<void>{};
        }
        GError* error = nullptr;
        gchar* debug = nullptr;
        gst_message_parse_error(message, &error, &debug);
        if (error != nullptr) {
            g_error_free(error);
        }
        g_free(debug);
        gst_message_unref(message);
        return core::Result<void>{Error{ErrorCode::channel_decode_failed}};
    }

    // appsrc -> h264parse -> decoder [-> nvvidconv when the decoder output is
    // NVMM-only] -> videoconvert -> appsink(I420). Fails (and releases
    // everything) when any element is missing, the link fails or PLAYING is
    // refused, so callers can record the cause and retry another backend.
    [[nodiscard]] core::Result<void> build_pipeline(const std::string& decoder_element,
                                                    bool needs_nvmm_conversion);
};

DecodeHelper::DecodeHelper(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

DecodeHelper::~DecodeHelper() = default;

DecodeHelper::DecodeHelper(DecodeHelper&& other) noexcept = default;

DecodeHelper& DecodeHelper::operator=(DecodeHelper&& other) noexcept = default;

DecodeReport DecodeHelper::probe_backends() {
    ensure_gstreamer_initialized();
    DecodeReport report;

    const BackendCandidate conversion =
        probe_candidate(KnownDecoder{kNvmmConversionElement, false, false});
    report.nvmm_conversion_available = conversion.available && conversion.viable;
    report.nvmm_conversion_cause = conversion.cause;

    report.backends.reserve(std::size(kKnownDecoders));
    for (const auto& known : kKnownDecoders) {
        BackendCandidate candidate = probe_candidate(known);
        apply_output_path_policy(candidate, report.nvmm_conversion_available);
        report.backends.push_back(std::move(candidate));
    }

    const BackendSelection selection = select_backend(report, DecodeBackend::automatic);
    report.nvidia_enabled = selection.nvidia_enabled;
    report.nvidia_cause = selection.nvidia_cause;
    return report;
}

core::Result<void> DecodeHelper::Impl::build_pipeline(const std::string& decoder_element,
                                                      bool needs_nvmm_conversion) {
    GstElement* pipeline = gst_pipeline_new("aa_consumer_decode");
    GstElement* source = gst_element_factory_make("appsrc", "src");
    GstElement* parser = gst_element_factory_make("h264parse", "parser");
    GstElement* decoder = gst_element_factory_make(decoder_element.c_str(), "decoder");
    GstElement* nvmm_convert =
        needs_nvmm_conversion ? gst_element_factory_make(kNvmmConversionElement, "nvmm_convert")
                              : nullptr;
    GstElement* convert = gst_element_factory_make("videoconvert", "convert");
    GstElement* sink = gst_element_factory_make("appsink", "sink");
    const bool complete = pipeline != nullptr && source != nullptr && parser != nullptr
        && decoder != nullptr && convert != nullptr && sink != nullptr
        && (!needs_nvmm_conversion || nvmm_convert != nullptr);
    if (!complete) {
        if (pipeline != nullptr) {
            gst_object_unref(pipeline);
        }
        if (source != nullptr) {
            gst_object_unref(source);
        }
        if (parser != nullptr) {
            gst_object_unref(parser);
        }
        if (decoder != nullptr) {
            gst_object_unref(decoder);
        }
        if (nvmm_convert != nullptr) {
            gst_object_unref(nvmm_convert);
        }
        if (convert != nullptr) {
            gst_object_unref(convert);
        }
        if (sink != nullptr) {
            gst_object_unref(sink);
        }
        return core::Result<void>{Error{ErrorCode::channel_decode_failed}};
    }

    GstCaps* src_caps = gst_caps_new_simple("video/x-h264",
                                            "stream-format", G_TYPE_STRING, "byte-stream",
                                            "alignment", G_TYPE_STRING, "au",
                                            "width", G_TYPE_INT, static_cast<int>(caps.width),
                                            "height", G_TYPE_INT, static_cast<int>(caps.height),
                                            "framerate", GST_TYPE_FRACTION,
                                            static_cast<int>(caps.fps), 1,
                                            nullptr);
    g_object_set(source,
                 "caps", src_caps,
                 "format", GST_FORMAT_TIME,
                 "is-live", FALSE,
                 "block", FALSE,
                 nullptr);
    gst_caps_unref(src_caps);

    GstCaps* sink_caps = gst_caps_new_simple("video/x-raw", "format", G_TYPE_STRING, "I420", nullptr);
    g_object_set(sink,
                 "emit-signals", FALSE,
                 "sync", FALSE,
                 "max-buffers", 10,
                 "drop", FALSE,
                 "caps", sink_caps,
                 nullptr);
    gst_caps_unref(sink_caps);

    if (needs_nvmm_conversion) {
        gst_bin_add_many(GST_BIN(pipeline), source, parser, decoder, nvmm_convert, convert, sink,
                         nullptr);
    } else {
        gst_bin_add_many(GST_BIN(pipeline), source, parser, decoder, convert, sink, nullptr);
    }
    const bool linked = needs_nvmm_conversion
        ? gst_element_link_many(source, parser, decoder, nvmm_convert, convert, sink, nullptr)
        : gst_element_link_many(source, parser, decoder, convert, sink, nullptr);
    if (!linked) {
        gst_object_unref(pipeline);
        return core::Result<void>{Error{ErrorCode::channel_decode_failed}};
    }

    if (gst_element_set_state(pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
        gst_element_set_state(pipeline, GST_STATE_NULL);
        gst_object_unref(pipeline);
        return core::Result<void>{Error{ErrorCode::channel_decode_failed}};
    }

    this->pipeline = pipeline;
    this->appsrc = source;
    this->appsink = sink;
    this->bus = gst_element_get_bus(pipeline);
    return core::Result<void>{};
}

core::Result<DecodeHelper> DecodeHelper::open(DecodeConfig config) {
    ensure_gstreamer_initialized();

    // Caps negotiation against the channel budget profile: anything outside
    // 1280x720@30 / 800x480@30 fails typed before any pipeline is built.
    if (!protocol::valid_video_profile(config.caps)) {
        return core::Result<DecodeHelper>{Error{ErrorCode::channel_caps_unsupported}};
    }

    DecodeReport report = probe_backends();
    report.requested = config.backend;

    BackendSelection selection = select_backend(report, config.backend);
    if (!selection.usable) {
        return core::Result<DecodeHelper>{Error{ErrorCode::channel_decode_unavailable}};
    }

    auto impl = std::make_unique<Impl>();
    impl->caps = config.caps;
    impl->on_frame = std::move(config.on_frame);
    impl->frame_duration_ns = 1'000'000'000ULL / static_cast<std::uint64_t>(config.caps.fps);

    const auto needs_nvmm_conversion = [&](const std::string& element) {
        for (const auto& candidate : report.backends) {
            if (candidate.element == element) {
                return candidate.requires_nvmm_conversion;
            }
        }
        return false;
    };

    std::string nvidia_cause = selection.nvidia_cause;
    auto built = impl->build_pipeline(selection.element, needs_nvmm_conversion(selection.element));
    if (!built.has_value()) {
        for (auto& candidate : report.backends) {
            if (candidate.element == selection.element) {
                candidate.viable = false;
                candidate.cause = "full output path could not be constructed or started; disabled";
            }
        }
        if (config.backend != DecodeBackend::automatic || !selection.hardware) {
            // Explicit requests never silently downgrade; automatic only falls
            // back from a hardware candidate whose full path failed.
            return core::Result<DecodeHelper>{Error{ErrorCode::channel_decode_failed}};
        }
        nvidia_cause = "disabled: " + selection.element
            + " full output path could not be constructed or started; software backend selected "
              "(functional fallback only, not qualified - task-9 reduced scope)";
        selection = select_backend(report, DecodeBackend::software);
        if (!selection.usable) {
            return core::Result<DecodeHelper>{Error{ErrorCode::channel_decode_unavailable}};
        }
        built = impl->build_pipeline(selection.element, needs_nvmm_conversion(selection.element));
        if (!built.has_value()) {
            return core::Result<DecodeHelper>{Error{ErrorCode::channel_decode_failed}};
        }
    }

    report.selected_element = selection.element;
    report.nvidia_enabled = selection.hardware;
    report.nvidia_cause = nvidia_cause;
    impl->report = std::move(report);
    return core::Result<DecodeHelper>{DecodeHelper{std::move(impl)}};
}

core::Result<void> DecodeHelper::submit(const ipc::VideoAccessUnit& access_unit) {
    if (!impl_ || impl_->pipeline == nullptr) {
        return core::Result<void>{Error{ErrorCode::invalid_argument}};
    }
    if (impl_->eos_sent) {
        return core::Result<void>{Error{ErrorCode::invalid_argument}};
    }
    if (access_unit.payload.empty()) {
        return core::Result<void>{Error{ErrorCode::invalid_argument}};
    }
    if (impl_->pending.find(access_unit.timestamp_ns) != impl_->pending.end()) {
        return core::Result<void>{Error{ErrorCode::invalid_argument}};
    }
    if (gst_app_src_get_current_level_bytes(GST_APP_SRC(impl_->appsrc)) > kMaxFeedBacklogBytes
        || impl_->pending.size() >= kMaxPendingCorrelations) {
        return core::Result<void>{Error{ErrorCode::ipc_queue_full}};
    }
    if (auto failed = impl_->bus_error(); !failed.has_value()) {
        return failed;
    }

    GstBuffer* buffer = gst_buffer_new_allocate(nullptr, access_unit.payload.size(), nullptr);
    if (buffer == nullptr) {
        return core::Result<void>{Error{ErrorCode::channel_decode_failed}};
    }
    gst_buffer_fill(buffer, 0, access_unit.payload.data(), access_unit.payload.size());
    GST_BUFFER_PTS(buffer) = static_cast<GstClockTime>(access_unit.timestamp_ns);
    GST_BUFFER_DTS(buffer) = static_cast<GstClockTime>(access_unit.timestamp_ns);
    GST_BUFFER_DURATION(buffer) = static_cast<GstClockTime>(impl_->frame_duration_ns);

    const GstFlowReturn flow = gst_app_src_push_buffer(GST_APP_SRC(impl_->appsrc), buffer);
    if (flow != GST_FLOW_OK) {
        return core::Result<void>{Error{ErrorCode::channel_decode_failed}};
    }
    impl_->pending.emplace(access_unit.timestamp_ns,
                           Impl::PendingMeta{access_unit.frame, access_unit.sequence,
                                            access_unit.timestamp_ns});
    return core::Result<void>{};
}

core::Result<void> DecodeHelper::end_of_stream() {
    if (!impl_ || impl_->pipeline == nullptr || impl_->eos_sent) {
        return core::Result<void>{Error{ErrorCode::invalid_argument}};
    }
    if (auto failed = impl_->bus_error(); !failed.has_value()) {
        return failed;
    }
    if (gst_app_src_end_of_stream(GST_APP_SRC(impl_->appsrc)) != GST_FLOW_OK) {
        return core::Result<void>{Error{ErrorCode::channel_decode_failed}};
    }
    impl_->eos_sent = true;
    return core::Result<void>{};
}

core::Result<std::size_t> DecodeHelper::pump(core::Milliseconds wait) {
    if (!impl_ || impl_->pipeline == nullptr) {
        return core::Result<std::size_t>{Error{ErrorCode::invalid_argument}};
    }
    std::size_t delivered = 0;
    const std::uint64_t first_timeout =
        static_cast<std::uint64_t>(std::max<std::int64_t>(wait.count, 0)) * 1'000'000ULL;
    bool first_pull = true;

    while (true) {
        if (auto failed = impl_->bus_error(); !failed.has_value()) {
            return core::Result<std::size_t>{failed.error()};
        }
        const std::uint64_t timeout = first_pull ? first_timeout : 0ULL;
        first_pull = false;
        GstSample* sample = gst_app_sink_try_pull_sample(GST_APP_SINK(impl_->appsink), timeout);
        if (sample == nullptr) {
            // The pull may have raced the error message; re-check before returning.
            if (auto failed = impl_->bus_error(); !failed.has_value()) {
                return core::Result<std::size_t>{failed.error()};
            }
            break;
        }

        GstCaps* caps = gst_sample_get_caps(sample);
        GstBuffer* buffer = gst_sample_get_buffer(sample);
        GstVideoInfo info;
        GstVideoFrame video_frame;
        const bool mapped = caps != nullptr && buffer != nullptr
            && gst_video_info_from_caps(&info, caps)
            && GST_VIDEO_INFO_FORMAT(&info) == GST_VIDEO_FORMAT_I420
            && gst_video_frame_map(&video_frame, &info, buffer, GST_MAP_READ);
        if (!mapped) {
            gst_sample_unref(sample);
            return core::Result<std::size_t>{Error{ErrorCode::channel_decode_failed}};
        }

        const std::uint32_t width = static_cast<std::uint32_t>(info.width);
        const std::uint32_t height = static_cast<std::uint32_t>(info.height);
        if (width != impl_->caps.width || height != impl_->caps.height) {
            // No silent fallback to wrong dimensions: negotiated caps were not
            // honored by the stream, fail typed.
            gst_video_frame_unmap(&video_frame);
            gst_sample_unref(sample);
            return core::Result<std::size_t>{Error{ErrorCode::channel_caps_unsupported}};
        }

        if (impl_->report.negotiated_caps.empty()) {
            gchar* caps_text = gst_caps_to_string(caps);
            if (caps_text != nullptr) {
                impl_->report.negotiated_caps = caps_text;
                g_free(caps_text);
            }
        }

        DecodedFrame frame;
        frame.width = static_cast<std::uint16_t>(width);
        frame.height = static_cast<std::uint16_t>(height);
        const std::size_t plane_width = width;
        const std::size_t plane_height = height;
        const std::size_t chroma_width = plane_width / 2;
        const std::size_t chroma_height = plane_height / 2;
        frame.i420.resize(plane_width * plane_height + 2 * (chroma_width * chroma_height));

        const std::size_t planes[3] = {plane_width * plane_height,
                                       chroma_width * chroma_height,
                                       chroma_width * chroma_height};
        const std::size_t rows[3] = {plane_height, chroma_height, chroma_height};
        const std::size_t cols[3] = {plane_width, chroma_width, chroma_width};
        std::size_t offset = 0;
        for (std::uint32_t plane = 0; plane < 3; ++plane) {
            const auto* source = static_cast<const std::uint8_t*>(
                GST_VIDEO_FRAME_PLANE_DATA(&video_frame, plane));
            const auto stride = GST_VIDEO_FRAME_PLANE_STRIDE(&video_frame, plane);
            for (std::size_t row = 0; row < rows[plane]; ++row) {
                std::memcpy(frame.i420.data() + offset + row * cols[plane],
                            source + static_cast<std::ptrdiff_t>(row)
                                        * static_cast<std::ptrdiff_t>(stride),
                            cols[plane]);
            }
            offset += planes[plane];
        }
        gst_video_frame_unmap(&video_frame);

        const std::uint64_t pts = static_cast<std::uint64_t>(GST_BUFFER_PTS(buffer));
        const auto meta = impl_->pending.find(pts);
        if (meta == impl_->pending.end()) {
            ++impl_->report.frames_unmatched;
        } else {
            frame.frame = meta->second.frame;
            frame.sequence = meta->second.sequence;
            frame.timestamp_ns = meta->second.timestamp_ns;
            impl_->pending.erase(meta);
            if (impl_->on_frame) {
                impl_->on_frame(frame);
            }
            ++impl_->report.frames_delivered;
            ++delivered;
        }
        gst_sample_unref(sample);
    }

    return core::Result<std::size_t>{delivered};
}

void DecodeHelper::close() noexcept {
    if (impl_) {
        impl_->release();
    }
}

bool DecodeHelper::closed() const noexcept {
    return !impl_ || impl_->pipeline == nullptr;
}

const DecodeReport& DecodeHelper::report() const noexcept {
    static const DecodeReport empty{};
    return impl_ ? impl_->report : empty;
}

} // namespace aa::consumer
