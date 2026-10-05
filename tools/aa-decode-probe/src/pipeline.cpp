#include "pipeline.hpp"

#include <chrono>
#include <cmath>
#include <fstream>
#include <iostream>
#include <thread>
#include <vector>

#include <gst/app/gstappsink.h>
#include <gst/app/gstappsrc.h>
#include <gst/gst.h>
#include <gst/video/video.h>

namespace aa::decode_probe {

struct NaluAccessUnit {
    std::vector<uint8_t> data;
    bool is_idr = false;
};

// Split raw annex-B stream into Access Units (framewise)
static std::vector<NaluAccessUnit> parse_annexb_units(const std::vector<uint8_t>& stream) {
    std::vector<NaluAccessUnit> units;
    std::vector<size_t> starts;

    for (size_t i = 0; i + 3 < stream.size(); ++i) {
        if (stream[i] == 0 && stream[i+1] == 0) {
            if (stream[i+2] == 1) {
                starts.push_back(i);
                i += 2;
            } else if (stream[i+2] == 0 && i + 3 < stream.size() && stream[i+3] == 1) {
                starts.push_back(i);
                i += 3;
            }
        }
    }

    if (starts.empty()) return units;

    std::vector<uint8_t> current_au;
    bool current_has_idr = false;
    bool au_has_slice = false;
    bool has_aud = false;

    for (size_t idx = 0; idx < starts.size(); ++idx) {
        size_t start = starts[idx];
        size_t end = (idx + 1 < starts.size()) ? starts[idx + 1] : stream.size();

        size_t nal_offset = start + (stream[start + 2] == 1 ? 3 : 4);
        if (nal_offset >= stream.size()) continue;

        uint8_t nal_type = stream[nal_offset] & 0x1F;
        if (nal_type == 7 && (nal_offset + 1 >= end || stream[nal_offset + 1] != 66)) return {};
        if (nal_type == 9) has_aud = true;
        if ((nal_type == 1 || nal_type == 5) && !has_aud) return {};

        // Keep multi-slice frames together; only explicit non-VCL boundaries split units.
        if (nal_type == 9 || ((nal_type == 7 || nal_type == 8) && au_has_slice)) {
            if (au_has_slice) {
                units.push_back({std::move(current_au), current_has_idr});
                current_au.clear();
                current_has_idr = false;
                au_has_slice = false;
            }
        }

        current_au.insert(current_au.end(), stream.data() + start, stream.data() + end);
        if (nal_type == 5) {
            current_has_idr = true;
            au_has_slice = true;
        } else if (nal_type == 1) {
            au_has_slice = true;
        }
    }

    if (au_has_slice) {
        units.push_back({std::move(current_au), current_has_idr});
    }

    return units;
}

ProbeMetrics run_decode_probe(const ProbeOptions& options, const DiscoveryResult& discovery) {
    ProbeMetrics metrics;
    metrics.decoder_used = discovery.selected_decoder;
    metrics.is_hardware = discovery.is_hardware_selected;

    if (metrics.decoder_used.empty()) {
        metrics.pass = false;
        metrics.failure_reason = "No viable H.264 decoder element found";
        return metrics;
    }

    // Load fixture data
    if (options.fixture_path.empty()) {
        metrics.pass = false;
        metrics.failure_reason = "Fixture path is empty";
        return metrics;
    }

    std::ifstream f(options.fixture_path, std::ios::binary | std::ios::ate);
    if (!f.is_open()) {
        metrics.pass = false;
        metrics.failure_reason = "Cannot open fixture file: " + options.fixture_path;
        return metrics;
    }

    const auto fixture_size = f.tellg();
    if (fixture_size <= 0 || fixture_size > 64LL * 1024 * 1024) {
        metrics.failure_reason = "Fixture must contain 1..67108864 bytes";
        return metrics;
    }
    f.seekg(0);
    std::vector<uint8_t> fixture_data(static_cast<size_t>(fixture_size));
    if (!f.read(reinterpret_cast<char*>(fixture_data.data()), fixture_size)) {
        metrics.failure_reason = "Failed to read complete fixture";
        return metrics;
    }
    f.close();

    auto units = parse_annexb_units(fixture_data);
    if (units.empty()) {
        metrics.pass = false;
        metrics.failure_reason = "Fixture requires AUD-delimited Baseline H.264 access units (no B frames)";
        return metrics;
    }

    // Build GStreamer pipeline:
    // appsrc (byte-stream) -> h264parse -> <decoder> -> appsink
    GstElement* pipeline = gst_pipeline_new("decode_probe_pipeline");
    GstElement* appsrc = gst_element_factory_make("appsrc", "src");
    GstElement* parser = gst_element_factory_make("h264parse", "parser");
    GstElement* decoder = gst_element_factory_make(metrics.decoder_used.c_str(), "decoder");
    GstElement* appsink = gst_element_factory_make("appsink", "sink");

    if (!pipeline || !appsrc || !parser || !decoder || !appsink) {
        metrics.pass = false;
        metrics.failure_reason = "Failed to create GStreamer elements for pipeline";
        if (pipeline) gst_object_unref(pipeline);
        if (appsrc) gst_object_unref(appsrc);
        if (parser) gst_object_unref(parser);
        if (decoder) gst_object_unref(decoder);
        if (appsink) gst_object_unref(appsink);
        return metrics;
    }

    // Configure appsrc
    GstCaps* src_caps = gst_caps_new_simple("video/x-h264",
        "stream-format", G_TYPE_STRING, "byte-stream",
        "alignment", G_TYPE_STRING, "au",
        nullptr);
    g_object_set(appsrc,
        "caps", src_caps,
        "format", GST_FORMAT_TIME,
        "is-live", FALSE,
        "block", FALSE,
        nullptr);
    gst_caps_unref(src_caps);

    // Configure appsink
    g_object_set(appsink,
        "emit-signals", FALSE,
        "sync", FALSE,
        "max-buffers", 10,
        "drop", FALSE,
        nullptr);

    gst_bin_add_many(GST_BIN(pipeline), appsrc, parser, decoder, appsink, nullptr);
    if (!gst_element_link_many(appsrc, parser, decoder, appsink, nullptr)) {
        metrics.pass = false;
        metrics.failure_reason = "Failed to link pipeline elements (appsrc -> h264parse -> " + metrics.decoder_used + " -> appsink)";
        gst_object_unref(pipeline);
        return metrics;
    }

    // Tracking structures
    FrameCorrelation correlation;
    LatencyStats latency_stats;
    CpuSampler cpu_sampler;

    GstStateChangeReturn sret = gst_element_set_state(pipeline, GST_STATE_PLAYING);
    if (sret == GST_STATE_CHANGE_FAILURE) {
        metrics.pass = false;
        metrics.failure_reason = "Failed to set pipeline state to PLAYING";
        gst_element_set_state(pipeline, GST_STATE_NULL);
        gst_object_unref(pipeline);
        return metrics;
    }

    cpu_sampler.start();
    auto start_time = std::chrono::steady_clock::now();
    auto target_duration = std::chrono::seconds(options.duration_s);
    GstBus* bus = gst_element_get_bus(pipeline);
    auto drain_samples = [&]() {
            while (GstSample* sample = gst_app_sink_try_pull_sample(GST_APP_SINK(appsink), 0)) {
                auto recv_time = std::chrono::steady_clock::now();
                metrics.frames_decoded++;

                {
                    GstCaps* caps = gst_sample_get_caps(sample);
                    if (caps) {
                        gchar* caps_str = gst_caps_to_string(caps);
                        metrics.negotiated_caps_str = caps_str;
                        g_free(caps_str);

                        GstStructure* s = gst_caps_get_structure(caps, 0);
                        int width = 0, height = 0;
                        if (gst_structure_get_int(s, "width", &width) &&
                            gst_structure_get_int(s, "height", &height)) {
                            metrics.observed_width = static_cast<uint32_t>(width);
                            metrics.observed_height = static_cast<uint32_t>(height);
                            if (static_cast<uint32_t>(width) == options.width &&
                                static_cast<uint32_t>(height) == options.height) {
                                if (metrics.frames_decoded == 1) metrics.caps_negotiated_correct = true;
                            } else {
                                metrics.caps_negotiated_correct = false;
                                metrics.failure_reason = "Decoded resolution changed or mismatched requested dimensions";
                            }
                        } else {
                            metrics.caps_negotiated_correct = false;
                            metrics.failure_reason = "Decoded sample caps have no resolution";
                        }
                    } else {
                        metrics.caps_negotiated_correct = false;
                        metrics.failure_reason = "Decoded sample has no caps";
                    }
                }

                // Match identity, never the next FIFO item after a drop or reorder.
                GstBuffer* output = gst_sample_get_buffer(sample);
                auto sent = output ? correlation.take(GST_BUFFER_PTS(output)) : std::nullopt;
                if (sent) {
                        auto sent_time = *sent;
                        double lat_ms = std::chrono::duration<double, std::milli>(recv_time - sent_time).count();
                        latency_stats.latencies_ms.push_back(lat_ms);
                } else ++metrics.unmatched_frames;

                gst_sample_unref(sample);
            }
    };
    auto check_error = [&]() {
        GstMessage* message = gst_bus_pop_filtered(bus, GST_MESSAGE_ERROR);
        if (!message) return false;
        GError* error = nullptr;
        gchar* debug = nullptr;
        gst_message_parse_error(message, &error, &debug);
        metrics.failure_reason = error ? error->message : "GStreamer pipeline error";
        if (error) g_error_free(error);
        g_free(debug);
        gst_message_unref(message);
        return true;
    };

    // Paced feeder loop
    size_t au_index = 0;
    uint64_t pts_ns = 0;
    const uint64_t frame_duration_ns = GST_SECOND / options.fps;

    auto next_frame_time = start_time;

    while (true) {
        drain_samples();
        if (check_error()) break;
        auto now = std::chrono::steady_clock::now();
        if (now - start_time >= target_duration) {
            break;
        }
        if (now < next_frame_time) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }
        // Nonblocking appsrc still needs an explicit queue limit: fail rather than
        // silently dropping input or accumulating an unbounded stalled workload.
        if (gst_app_src_get_current_level_bytes(GST_APP_SRC(appsrc)) > 4ULL * 1024 * 1024 ||
            correlation.pending.size() >= 512) {
            metrics.failure_reason = "Pipeline input backlog exceeded bounded limit";
            break;
        }

        const auto& au = units[au_index];
        au_index = (au_index + 1) % units.size();

        GstBuffer* buf = gst_buffer_new_allocate(nullptr, au.data.size(), nullptr);
        if (!buf) {
            metrics.failure_reason = "Failed to allocate input buffer";
            break;
        }
        gst_buffer_fill(buf, 0, au.data.data(), au.data.size());
        GST_BUFFER_PTS(buf) = pts_ns;
        GST_BUFFER_DTS(buf) = pts_ns;
        GST_BUFFER_DURATION(buf) = frame_duration_ns;
        if (!correlation.record(pts_ns, std::chrono::steady_clock::now())) {
            gst_buffer_unref(buf);
            metrics.failure_reason = "Duplicate input PTS";
            break;
        }
        pts_ns += frame_duration_ns;

        GstFlowReturn flow = gst_app_src_push_buffer(GST_APP_SRC(appsrc), buf);
        if (flow != GST_FLOW_OK) {
            metrics.failure_reason = "appsrc push failed: " + std::to_string(flow);
            break;
        }

        metrics.frames_fed++;
        metrics.bytes_fed += au.data.size();

        next_frame_time = start_time + std::chrono::nanoseconds(pts_ns);
    }

