#include "pipeline.hpp"

#include <chrono>
#include <deque>
#include <fstream>
#include <iostream>
#include <mutex>
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

    for (size_t idx = 0; idx < starts.size(); ++idx) {
        size_t start = starts[idx];
        size_t end = (idx + 1 < starts.size()) ? starts[idx + 1] : stream.size();

        size_t nal_offset = start + (stream[start + 2] == 1 ? 3 : 4);
        if (nal_offset >= stream.size()) continue;

        uint8_t nal_type = stream[nal_offset] & 0x1F;

        // An AU starts before an AUD (type 9), SPS (7), PPS (8), or before a new VCL slice (1 or 5) if we already have one
        if (nal_type == 9 || ((nal_type == 7 || nal_type == 8) && au_has_slice)) {
            if (!current_au.empty()) {
                units.push_back({std::move(current_au), current_has_idr});
                current_au.clear();
                current_has_idr = false;
                au_has_slice = false;
            }
        }

        current_au.insert(current_au.end(), stream.begin() + start, stream.begin() + end);
        if (nal_type == 5) {
            current_has_idr = true;
            au_has_slice = true;
        } else if (nal_type == 1) {
            au_has_slice = true;
        }
    }

    if (!current_au.empty()) {
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

    std::ifstream f(options.fixture_path, std::ios::binary);
    if (!f.is_open()) {
        metrics.pass = false;
        metrics.failure_reason = "Cannot open fixture file: " + options.fixture_path;
        return metrics;
    }

    std::vector<uint8_t> fixture_data((std::istreambuf_iterator<char>(f)),
                                       std::istreambuf_iterator<char>());
    f.close();

    auto units = parse_annexb_units(fixture_data);
    if (units.empty()) {
        metrics.pass = false;
        metrics.failure_reason = "Fixture file contains no valid H.264 access units";
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
        "block", TRUE,
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
    std::mutex queue_mutex;
    std::deque<std::chrono::steady_clock::time_point> input_timestamps;
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
    auto target_duration = std::chrono::milliseconds(options.duration_s * 1000);
    uint32_t frame_interval_us = 1'000'000 / options.fps;

    bool stop_requested = false;
    std::exception_ptr pull_exception = nullptr;

    // Output pull thread
    std::thread sink_thread([&]() {
        try {
            while (!stop_requested) {
                GstSample* sample = gst_app_sink_try_pull_sample(GST_APP_SINK(appsink), 100 * GST_MSECOND);
                if (!sample) {
                    if (gst_app_sink_is_eos(GST_APP_SINK(appsink))) {
                        break;
                    }
                    continue;
                }

                auto recv_time = std::chrono::steady_clock::now();
                metrics.frames_decoded++;

                // Check caps on first frame
                if (metrics.frames_decoded == 1) {
                    GstCaps* caps = gst_sample_get_caps(sample);
                    if (caps) {
                        gchar* caps_str = gst_caps_to_string(caps);
                        metrics.negotiated_caps_str = caps_str;
                        g_free(caps_str);

                        GstStructure* s = gst_caps_get_structure(caps, 0);
                        int width = 0, height = 0;
                        if (gst_structure_get_int(s, "width", &width) &&
                            gst_structure_get_int(s, "height", &height)) {
                            if (static_cast<uint32_t>(width) == options.width &&
                                static_cast<uint32_t>(height) == options.height) {
                                metrics.caps_negotiated_correct = true;
                            }
                        }
                    }
                }

                // Match input timestamp
                {
                    std::lock_guard<std::mutex> lock(queue_mutex);
                    if (!input_timestamps.empty()) {
                        auto sent_time = input_timestamps.front();
                        input_timestamps.pop_front();
                        double lat_ms = std::chrono::duration<double, std::milli>(recv_time - sent_time).count();
                        latency_stats.latencies_ms.push_back(lat_ms);
                    }
                }

                gst_sample_unref(sample);
            }
        } catch (...) {
            pull_exception = std::current_exception();
        }
    });

    // Paced feeder loop
    size_t au_index = 0;
    uint64_t pts_ns = 0;
    const uint64_t frame_duration_ns = GST_SECOND / options.fps;

    auto next_frame_time = start_time;

    while (true) {
        auto now = std::chrono::steady_clock::now();
        if (now - start_time >= target_duration) {
            break;
        }

        const auto& au = units[au_index];
        au_index = (au_index + 1) % units.size();

        GstBuffer* buf = gst_buffer_new_allocate(nullptr, au.data.size(), nullptr);
        gst_buffer_fill(buf, 0, au.data.data(), au.data.size());
        GST_BUFFER_PTS(buf) = pts_ns;
        GST_BUFFER_DTS(buf) = pts_ns;
        GST_BUFFER_DURATION(buf) = frame_duration_ns;
        pts_ns += frame_duration_ns;

        {
            std::lock_guard<std::mutex> lock(queue_mutex);
            input_timestamps.push_back(std::chrono::steady_clock::now());
        }

        GstFlowReturn flow = gst_app_src_push_buffer(GST_APP_SRC(appsrc), buf);
        if (flow != GST_FLOW_OK) {
            std::cerr << "appsrc push failed with code " << flow << std::endl;
            break;
        }

        metrics.frames_fed++;

        // Pacing sleep until next frame slot
        next_frame_time += std::chrono::microseconds(frame_interval_us);
        std::this_thread::sleep_until(next_frame_time);
    }

    // Send EOS and wait for sink to finish
    gst_app_src_end_of_stream(GST_APP_SRC(appsrc));

    auto eos_start = std::chrono::steady_clock::now();
    while (!gst_app_sink_is_eos(GST_APP_SINK(appsink))) {
        if (std::chrono::steady_clock::now() - eos_start > std::chrono::seconds(5)) {
            break; // Timeout
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    stop_requested = true;
    if (sink_thread.joinable()) {
        sink_thread.join();
    }

    auto end_time = std::chrono::steady_clock::now();
    metrics.duration_s = std::chrono::duration<double>(end_time - start_time).count();
    metrics.cpu_percent = cpu_sampler.sample_core_percent();

    latency_stats.calculate();
    metrics.p95_latency_ms = latency_stats.p95_ms;
    metrics.max_latency_ms = latency_stats.max_ms;
    metrics.avg_latency_ms = latency_stats.avg_ms;

    if (metrics.frames_fed >= metrics.frames_decoded) {
        metrics.dropped_frames = metrics.frames_fed - metrics.frames_decoded;
    } else {
        metrics.dropped_frames = 0;
    }

    // Clean EOS shutdown
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(pipeline);

    // Pass criteria:
    // 1. Decoder available
    // 2. Valid fixture frame drops == 0
    // 3. p95 decode latency <= 33 ms
    // 4. CPU <= 50% of one core
    // 5. Caps negotiated matching 1280x720
    if (metrics.dropped_frames != 0) {
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
        metrics.pass = true;
    }

    return metrics;
}

} // namespace aa::decode_probe
