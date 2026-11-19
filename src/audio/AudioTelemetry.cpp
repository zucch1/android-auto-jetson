// SPDX-License-Identifier: GPL-3.0-or-later
#include <aa/audio/AudioTelemetry.hpp>

#include <charconv>
#include <cmath>
#include <string>
#include <system_error>
#include <utility>

namespace aa::audio {
namespace {

std::string optional_ns(std::optional<core::Nanoseconds> value) {
    return value ? std::to_string(value->count) : "null";
}

template <class T>
std::string optional_scalar(std::optional<T> value) {
    return value ? std::to_string(*value) : "null";
}

} // namespace

std::string json_text(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (const char c : text) {
        if (c == '"' || c == '\\') {
            out += '\\';
            out += c;
        } else if (static_cast<unsigned char>(c) < 0x20) {
            out += ' ';
        } else {
            out += c;
        }
    }
    return out;
}

std::string format_ms(core::Nanoseconds value) {
    const double ms = static_cast<double>(value.count) / 1'000'000.0;
    if (!std::isfinite(ms)) {
        return "null";
    }
    char buffer[32];
    const auto [end, error] =
        std::to_chars(buffer, buffer + sizeof(buffer), ms, std::chars_format::general);
    if (error != std::errc{}) {
        return "null";
    }
    return std::string{buffer, end};
}

std::string audio_sink_selected_json(const SinkReport& selected, std::string_view origin) {
    return std::string{"{\"event\":\"audio_sink_selected\",\"origin\":\""} +
        std::string{origin} + "\",\"requested\":\"" +
        std::string{to_string(selected.requested)} + "\",\"element\":\"" +
        json_text(selected.selected_element) + "\",\"cause\":\"" +
        json_text(selected.cause) + "\"}";
}

void log_sink_selected(const SinkReport& selected, std::string_view origin,
                       const AudioTelemetryCallbacks& telemetry) {
    const auto& sink = (origin == "reopen" && telemetry.log_line_nonblocking)
        ? telemetry.log_line_nonblocking
        : telemetry.log_line;
    if (sink) {
        sink(audio_sink_selected_json(selected, origin));
    }
}

std::string audio_write_completed_json(const WriteCompletion& completion) {
    // presented_ns (#60): when the chunk starts playing on the sink's
    // clock; the A/V gate uses it in place of the element handoff. It is
    // ALWAYS present on this playback-time contract: null when the sink
    // reported no slot, so the collector invalidates A/V instead of
    // mixing in a handoff endpoint.
    return "{\"event\":\"audio_write_completed\",\"scheduled_ns\":" +
        std::to_string(completion.scheduled.count) + ",\"scheduled_end_ns\":" +
        std::to_string(completion.scheduled_end.count) + ",\"completed_ns\":" +
        std::to_string(completion.completed.count) + ",\"presented_ns\":" +
        (completion.presented ? std::to_string(completion.presented->count) : std::string{"null"}) +
        ",\"playback_slot_absence\":" +
        (completion.presented
             ? std::string{"null"}
             : "\"" + std::string{to_string(completion.playback_slot_absence)} + "\"") +
        "}";
}

std::string audio_chunks_json(std::uint64_t count) {
    return "{\"event\":\"audio_chunks\",\"count\":" + std::to_string(count) + "}";
}

std::string media_timing_json(const RouterStats& stats) {
    return "{\"event\":\"media_timing\",\"audio_scheduling_skew_ms\":" +
        format_ms(stats.max_completion_skew) + ",\"audio_latency_ms\":" +
        format_ms(stats.max_path_latency) + "}";
}

std::string audio_catch_up_json(std::uint64_t frames, std::uint64_t total, std::uint32_t rate) {
    return "{\"event\":\"audio_catch_up\",\"frames\":" + std::to_string(frames) +
        ",\"frames_total\":" + std::to_string(total) + ",\"rate\":" + std::to_string(rate) + "}";
}

std::string sink_write_json(const SinkWriteProgress& value) {
    return "{\"sequence\":" + std::to_string(value.sequence) +
        ",\"readiness_probe\":" + (value.readiness_probe ? "true" : "false") +
        ",\"effective_scheduled_ns\":" + std::to_string(value.effective_scheduled.count) +
        ",\"entered_at_ns\":" + std::to_string(value.entered_at.count) +
        ",\"deadline_ns\":" + std::to_string(value.deadline.count) +
        ",\"levels_at_ns\":" + (value.levels_valid ? std::to_string(value.levels_at.count) : "null") +
        ",\"queued_bytes\":" + (value.levels_valid ? std::to_string(value.queued_bytes) : "null") +
        ",\"queued_buffers\":" + (value.levels_valid ? std::to_string(value.queued_buffers) : "null") +
        ",\"max_bytes\":" + std::to_string(value.max_bytes) +
        ",\"max_buffers\":" + std::to_string(value.max_buffers) +
        ",\"admitted_at_ns\":" + optional_ns(value.admitted_at) +
        ",\"pair_pushed_at_ns\":" + optional_ns(value.pair_pushed_at) +
        ",\"ingress_at_ns\":" + optional_ns(value.ingress_at) +
        ",\"marker_at_ns\":" + optional_ns(value.marker_at) +
        ",\"handoff_at_ns\":" + optional_ns(value.handoff_at) +
        ",\"completed_at_ns\":" + optional_ns(value.completed_at) +
        ",\"error\":" + (value.outcome ? "\"" + std::string{aa::to_string(*value.outcome)} + "\"" : "null") +
        ",\"pts_ns\":" + optional_scalar(value.pts_ns) +
        ",\"pts_origin_ns\":" + optional_scalar(value.pts_origin_ns) +
        ",\"clock_ns\":" + optional_scalar(value.clock_ns) +
        ",\"base_time_ns\":" + optional_scalar(value.base_time_ns) +
        ",\"running_time_ns\":" + optional_scalar(value.running_time_ns) + "}";
}

std::string audio_dispatch_json(const AudioDispatchObservation& value) {
    return "{\"role\":" + std::to_string(static_cast<unsigned>(value.delivery.role)) +
        ",\"generation\":" + std::to_string(value.delivery.generation) +
        ",\"delivery_sequence\":" + std::to_string(value.delivery.sequence) +
        ",\"session_id\":" + std::to_string(value.delivery.session_id) +
        ",\"message_sequence\":" + std::to_string(value.message_sequence) +
        ",\"original_scheduled_ns\":" + std::to_string(value.original_scheduled.count) +
        ",\"received_at_ns\":" + std::to_string(value.received_at.count) +
        ",\"dequeued_at_ns\":" + optional_ns(value.dequeued_at) +
        ",\"write\":" + (value.write ? sink_write_json(*value.write) : "null") + "}";
}

std::string render_failure_json(const RenderFailureSnapshot& value) {
    using Reason = RenderFailureReason;
    const auto reason = [&]() -> const char* {
        switch (value.reason) {
        case Reason::closed: return "closed";
        case Reason::degraded: return "degraded";
        case Reason::flushed: return "flushed";
        case Reason::missing_sink: return "missing_sink";
        case Reason::zero_quantum: return "zero_quantum";
        case Reason::cursor_range: return "cursor_range";
        case Reason::quantum_end_overflow: return "quantum_end_overflow";
        case Reason::intended_time_conversion: return "intended_time_conversion";
        case Reason::late: return "late";
        case Reason::placement_time_conversion: return "placement_time_conversion";
        case Reason::parent_end_overflow: return "parent_end_overflow";
        case Reason::parent_end_time_conversion: return "parent_end_time_conversion";
        case Reason::single_gap: return "single_gap";
        case Reason::aggregate_gap: return "aggregate_gap";
        case Reason::output_start_time_conversion: return "output_start_time_conversion";
        case Reason::output_end_time_conversion: return "output_end_time_conversion";
        case Reason::mixer_error: return "mixer_error";
        case Reason::sink_write_error: return "sink_write_error";
        }
        return "unknown";
    }();
    std::string json = "{\"reason\":\"" + std::string{reason} + "\",\"underlying_code\":" +
        (value.underlying_code ? "\"" + std::string{aa::to_string(*value.underlying_code)} + "\"" : "null") +
        ",\"output_sample_rate\":" + std::to_string(value.output_sample_rate) +
        ",\"base_committed\":" + (value.base_committed ? "true" : "false") +
        ",\"entry_cursor\":" + std::to_string(value.entry_cursor);
    const auto field = [&](const char* name, const auto& operand) {
        json += ",\"" + std::string{name} + "\":" + optional_scalar(operand);
    };
    field("base_ns", value.base_ns);
    field("quantum", value.quantum);
    field("cap", value.cap);
    field("intended", value.intended);
    field("role_end", value.role_end);
    field("start", value.start);
    field("remaining", value.remaining);
    field("time_conversion_frame", value.time_conversion_frame);
    field("covered_end", value.covered_end);
    field("raw_gap", value.raw_gap);
    field("uncovered_gap", value.uncovered_gap);
    field("total_gap_before", value.total_gap_before);
    field("now_ns", value.now_ns);
    field("prefix_end", value.prefix_end);
    field("stored_idle_prefix", value.stored_idle_prefix);
    field("output_start", value.output_start);
    field("output_end", value.output_end);
    json += ",\"preflighted\":" + std::string{value.preflighted ? (*value.preflighted ? "true" : "false") : "null"};
    json += ",\"offending\":";
    if (value.offending) {
        json += "{\"role\":" + std::to_string(static_cast<unsigned>(value.offending->channel)) +
            ",\"sequence\":" + std::to_string(value.offending->sequence) +
            ",\"original_timestamp_ns\":" + std::to_string(value.offending->timestamp.count) + "}";
    } else {
        json += "null";
    }
    const auto ranges = [&](const char* name, const RenderRangeSummary& range) {
        json += ",\"" + std::string{name} + "\":{\"count\":" + std::to_string(range.count) +
            ",\"start_ns\":" + optional_scalar(range.start_ns) +
            ",\"end_ns\":" + optional_scalar(range.end_ns) +
            ",\"first_frame\":" + optional_scalar(range.first_frame) +
            ",\"last_frame\":" + optional_scalar(range.last_frame) +
            ",\"excluded_frames\":" + std::to_string(range.excluded_frames) + "}";
    };
    ranges("stored_backpressure", value.stored_backpressure);
    ranges("stored_idle", value.stored_idle);
    if (value.effective_backpressure) {
        ranges("effective_backpressure", *value.effective_backpressure);
    } else {
        json += ",\"effective_backpressure\":null";
    }
    if (value.effective_idle) {
        ranges("effective_idle", *value.effective_idle);
    } else {
        json += ",\"effective_idle\":null";
    }
    return json + "}";
}

std::string role_counts_json(const std::array<std::size_t, 4>& counts) {
    return "{\"media\":" + std::to_string(counts[0]) + ",\"guidance\":" + std::to_string(counts[1]) +
        ",\"system\":" + std::to_string(counts[2]) + ",\"speech\":" + std::to_string(counts[3]) + "}";
}

std::string worker_queue_json(const WorkerQueueObservation& value) {
    return "{\"queued_by_role\":" + role_counts_json(value.queued_by_role) +
        ",\"in_flight\":" + (value.in_flight ? "true" : "false") +
        ",\"output_established\":" + (value.output_established ? "true" : "false") + "}";
}

std::string worker_completion_json(const std::optional<WriteCompletion>& value) {
    if (!value) { return "null"; }
    return "{\"scheduled_ns\":" + std::to_string(value->scheduled.count) +
        ",\"scheduled_end_ns\":" + std::to_string(value->scheduled_end.count) +
        ",\"completed_ns\":" + std::to_string(value->completed.count) + "}";
}

std::string worker_success_json(const std::optional<WorkerSuccessfulStep>& value) {
    if (!value) { return "null"; }
    return "{\"returned_at_ns\":" + std::to_string(value->returned_at.count) +
        ",\"reconciled_at_ns\":" + std::to_string(value->reconciled_at.count) +
        ",\"pending\":" + (value->pending ? "true" : "false") +
        ",\"contributors\":" + std::to_string(value->contributors) +
        ",\"completed\":" + std::to_string(value->completed) +
        ",\"queue\":" + worker_queue_json(value->queue) + "}";
}

std::string worker_admission_json(const std::optional<WorkerAdmission>& value) {
    if (!value) { return "null"; }
    const auto disposition = [&] {
        switch (value->disposition) {
        case IdleDisposition::absent: return "absent";
        case IdleDisposition::receipt_not_after_start: return "receipt_not_after_start";
        case IdleDisposition::short_interval: return "short";
        case IdleDisposition::authorized: return "authorized";
        }
        return "unknown";
    }();
    return "{\"parent\":" + audio_dispatch_json(value->parent) +
        ",\"accepted_at_ns\":" + std::to_string(value->accepted_at.count) +
        ",\"idle_since_ns\":" + optional_ns(value->idle_since) +
        ",\"disposition\":\"" + disposition + "\",\"queue\":" + worker_queue_json(value->queue) +
        ",\"last_completion\":" + worker_completion_json(value->last_completion) +
        ",\"last_success\":" + worker_success_json(value->last_success) + "}";
}

std::string worker_failure_json(const WorkerFailureObservation& value) {
    std::string idle = "null";
    if (value.last_idle) {
        idle = "{\"observed_at_ns\":" + std::to_string(value.last_idle->observed_at.count) +
            ",\"idle_since_ns\":" + optional_ns(value.last_idle->idle_since) +
            ",\"eligible\":" + (value.last_idle->eligible ? "true" : "false") +
            ",\"queue\":" + worker_queue_json(value.last_idle->queue) + "}";
    }
    return "{\"queue\":" + worker_queue_json(value.queue) +
        ",\"admission\":" + worker_admission_json(value.admission) +
        ",\"last_idle_close\":" + worker_admission_json(value.last_idle_close) +
        ",\"last_completion\":" + worker_completion_json(value.last_completion) +
        ",\"last_success\":" + worker_success_json(value.last_success) +
        ",\"last_idle\":" + idle + "}";
}

std::string audio_failure_json(const FailureContext& value) {
    const auto origin = [&]() -> const char* {
        switch (value.origin) {
        case AudioFailureOrigin::open: return "open";
        case AudioFailureOrigin::router_submit: return "router_submit";
        case AudioFailureOrigin::render_step: return "render_step";
        case AudioFailureOrigin::flush: return "flush";
        case AudioFailureOrigin::reopen_ack_timeout: return "reopen_ack_timeout";
        }
        return "unknown";
    }();
    return "{\"origin\":\"" + std::string{origin} + "\",\"latched_at_ns\":" +
        std::to_string(value.latched_at.count) + ",\"parent\":" +
        (value.parent ? audio_dispatch_json(*value.parent) : "null") + ",\"router\":" +
        (value.router ? render_failure_json(*value.router) : "null") +
        ",\"worker\":" + worker_failure_json(value.worker) + "}";
}

std::string audio_failure_event_json(const char* detail, ErrorCode code,
                                     const std::optional<FailureContext>& context) {
    return "{\"event\":\"err\",\"detail\":\"" + std::string{detail} + "\",\"code\":\"" +
        std::string{aa::to_string(code)} + "\"" +
        (context ? ",\"audio_failure\":" + audio_failure_json(*context) : "") + "}";
}

std::string audio_rejection_json(const AudioRejectionSnapshot& value) {
    return "{\"rejected_at_ns\":" + std::to_string(value.rejected_at.count) +
        ",\"rejected\":" + audio_dispatch_json(value.rejected) +
        ",\"active\":" + (value.active ? audio_dispatch_json(*value.active) : "null") +
        ",\"oldest_queued_receipt_ns\":" + optional_ns(value.oldest_queued_receipt) +
        ",\"occupied_by_role\":" + role_counts_json(value.occupied_by_role) +
        ",\"pending_completions_by_role\":" + role_counts_json(value.pending_completions_by_role) + "}";
}

RouterConfig pcm_router_config(AudioTelemetryCallbacks telemetry, SinkBackend backend) {
    RouterConfig config;
    config.backend = backend;
    config.output_format = AudioFormat{48000, 2, 16};
    for (auto& role : config.roles) {
        role.codec = Codec::pcm;
        role.input_format = AudioFormat{16000, 1, 16};
    }
    config.roles[role_index(Role::media)].input_format = AudioFormat{48000, 2, 16};
    // Live-PCM drift policy: the only production enable point. The phone's
    // media clock disagrees with the output clock (~100-200 ppm); without
    // bounded head trims the preserved backlog grows without bound.
    config.live_pcm_drift.enabled = true;
    // #155 catch-up seam: once LivePcmDriftPolicy::catch_up_watermark lands on main,
    // set `config.live_pcm_drift.catch_up_watermark = core::Nanoseconds{90'000'000};` here.
    config.on_write_completed = [log_media = std::move(telemetry.log_media)](const WriteCompletion& completion) {
        if (log_media) {
            log_media("audio_write_completed", audio_write_completed_json(completion));
        }
    };
    return config;
}

RouterConfig pcm_router_config(SinkBackend backend, AudioTelemetryCallbacks telemetry) {
    return pcm_router_config(std::move(telemetry), backend);
}

} // namespace aa::audio