    // Send EOS and wait for sink to finish
    auto feed_end = std::chrono::steady_clock::now();
    metrics.feed_duration_s = std::chrono::duration<double>(feed_end - start_time).count();
    if (gst_app_src_end_of_stream(GST_APP_SRC(appsrc)) != GST_FLOW_OK && metrics.failure_reason.empty())
        metrics.failure_reason = "appsrc EOS failed";

    auto eos_start = std::chrono::steady_clock::now();
    while (!gst_app_sink_is_eos(GST_APP_SINK(appsink))) {
        drain_samples();
        if (check_error() || !metrics.failure_reason.empty()) break;
        if (std::chrono::steady_clock::now() - eos_start > std::chrono::seconds(5)) {
            metrics.failure_reason = "EOS drain timed out after 5 seconds";
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    drain_samples();
    check_error();

    auto end_time = std::chrono::steady_clock::now();
    metrics.duration_s = std::chrono::duration<double>(end_time - start_time).count();
    metrics.cpu_percent = cpu_sampler.sample_core_percent();

    latency_stats.calculate();
    metrics.p95_latency_ms = latency_stats.p95_ms;
    metrics.max_latency_ms = latency_stats.max_ms;
    metrics.avg_latency_ms = latency_stats.avg_ms;
    metrics.latency_samples = latency_stats.latencies_ms.size();
    if (metrics.feed_duration_s > 0) {
        metrics.observed_fps = static_cast<double>(metrics.frames_decoded) / metrics.duration_s;
        metrics.observed_bitrate_bps = static_cast<double>(metrics.bytes_fed) * 8.0 / metrics.feed_duration_s;
    }

    if (metrics.frames_fed >= metrics.frames_decoded) {
        metrics.dropped_frames = metrics.frames_fed - metrics.frames_decoded;
    } else {
        metrics.dropped_frames = 0;
    }

    // Clean EOS shutdown
    if (gst_element_set_state(pipeline, GST_STATE_NULL) == GST_STATE_CHANGE_FAILURE && metrics.failure_reason.empty())
        metrics.failure_reason = "Pipeline shutdown failed";
    gst_object_unref(bus);
    gst_object_unref(pipeline);

    if (!metrics.failure_reason.empty()) {
        metrics.pass = false;
    } else if (metrics.frames_decoded == 0 || metrics.unmatched_frames != 0 ||
               metrics.latency_samples != metrics.frames_decoded || !correlation.pending.empty()) {
        metrics.failure_reason = "Missing or unmatched PTS/frame correlation";
    } else if (metrics.dropped_frames != 0) {
        metrics.pass = false;
        metrics.failure_reason = "Dropped frames observed: " + std::to_string(metrics.dropped_frames);
    } else if (metrics.p95_latency_ms > 33.0) {
        metrics.pass = false;
        metrics.failure_reason = "p95 latency exceeded budget: " + std::to_string(metrics.p95_latency_ms) + " ms > 33 ms";
    } else if (metrics.cpu_percent > 50.0) {
        metrics.pass = false;
        metrics.failure_reason = "CPU usage exceeded budget: " + std::to_string(metrics.cpu_percent) + "% > 50%";
    } else if (!metrics.caps_negotiated_correct) {
        metrics.pass = false;
        metrics.failure_reason = "Negotiated caps mismatch: " + metrics.negotiated_caps_str;
    } else {
        metrics.smoke_pass = true;
        metrics.requested_workload_met =
            std::abs(metrics.observed_fps / options.fps - 1.0) <= 0.05 &&
            std::abs(metrics.observed_bitrate_bps / static_cast<double>(options.bitrate_bps) - 1.0) <= 0.05;
        metrics.pass = metrics.requested_workload_met;
        if (!metrics.pass) metrics.failure_reason = "Observed rate/bitrate does not match requested workload (5% tolerance); fixture replay is not qualification";
    }

    return metrics;
}

} // namespace aa::decode_probe
