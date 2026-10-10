// SPDX-License-Identifier: GPL-3.0-or-later
// Task 25 AAP audio decode, adapter-zone implementation. This directory
// (src/audio/gstreamer_adapter/) is the only place in the audio library that
// may include GStreamer headers (tests/architecture boundary_scan.py adapter
// zone); include/aa/audio stays GStreamer-free.
//
// Backend honesty mirrors the task-23 consumer decode helper: every known
// element is probed (factory find + instantiate + READY) and reported with a
// recorded cause; absence is a recorded fact, never an assumption.
#include <aa/audio/AudioDecode.hpp>

#include <gst/app/gstappsink.h>
#include <gst/app/gstappsrc.h>
#include <gst/gst.h>

#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace aa::audio {
namespace {

void ensure_gstreamer_initialized() {
    static std::once_flag flag;
    std::call_once(flag, [] { gst_init(nullptr, nullptr); });
}

struct KnownDecoder final {
    const char* element;
};

// Decode element alternatives per codec. PCM needs no codec decoder (the
// payload is already samples); AAC-LC needs one of these plus the aacparse
// framing element.
constexpr KnownDecoder kAacDecoders[] = {
    {"faad"},
    {"avdec_aac"},
};

constexpr const char* kAacParser = "aacparse";
constexpr const char* kConvertElement = "audioconvert";
constexpr const char* kResampleElement = "audioresample";

constexpr std::uint64_t kDecodeTimeoutNs = 500ULL * 1000ULL * 1000ULL;
constexpr std::uint64_t kDrainQuietNs = 25ULL * 1000ULL * 1000ULL;

[[nodiscard]] DecodeCandidate probe_element(const char* name) {
    DecodeCandidate candidate;
    candidate.element = name;

    GstElementFactory* factory = gst_element_factory_find(name);
    if (factory == nullptr) {
        candidate.available = false;
        candidate.viable = false;
        candidate.cause = "Element not found in GStreamer registry";
        return candidate;
    }
    gst_object_unref(factory);
    candidate.available = true;

    GstElement* element = gst_element_factory_make(name, "aa_audio_probe");
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

[[nodiscard]] std::string disable_cause(const std::vector<DecodeCandidate>& candidates) {
    std::string cause = "disabled:";
    bool first = true;
    for (const auto& candidate : candidates) {
        if (candidate.viable) {
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

[[nodiscard]] const char* raw_format_name(std::uint8_t bits) {
    switch (bits) {
    case 16: return "S16LE";
    case 24: return "S24LE";
    case 32: return "S32LE";
    default: return nullptr;
    }
}

} // namespace

struct AudioDecoder::Impl final {
    Config config{};
    DecodeReport decode_report{};

    GstElement* pipeline{nullptr};
    GstElement* appsrc{nullptr};
    GstElement* appsink{nullptr};
    GstBus* bus{nullptr};

    std::vector<std::byte> decoded{};

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
        decoded.clear();
    }

    [[nodiscard]] bool bus_has_error() const {
        if (bus == nullptr) {
            return false;
        }
        GstMessage* message = gst_bus_pop_filtered(bus, GST_MESSAGE_ERROR);
        if (message == nullptr) {
            return false;
        }
        GError* error = nullptr;
        gchar* debug = nullptr;
        gst_message_parse_error(message, &error, &debug);
        if (error != nullptr) {
            g_error_free(error);
        }
        g_free(debug);
        gst_message_unref(message);
        return true;
    }

    // appsrc caps: PCM carries the negotiated source shape; AAC-LC-ADTS is
    // self-describing; raw AAC-LC is completed per chunk with the
    // AudioSpecificConfig from the channel setup (codec_data).
    [[nodiscard]] GstCaps* make_source_caps(std::span<const std::byte> codec_data) const {
        switch (config.codec) {
        case Codec::pcm: {
            const char* format = raw_format_name(config.input_format.bits_per_sample);
            if (format == nullptr) {
                return nullptr;
            }
            return gst_caps_new_simple("audio/x-raw",
                                       "format", G_TYPE_STRING, format,
                                       "rate", G_TYPE_INT,
                                       static_cast<int>(config.input_format.sample_rate_hz),
                                       "channels", G_TYPE_INT,
                                       static_cast<int>(config.input_format.channels),
                                       "layout", G_TYPE_STRING, "interleaved",
                                       nullptr);
        }
        case Codec::aac_lc_adts:
            return gst_caps_new_simple("audio/mpeg",
                                       "mpegversion", G_TYPE_INT, 4,
                                       "stream-format", G_TYPE_STRING, "adts",
                                       nullptr);
        case Codec::aac_lc: {
            GstCaps* caps = gst_caps_new_simple("audio/mpeg",
                                                "mpegversion", G_TYPE_INT, 4,
                                                "stream-format", G_TYPE_STRING, "raw",
                                                nullptr);
            if (!codec_data.empty()) {
                GBytes* bytes = g_bytes_new(codec_data.data(), codec_data.size());
                GstBuffer* asc = gst_buffer_new_wrapped_bytes(bytes);
                gst_caps_set_simple(caps, "codec_data", GST_TYPE_BUFFER, asc, nullptr);
                gst_buffer_unref(asc);
            }
            return caps;
        }
        }
        return nullptr;
    }
};

[[nodiscard]] std::string_view to_string(Codec codec) noexcept {
    switch (codec) {
    case Codec::pcm: return "pcm";
    case Codec::aac_lc: return "aac_lc";
    case Codec::aac_lc_adts: return "aac_lc_adts";
    }
    return "unknown";
}

DecodeReport AudioDecoder::probe(Codec codec) {
    ensure_gstreamer_initialized();
    DecodeReport report;
    report.codec = codec;

    // The normalization chain is required for every codec: decoded output is
    // always re-shaped to the routed 16-bit interleaved format.
    const DecodeCandidate convert = probe_element(kConvertElement);
    const DecodeCandidate resample = probe_element(kResampleElement);
    report.candidates.push_back(convert);
    report.candidates.push_back(resample);

    if (codec != Codec::pcm) {
        report.candidates.push_back(probe_element(kAacParser));
        for (const auto& known : kAacDecoders) {
            report.candidates.push_back(probe_element(known.element));
        }
    }

    const DecodeSelection selection = select_decode_backend(report);
    report.usable = selection.usable;
    report.selected_element = selection.usable ? selection.element : "";
    if (!selection.usable) {
        report.cause = selection.cause;
    }
    return report;
}

DecodeSelection select_decode_backend(const DecodeReport& report) {
    DecodeSelection selection;

    const DecodeCandidate* convert = nullptr;
    const DecodeCandidate* resample = nullptr;
    const DecodeCandidate* parser = nullptr;
    const DecodeCandidate* chosen = nullptr;
    for (const auto& candidate : report.candidates) {
        if (candidate.element == kConvertElement) {
            convert = &candidate;
        } else if (candidate.element == kResampleElement) {
            resample = &candidate;
        } else if (candidate.element == kAacParser) {
            parser = &candidate;
        }
        for (const auto& known : kAacDecoders) {
            if (candidate.element == known.element && candidate.viable && chosen == nullptr) {
                chosen = &candidate;
            }
        }
    }

    const bool chain_ok = convert != nullptr && convert->viable && resample != nullptr
        && resample->viable;
    if (report.codec == Codec::pcm) {
        selection.usable = chain_ok;
        selection.element = selection.usable ? "audioconvert+audioresample" : "";
    } else {
        selection.usable = chain_ok && parser != nullptr && parser->viable && chosen != nullptr;
        if (selection.usable) {
            selection.element = chosen->element;
        }
    }
    if (!selection.usable) {
        selection.cause = disable_cause(report.candidates);
    }
    return selection;
}

core::Result<AudioDecoder> AudioDecoder::open(const Config& config) {
    ensure_gstreamer_initialized();

    if (config.output_format.bits_per_sample != 16 || config.output_format.channels == 0
        || config.output_format.sample_rate_hz == 0) {
        return core::Result<AudioDecoder>{Error{ErrorCode::audio_format_unsupported}};
    }
    if (config.codec == Codec::pcm
        && (config.input_format.bits_per_sample == 0 || config.input_format.channels == 0
            || config.input_format.sample_rate_hz == 0)) {
        return core::Result<AudioDecoder>{Error{ErrorCode::audio_format_unsupported}};
    }

    const DecodeReport report = probe(config.codec);
    if (!report.usable) {
        return core::Result<AudioDecoder>{Error{ErrorCode::audio_format_unsupported}};
    }

    auto impl = std::make_unique<Impl>();
    impl->config = config;
    impl->decode_report = report;

    GstElement* pipeline = gst_pipeline_new("aa_audio_decode");
    GstElement* source = gst_element_factory_make("appsrc", "src");
    GstElement* parser = nullptr;
    GstElement* decoder = nullptr;
    if (config.codec != Codec::pcm) {
        parser = gst_element_factory_make(kAacParser, "parser");
        decoder = gst_element_factory_make(report.selected_element.c_str(), "decoder");
    }
    GstElement* convert = gst_element_factory_make(kConvertElement, "convert");
    GstElement* resample = gst_element_factory_make(kResampleElement, "resample");
    GstElement* sink = gst_element_factory_make("appsink", "sink");

    GstCaps* sink_caps = gst_caps_new_simple("audio/x-raw",
                                             "format", G_TYPE_STRING, "S16LE",
                                             "rate", G_TYPE_INT,
                                             static_cast<int>(config.output_format.sample_rate_hz),
                                             "channels", G_TYPE_INT,
                                             static_cast<int>(config.output_format.channels),
                                             "layout", G_TYPE_STRING, "interleaved",
                                             nullptr);
    GstCaps* source_caps = impl->make_source_caps({});

    const bool complete = pipeline != nullptr && source != nullptr && convert != nullptr
        && resample != nullptr && sink != nullptr && sink_caps != nullptr && source_caps != nullptr
        && (config.codec == Codec::pcm || (parser != nullptr && decoder != nullptr));

    if (!complete) {
        for (GstElement* element : {pipeline, source, parser, decoder, convert, resample, sink}) {
            if (element != nullptr) {
                gst_object_unref(element);
            }
        }
        if (sink_caps != nullptr) {
            gst_caps_unref(sink_caps);
        }
        if (source_caps != nullptr) {
            gst_caps_unref(source_caps);
        }
        return core::Result<AudioDecoder>{Error{ErrorCode::audio_format_unsupported}};
    }

    g_object_set(source,
                 "caps", source_caps,
                 "format", GST_FORMAT_TIME,
                 "is-live", FALSE,
                 "block", TRUE,
                 nullptr);
    gst_caps_unref(source_caps);

    g_object_set(sink,
                 "emit-signals", FALSE,
                 "sync", FALSE,
                 "async", FALSE,
                 "max-buffers", 8,
                 "drop", FALSE,
                 "caps", sink_caps,
                 nullptr);
    gst_caps_unref(sink_caps);

    bool linked = false;
    if (config.codec == Codec::pcm) {
        gst_bin_add_many(GST_BIN(pipeline), source, convert, resample, sink, nullptr);
        linked = gst_element_link_many(source, convert, resample, sink, nullptr);
    } else {
        gst_bin_add_many(GST_BIN(pipeline), source, parser, decoder, convert, resample, sink,
                         nullptr);
        linked = gst_element_link_many(source, parser, decoder, convert, resample, sink, nullptr);
    }
    if (!linked) {
        gst_element_set_state(pipeline, GST_STATE_NULL);
        gst_object_unref(pipeline);
        return core::Result<AudioDecoder>{Error{ErrorCode::audio_format_unsupported}};
    }

    if (gst_element_set_state(pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
        gst_element_set_state(pipeline, GST_STATE_NULL);
        gst_object_unref(pipeline);
        return core::Result<AudioDecoder>{Error{ErrorCode::audio_decode_failed}};
    }

    impl->pipeline = pipeline;
    impl->appsrc = source;
    impl->appsink = sink;
    impl->bus = gst_element_get_bus(pipeline);
    return core::Result<AudioDecoder>{AudioDecoder{std::move(impl)}};
}

AudioDecoder::AudioDecoder(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

AudioDecoder::~AudioDecoder() = default;

AudioDecoder::AudioDecoder(AudioDecoder&& other) noexcept = default;

AudioDecoder& AudioDecoder::operator=(AudioDecoder&& other) noexcept = default;

core::Result<PcmChunk> AudioDecoder::decode(const EncodedChunk& chunk) {
    if (!impl_ || impl_->pipeline == nullptr) {
        return Error{ErrorCode::invalid_argument};
    }
    if (chunk.payload.empty()) {
        return Error{ErrorCode::invalid_argument};
    }
    if (chunk.codec != impl_->config.codec) {
        return Error{ErrorCode::audio_format_unsupported};
    }
    if (impl_->config.codec == Codec::pcm && !(chunk.input_format == impl_->config.input_format)) {
        return Error{ErrorCode::audio_format_unsupported};
    }
    if (impl_->config.codec == Codec::aac_lc) {
        // Raw AAC-LC is only decodable when the AudioSpecificConfig travels
        // with the stream; without it the decode is refused typed.
        if (chunk.codec_data.empty()) {
            impl_->decode_report.cause =
                "disabled: raw AAC-LC requires AudioSpecificConfig (codec_data); "
                "use aac_lc_adts or supply codec_data";
            return Error{ErrorCode::audio_format_unsupported};
        }
        GstCaps* caps = impl_->make_source_caps(chunk.codec_data);
        if (caps == nullptr) {
            return Error{ErrorCode::audio_format_unsupported};
        }
        gst_app_src_set_caps(GST_APP_SRC(impl_->appsrc), caps);
        gst_caps_unref(caps);
    }

    GstBuffer* buffer = gst_buffer_new_allocate(nullptr, chunk.payload.size(), nullptr);
    if (buffer == nullptr) {
        return Error{ErrorCode::audio_decode_failed};
    }
    GstMapInfo map;
    if (!gst_buffer_map(buffer, &map, GST_MAP_WRITE)) {
        gst_buffer_unref(buffer);
        return Error{ErrorCode::audio_decode_failed};
    }
    std::memcpy(map.data, chunk.payload.data(), chunk.payload.size());
    gst_buffer_unmap(buffer, &map);
    GST_BUFFER_PTS(buffer) = static_cast<GstClockTime>(chunk.timestamp.count);

    // One submitted payload is one coded unit; exactly one normalized chunk
    // comes back (documented 1:1 decode contract).
    const GstFlowReturn pushed = gst_app_src_push_buffer(GST_APP_SRC(impl_->appsrc), buffer);
    if (pushed != GST_FLOW_OK) {
        return Error{ErrorCode::audio_decode_failed};
    }

    GstSample* sample = gst_app_sink_try_pull_sample(GST_APP_SINK(impl_->appsink), kDecodeTimeoutNs);
    if (sample == nullptr) {
        if (impl_->bus_has_error()) {
            return Error{ErrorCode::audio_decode_failed};
        }
        return Error{ErrorCode::timeout};
    }

    impl_->decoded.clear();
    for (;;) {
        GstBuffer* out = gst_sample_get_buffer(sample);
        GstMapInfo out_map;
        if (out == nullptr || !gst_buffer_map(out, &out_map, GST_MAP_READ)) {
            gst_sample_unref(sample);
            return Error{ErrorCode::audio_decode_failed};
        }
        const std::size_t offset = impl_->decoded.size();
        impl_->decoded.resize(offset + out_map.size);
        std::memcpy(impl_->decoded.data() + offset, out_map.data, out_map.size);
        gst_buffer_unmap(out, &out_map);
        gst_sample_unref(sample);

        sample = gst_app_sink_try_pull_sample(GST_APP_SINK(impl_->appsink),
                                              impl_->config.codec == Codec::pcm
                                                  ? 0
                                                  : kDrainQuietNs);
        if (sample == nullptr) {
            break;
        }
    }

    PcmChunk result;
    result.role = chunk.role;
    result.format = impl_->config.output_format;
    result.timestamp = chunk.timestamp;
    result.pcm = std::span<const std::byte>{impl_->decoded.data(), impl_->decoded.size()};
    return result;
}

void AudioDecoder::flush() noexcept {
    if (!impl_ || impl_->pipeline == nullptr) {
        return;
    }
    gst_element_send_event(impl_->pipeline, gst_event_new_flush_start());
    gst_element_send_event(impl_->pipeline, gst_event_new_flush_stop(TRUE));
}

void AudioDecoder::close() noexcept {
    if (impl_) {
        impl_->release();
    }
}

[[nodiscard]] const DecodeReport& AudioDecoder::report() const noexcept {
    return impl_->decode_report;
}

} // namespace aa::audio
