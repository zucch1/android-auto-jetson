// SPDX-License-Identifier: GPL-3.0-or-later
//
// Wired end-to-end receiver entry point (task 30 phase-B harness driver): the
// composed receiver for the real approved phone over USB/AOA. It wires the
// task-28 USB/AOA link (open_wired_link: admit_via(TrustStore)) into the
// task-30 integrator (aa::app::WiredSession) and serves the task-22 video
// socket, the task-24 input socket, the task-25 audio router and the task-21
// D-Bus control contract that the dummy display (tools/dummy-display)
// registers against. It implements no module internals.
//
// TEST/QA TOOLING: this drives the live qualification round (tools/wired-qa);
// the production service packaging is todo 41. Pairing is deliberately NOT
// opened here: the approved-phones store must be pre-loaded (fail closed, task
// 27 semantics); only ForgetPhone mutates it (mid-session revoke).
//
// Threading: one SESSION thread runs every transport interaction (the
// Transport contract: the session thread is the only caller of
// open/send/receive/close). Display input events are queued by the input
// thread and drained by the session thread between receives (input is
// low-rate; during projection a media frame bounds the drain latency). The
// main thread owns the D-Bus dispatch loop, video-consumer accept and the run
// budget. The session thread is the only caller of WiredSession methods.

#include <aa/app/WiredLink.hpp>
#include <aa/app/WiredPhoneControl.hpp>
#include <aa/app/WiredSession.hpp>
#include <aa/audio/AudioRouter.hpp>
#include <aa/channels/SensorSink.hpp>
#include <aa/channels/MicrophoneSink.hpp>
#include <aa/channels/InputSink.hpp>
#include <aa/diagnostics/Diagnostics.hpp>
#include <aa/ipc/ControlBusService.hpp>
#include <aa/ipc/ControlService.hpp>
#include <aa/ipc/InputChannel.hpp>
#include <aa/ipc/InputSocket.hpp>
#include <aa/ipc/PairingGate.hpp>
#include <aa/ipc/VideoChannel.hpp>
#include <aa/ipc/VideoSocket.hpp>
#include <aa/protocol/Messages.hpp>
#include <aa/protocol/Protocol.hpp>
#include <aa/session/Events.hpp>
#include <aa/session/Session.hpp>
#include <aa/transport/TlsTransport.hpp>
#include <aa/trust/Store.hpp>

#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <system_error>
#include <thread>
#include <variant>
#include <vector>

#include <signal.h>
#include "VideoOutlet.hpp"
#include "StdoutLog.hpp"
#include "AvCreditJson.hpp"

namespace {

using aa::core::Milliseconds;
using aa::wired_receiver::VideoOutlet;

std::atomic<bool> g_running{true};

void log_line(const std::string &json) {
    auto *writer = aa::wired_receiver::active_stdout_log.load();
    if (!writer) { std::abort(); }
    if (writer->submit(json)) { g_running.store(false); }
}

void on_signal(int) { g_running.store(false); }

struct AudioDispatchObservation final {
    aa::channels::AvDeliveryId delivery;
    std::uint64_t message_sequence{};
    aa::core::Nanoseconds original_scheduled{};
    aa::core::Nanoseconds received_at{};
    std::optional<aa::core::Nanoseconds> dequeued_at{};
    std::optional<aa::audio::SinkWriteProgress> write{};
    [[nodiscard, maybe_unused]] friend bool operator==(const AudioDispatchObservation&,
                                        const AudioDispatchObservation&) = default;
};

struct AudioRejectionSnapshot final {
    aa::diagnostics::QueueRejection queue;
    aa::core::Nanoseconds rejected_at{};
    AudioDispatchObservation rejected;
    std::optional<AudioDispatchObservation> active{};
    std::optional<aa::core::Nanoseconds> oldest_queued_receipt{};
    std::array<std::size_t, 4> occupied_by_role{};
    std::array<std::size_t, 4> pending_completions_by_role{};
    [[nodiscard, maybe_unused]] friend bool operator==(const AudioRejectionSnapshot&,
                                        const AudioRejectionSnapshot&) = default;
};

enum class AudioFailureOrigin : std::uint8_t { open, router_submit, render_step, flush };

enum class IdleDisposition : std::uint8_t { absent, receipt_not_after_start, short_interval, authorized };

struct WorkerQueueObservation final {
    std::array<std::size_t, 4> queued_by_role{};
    bool in_flight{};
    bool output_established{};
};

struct WorkerSuccessfulStep final {
    aa::core::Nanoseconds returned_at{};
    aa::core::Nanoseconds reconciled_at{};
    bool pending{};
    std::size_t contributors{};
    std::size_t completed{};
    WorkerQueueObservation queue{};
};

struct WorkerAdmission final {
    AudioDispatchObservation parent;
    aa::core::Nanoseconds accepted_at{}; // Real monotonic, unlike the injectable receipt/start clock.
    std::optional<aa::core::Nanoseconds> idle_since{};
    IdleDisposition disposition{};
    WorkerQueueObservation queue{};
    std::optional<aa::audio::WriteCompletion> last_completion{};
    std::optional<WorkerSuccessfulStep> last_success{};
};

struct WorkerIdleObservation final {
    aa::core::Nanoseconds observed_at{}; // Real monotonic observation time.
    std::optional<aa::core::Nanoseconds> idle_since{};
    bool eligible{};
    WorkerQueueObservation queue{};
};

struct WorkerFailureObservation final {
    WorkerQueueObservation queue{};
    std::optional<WorkerAdmission> admission{};
    std::optional<WorkerAdmission> last_idle_close{};
    std::optional<aa::audio::WriteCompletion> last_completion{};
    std::optional<WorkerSuccessfulStep> last_success{};
    std::optional<WorkerIdleObservation> last_idle{};
};

struct FailureContext final {
    AudioFailureOrigin origin{};
    aa::core::Nanoseconds latched_at{};
    std::optional<AudioDispatchObservation> parent{};
    std::optional<aa::audio::RenderFailureSnapshot> router{};
    WorkerFailureObservation worker{};
};

std::string optional_ns(std::optional<aa::core::Nanoseconds> value) {
    return value ? std::to_string(value->count) : "null";
}

template <class T> std::string optional_scalar(std::optional<T> value) {
    return value ? std::to_string(*value) : "null";
}

std::string sink_write_json(const aa::audio::SinkWriteProgress &value) {
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

std::string audio_dispatch_json(const AudioDispatchObservation &value) {
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

std::string render_failure_json(const aa::audio::RenderFailureSnapshot &value) {
    using Reason = aa::audio::RenderFailureReason;
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
    } else { json += "null"; }
    const auto ranges = [&](const char* name, const aa::audio::RenderRangeSummary& range) {
        json += ",\"" + std::string{name} + "\":{\"count\":" + std::to_string(range.count) +
            ",\"start_ns\":" + optional_scalar(range.start_ns) +
            ",\"end_ns\":" + optional_scalar(range.end_ns) +
            ",\"first_frame\":" + optional_scalar(range.first_frame) +
            ",\"last_frame\":" + optional_scalar(range.last_frame) +
            ",\"excluded_frames\":" + std::to_string(range.excluded_frames) + "}";
    };
    ranges("stored_backpressure", value.stored_backpressure);
    ranges("stored_idle", value.stored_idle);
    if (value.effective_backpressure) { ranges("effective_backpressure", *value.effective_backpressure); }
    else { json += ",\"effective_backpressure\":null"; }
    if (value.effective_idle) { ranges("effective_idle", *value.effective_idle); }
    else { json += ",\"effective_idle\":null"; }
    return json + "}";
}

std::string role_counts_json(const std::array<std::size_t, 4> &counts);

std::string worker_queue_json(const WorkerQueueObservation &value) {
    return "{\"queued_by_role\":" + role_counts_json(value.queued_by_role) +
        ",\"in_flight\":" + (value.in_flight ? "true" : "false") +
        ",\"output_established\":" + (value.output_established ? "true" : "false") + "}";
}

std::string worker_completion_json(const std::optional<aa::audio::WriteCompletion> &value) {
    if (!value) { return "null"; }
    return "{\"scheduled_ns\":" + std::to_string(value->scheduled.count) +
        ",\"scheduled_end_ns\":" + std::to_string(value->scheduled_end.count) +
        ",\"completed_ns\":" + std::to_string(value->completed.count) + "}";
}

std::string worker_success_json(const std::optional<WorkerSuccessfulStep> &value) {
    if (!value) { return "null"; }
    return "{\"returned_at_ns\":" + std::to_string(value->returned_at.count) +
        ",\"reconciled_at_ns\":" + std::to_string(value->reconciled_at.count) +
        ",\"pending\":" + (value->pending ? "true" : "false") +
        ",\"contributors\":" + std::to_string(value->contributors) +
        ",\"completed\":" + std::to_string(value->completed) +
        ",\"queue\":" + worker_queue_json(value->queue) + "}";
}

std::string worker_admission_json(const std::optional<WorkerAdmission> &value) {
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

std::string worker_failure_json(const WorkerFailureObservation &value) {
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

std::string audio_failure_json(const FailureContext &value) {
    const auto origin = [&]() -> const char* {
        switch (value.origin) {
        case AudioFailureOrigin::open: return "open";
        case AudioFailureOrigin::router_submit: return "router_submit";
        case AudioFailureOrigin::render_step: return "render_step";
        case AudioFailureOrigin::flush: return "flush";
        }
        return "unknown";
    }();
    return "{\"origin\":\"" + std::string{origin} + "\",\"latched_at_ns\":" +
        std::to_string(value.latched_at.count) + ",\"parent\":" +
        (value.parent ? audio_dispatch_json(*value.parent) : "null") + ",\"router\":" +
        (value.router ? render_failure_json(*value.router) : "null") +
        ",\"worker\":" + worker_failure_json(value.worker) + "}";
}

std::string audio_failure_event_json(const char* detail, aa::ErrorCode code,
                                   const std::optional<FailureContext>& context) {
    return "{\"event\":\"err\",\"detail\":\"" + std::string{detail} + "\",\"code\":\"" +
        std::string{aa::to_string(code)} + "\"" +
        (context ? ",\"audio_failure\":" + audio_failure_json(*context) : "") + "}";
}

std::string role_counts_json(const std::array<std::size_t, 4> &counts) {
    return "{\"media\":" + std::to_string(counts[0]) + ",\"guidance\":" + std::to_string(counts[1]) +
        ",\"system\":" + std::to_string(counts[2]) + ",\"speech\":" + std::to_string(counts[3]) + "}";
}

std::string audio_rejection_json(const AudioRejectionSnapshot &value) {
    return "{\"rejected_at_ns\":" + std::to_string(value.rejected_at.count) +
        ",\"rejected\":" + audio_dispatch_json(value.rejected) +
        ",\"active\":" + (value.active ? audio_dispatch_json(*value.active) : "null") +
        ",\"oldest_queued_receipt_ns\":" + optional_ns(value.oldest_queued_receipt) +
        ",\"occupied_by_role\":" + role_counts_json(value.occupied_by_role) +
        ",\"pending_completions_by_role\":" + role_counts_json(value.pending_completions_by_role) + "}";
}

std::string poll_error_json(aa::ErrorCode code,
                            std::optional<aa::app::WiredSession::RejectedFrame> reject,
                            std::optional<aa::diagnostics::QueueRejection> queue = {},
                            std::optional<AudioRejectionSnapshot> audio = {}) {
    std::string line = std::string{"{\"event\":\"err\",\"detail\":\"poll\",\"code\":\""} +
                       std::string{aa::to_string(code)} + "\"";
    if (code == aa::ErrorCode::channel_not_registered && reject) {
        line += ",\"reject_channel\":" + std::to_string(reject->channel);
        if (reject->msg_id) {
            line += ",\"reject_msg_id\":" + std::to_string(*reject->msg_id);
        }
    }
    if (code == aa::ErrorCode::ipc_queue_full && queue) {
        line += ",\"queue_rejection\":{\"source\":\"" +
                std::string{aa::diagnostics::to_string(queue->source)} + "\",\"reason\":\"" +
                std::string{aa::diagnostics::to_string(queue->reason)} + "\",\"role\":" +
                std::to_string(static_cast<unsigned>(queue->role)) +
                ",\"items\":" + std::to_string(queue->items) +
                ",\"capacity\":" + std::to_string(queue->capacity) +
                ",\"bytes\":" + std::to_string(queue->bytes) +
                ",\"byte_capacity\":" + std::to_string(queue->byte_capacity) +
                ",\"units\":" + std::to_string(queue->units) +
                ",\"unit_capacity\":" + std::to_string(queue->unit_capacity) +
                ",\"requested_bytes\":" + std::to_string(queue->requested_bytes) +
                ",\"requested_units\":" + std::to_string(queue->requested_units);
        if (queue->source == aa::diagnostics::QueueSource::audio_worker) {
            line += ",\"requested_items\":" + std::to_string(queue->requested_items) +
                    ",\"remaining_permits\":" + std::to_string(queue->remaining_permits) +
                    ",\"reserved_bytes\":" + std::to_string(queue->reserved_bytes) +
                    ",\"reserved_units\":" + std::to_string(queue->reserved_units);
        }
        line += "}";
        if (audio && audio->queue == *queue) {
            line += ",\"audio_rejection\":" + audio_rejection_json(*audio);
        }
    }
    return line + "}";
}

// Media-timing diagnostics: nanoseconds -> milliseconds as an exact rational
// (ns / 1e6) carried in a double, no lossy pre-rounding - the collector and
// reporter compare the value as-is against the ms budgets. Non-finite input
// (not producible from an int64 count) degrades to a JSON null, which the
// collector treats as an absent metric (BLOCKED), never 0.
std::string format_ms(aa::core::Nanoseconds value) {
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

class StdoutDiagnostics final : public aa::diagnostics::DiagnosticSink {
  public:
    void emit_json(std::string_view redacted_json) override {
        log_line(std::string{"{\"event\":\"diag\","} + std::string{redacted_json}.substr(1));
    }
};

class StorePhones final : public aa::ipc::PhoneDirectory {
  public:
    explicit StorePhones(aa::app::WiredPhoneControl &control) : control_(control) {}

    [[nodiscard]] aa::core::Result<void> forget(aa::core::PhoneId phone) override {
        return control_.forget(phone);
    }
    std::optional<aa::helper::RevocationV1> take_revocation_v1() override {
        return control_.take_revocation_v1();
    }
    [[nodiscard]] aa::core::Result<void> pairing_requested(aa::core::PairingRequestId) override {
        return aa::Error{aa::ErrorCode::invalid_argument};
    }
    [[nodiscard]] aa::core::Result<aa::core::PhoneId>
    pairing_confirmed(aa::core::PairingRequestId) override {
        return aa::Error{aa::ErrorCode::invalid_argument};
    }
    void pairing_cancelled(aa::core::PairingRequestId) override {}
    [[nodiscard]] std::vector<aa::core::PairingRequestId> reap_expired() override { return {}; }
    [[nodiscard]] std::vector<aa::core::PairingRequestId> take_invalidated_pairings() override {
        return {};
    }

  private:
    aa::app::WiredPhoneControl &control_;
};

class ConcreteAudioRenderWorker final {
  public:
    using Clock = aa::core::Deadline::Clock;
    using IdleClock = std::function<aa::core::Nanoseconds()>;
    using OpenRouter = std::function<aa::core::Result<aa::audio::AudioRouter>(
        const aa::audio::RouterConfig &)>;
#ifdef AA_WIRED_RECEIVER_SHUTDOWN_TEST
    std::function<void(const std::array<std::size_t, 4> &, bool, bool,
                       const aa::audio::RouterReport &)> on_idle_observed_for_test;
#endif
    explicit ConcreteAudioRenderWorker(aa::audio::RouterConfig config,
                                       OpenRouter open = aa::audio::AudioRouter::open,
                                       IdleClock idle_clock = monotonic_now)
        : idle_clock_(std::move(idle_clock)),
          thread_([this, config = std::move(config), open = std::move(open)] {
              run(config, open);
          }) {}
    ~ConcreteAudioRenderWorker() { (void)finish(); }
    ConcreteAudioRenderWorker(const ConcreteAudioRenderWorker &) = delete;
    ConcreteAudioRenderWorker &operator=(const ConcreteAudioRenderWorker &) = delete;

    [[nodiscard]] aa::core::Result<void> ready() {
        std::unique_lock lock(mutex_);
        cv_.wait(lock, [&] { return initialized_; });
        if (failure_) { return *failure_; }
        return {};
    }

    [[nodiscard]] aa::core::Result<void>
    submit(const aa::protocol::MessageView &message, aa::channels::AvDeliveryId id) {
        const auto receiver_input_at = idle_clock_();
        SettlementDiagnostics diagnostics;
        std::unique_lock lock(mutex_);
        if (failure_) {
            last_queue_rejection_ = failure_queue_rejection_;
            return *failure_;
        }
        if (stopping_) { return aa::Error{aa::ErrorCode::cancelled}; }
        last_queue_rejection_.reset();
        const auto role = audio_index(message.header.channel);
        if (!role || *role >= kAdvertisedRoles || id.role != message.header.channel || id.generation != generations_[*role]) {
            return aa::Error{aa::ErrorCode::invalid_argument};
        }
        const auto bytes = message.payload.size();
        const auto reject = [&](aa::diagnostics::QueueReason reason, std::size_t units) {
            last_queue_rejection_ = aa::diagnostics::QueueRejection{
                aa::diagnostics::QueueSource::audio_worker, reason, message.header.channel,
                role_slots_[*role], kRoleSlots, role_bytes_[*role], capacity(*role).bytes,
                role_units_[*role], capacity(*role).units, bytes, units, 1,
                credits_[*role].empty() ? 0 : credits_[*role].size() - 1};
            last_queue_rejection_->reserved_bytes =
                last_queue_rejection_->remaining_permits * envelope(*role).bytes;
            last_queue_rejection_->reserved_units =
                last_queue_rejection_->remaining_permits * envelope(*role).units;
            AudioRejectionSnapshot snapshot{
                *last_queue_rejection_, monotonic_now(),
                {id, message.header.sequence, message.header.timestamp, receiver_input_at}, active_};
            for (const auto &job : queued_) {
                ++snapshot.occupied_by_role[*audio_index(job.id.role)];
                if (!snapshot.oldest_queued_receipt || job.receiver_input_at < *snapshot.oldest_queued_receipt) {
                    snapshot.oldest_queued_receipt = job.receiver_input_at;
                }
            }
            for (const auto &completion : completed_) {
                ++snapshot.pending_completions_by_role[*audio_index(completion.role)];
            }
            frozen_rejection_ = snapshot;
            aa::channels::AvObservation observation{};
            observation.event = aa::channels::AvCreditEvent::rejected;
            observation.delivery = id;
            observation.session_active = true;
            observation.message_sequence = message.header.sequence;
            observation.bytes = bytes;
            observation.error = aa::ErrorCode::ipc_queue_full;
            record_credit_locked(observation);
            return aa::Error{aa::ErrorCode::ipc_queue_full};
        };
        if (bytes > envelope(*role).bytes) {
            return aa::Error{aa::ErrorCode::protocol_payload_oversize};
        }
        // Units are input bytes at 192000 B/s; voice costs 6x. Round UP to
        // whole input frames, so malformed short frames never under-reserve.
        const std::size_t frame_bytes = *role == 0 ? 4 : 2;
        const auto units = ((bytes + frame_bytes - 1) / frame_bytes) * frame_bytes *
                           (*role == 0 ? 1 : 6);
        const auto permits = credits_[*role].empty() ? 0 : credits_[*role].size() - 1;
        const auto promised = envelope(*role);
        if (role_slots_[*role] + permits >= kRoleSlots ||
            role_bytes_[*role] + permits * promised.bytes + bytes > capacity(*role).bytes ||
            role_units_[*role] + permits * promised.units + units > capacity(*role).units) {
            return reject(role_slots_[*role] + permits >= kRoleSlots ? aa::diagnostics::QueueReason::slots
                : role_bytes_[*role] + permits * promised.bytes + bytes > capacity(*role).bytes ? aa::diagnostics::QueueReason::bytes
                : aa::diagnostics::QueueReason::weighted_units, units);
        }
        const WorkerAdmission admission{{id, message.header.sequence, message.header.timestamp, receiver_input_at},
            monotonic_now(), idle_since_, idle_disposition(receiver_input_at), queue_observation(),
            last_completion_, last_success_};
        queued_.push_back(Job{message.header,
                               {message.payload.begin(), message.payload.end()}, id,
                               receiver_input_at, {bytes, units}, {}, admission});
        ++slots_;
        ++role_slots_[*role];
        bytes_ += bytes;
        units_ += units;
        role_bytes_[*role] += bytes;
        role_units_[*role] += units;
        // This arrival consumed the role's oldest outstanding credit.
        if (!credits_[*role].empty()) { credits_[*role].pop_front(); }
        // Each parent makes exactly one ACK decision. A credit reserves the
        // FULL supported next envelope, independently of this parent's size.
        deferred_[*role].push_back(DeferredCredit{id, envelope(*role), monotonic_now(), false,
                                                message.header.sequence, bytes});
        close_idle(receiver_input_at, diagnostics);
        if (admission.idle_since) { last_idle_close_ = admission; }
        grant_deferred(diagnostics);
        cv_.notify_all();
        lock.unlock();
        diagnostics.publish();
        return {};
    }

    void invalidate(aa::protocol::ChannelRole role, std::uint64_t generation) {
        SettlementDiagnostics diagnostics;
        std::unique_lock lock(mutex_);
        const auto index = audio_index(role);
        if (!index) { return; }
        if (generations_[*index] == generation) { return; }
        if (router_) { router_->cancel_pending(role); }
        credits_[*index].clear();
        generations_[*index] = generation;
        const auto stale = [&](const auto &entry) {
            return entry.id.role == role && entry.id.generation != generation;
        };
        discard_if(queued_, stale);
        std::erase_if(completed_, [&](const aa::channels::AvDeliveryId &entry) {
            return entry.role == role && entry.generation != generation;
        });
        std::erase_if(deferred_[*index], [&](const DeferredCredit &entry) {
            return entry.id.generation != generation;
        });
        observe_idle(diagnostics);
        grant_deferred(diagnostics);
        cv_.notify_all();
        lock.unlock();
        diagnostics.publish();
    }

    [[nodiscard]] std::optional<aa::channels::AvDeliveryId>
    take_completion(Clock::time_point until = Clock::now()) {
        std::unique_lock lock(mutex_);
        cv_.wait_until(lock, until, [&] { return !completed_.empty() || failure_ || stopped_; });
        if (completed_.empty()) { return std::nullopt; }
        const auto completion = completed_.front();
        record_credit_locked(credit_event(aa::channels::AvCreditEvent::taken, completion));
        completed_.pop_front();
        return completion;
    }

    [[nodiscard]] std::optional<aa::Error> failure() const {
        std::lock_guard lock(mutex_);
        return failure_;
    }
    [[nodiscard]] std::optional<FailureContext> failure_context() const {
        std::lock_guard lock(mutex_);
        return failure_context_;
    }
#ifdef AA_WIRED_RECEIVER_SHUTDOWN_TEST
    [[nodiscard]] WorkerFailureObservation worker_observation_for_test() const {
        std::lock_guard lock(mutex_);
        auto observation = worker_observation();
        if (!queued_.empty()) { observation.admission = queued_.back().admission; }
        return observation;
    }
#endif
    [[nodiscard]] std::optional<aa::diagnostics::QueueRejection> last_queue_rejection() const {
        std::lock_guard lock(mutex_);
        return last_queue_rejection_;
    }
    [[nodiscard]] std::optional<aa::diagnostics::QueueRejection> failure_queue_rejection() const {
        std::lock_guard lock(mutex_);
        return failure_queue_rejection_;
    }
    [[nodiscard]] std::optional<AudioRejectionSnapshot> rejection_snapshot() const {
        std::lock_guard lock(mutex_);
        return frozen_rejection_;
    }
    // Retention is forensic; a later accepted submit must retire live attachment.
    [[nodiscard]] std::optional<AudioRejectionSnapshot> current_rejection_snapshot() const {
        std::lock_guard lock(mutex_);
        if (frozen_rejection_ && last_queue_rejection_ &&
            frozen_rejection_->queue == *last_queue_rejection_) {
            return frozen_rejection_;
        }
        return std::nullopt;
    }
    [[nodiscard]] bool outstanding() const {
        std::lock_guard lock(mutex_);
        return slots_ != 0 || in_flight_ || failure_.has_value();
    }
    void observe_av(const aa::channels::AvObservation& value) {
        std::lock_guard lock(mutex_);
        record_credit_locked(value);
    }
    [[nodiscard]] std::optional<aa::wired_receiver::AvCreditSnapshot> credit_trace() const {
        std::lock_guard lock(mutex_);
        return credit_ledger_.frozen();
    }
#ifdef AA_WIRED_RECEIVER_SHUTDOWN_TEST
    [[nodiscard]] aa::wired_receiver::AvCreditSnapshot credit_observation_for_test() const {
        std::lock_guard lock(mutex_);
        return credit_ledger_.snapshot();
    }
#endif
    // Blocks until every admitted reservation is released (each job rendered
    // or deliberately discarded) or the deadline passes. Telemetry may still be
    // publishing; callers needing observer completion must synchronize separately.
    [[nodiscard]] bool wait_idle(Clock::time_point until) {
        std::unique_lock lock(mutex_);
        return cv_.wait_until(lock, until, [&] { return slots_ == 0 && !in_flight_; });
    }
    void stop() {
        std::lock_guard lock(mutex_);
        stopping_ = true;
        idle_since_.reset();
        idle_resume_id_.reset();
        cancel_all();
        discard_if(queued_, [](const auto &) { return true; });
        completed_.clear();
        credits_ = {};
        deferred_ = {};
        cv_.notify_all();
    }
    [[nodiscard]] aa::core::Result<void> finish() {
        stop();
        if (thread_.joinable()) { thread_.join(); }
        if (const auto error = failure()) { return *error; }
        return {};
    }
    [[nodiscard]] std::optional<aa::audio::RouterReport> final_report() const {
        std::lock_guard lock(mutex_);
        return final_report_;
    }

  private:
    aa::channels::AvObservation credit_event(aa::channels::AvCreditEvent event,
                                             aa::channels::AvDeliveryId id) const {
        auto value = credit_ledger_.delivery_observation(id);
        value.event = event;
        value.delivery = id;
        value.session_active = true;
        value.pending = id;
        value.at_ns = 0;
        value.error.reset();
        value.count = 0;
        return value;
    }
    void record_credit_locked(const aa::channels::AvObservation& value) {
        if (credit_ledger_.frozen()) { return; }
        aa::wired_receiver::AvCreditState state{};
        for (std::size_t i = 0; i < state.roles.size(); ++i) {
            state.roles[i] = {role_slots_[i], role_bytes_[i], role_units_[i], credits_[i].size(),
                deferred_[i].size(), 0, kRoleSlots, capacity(i).bytes, capacity(i).units, envelope(i).bytes};
            for (const auto& pending : deferred_[i]) {
                if (value.pending == pending.id) { state.pending_deferred = true; }
            }
        }
        for (const auto& completion : completed_) {
            if (const auto role = aa::wired_receiver::AvCreditLedger::role_index(completion.role)) {
                ++state.roles[*role].completions;
            }
            if (value.pending == completion) { state.pending_available = true; }
        }
        credit_ledger_.record(value, state);
    }
    struct Reservation { std::size_t bytes; std::size_t units; };
    struct Job {
        aa::protocol::MessageHeader header;
        std::vector<std::byte> pcm;
        aa::channels::AvDeliveryId id;
        aa::core::Nanoseconds receiver_input_at;
        Reservation reservation;
        std::optional<aa::core::Nanoseconds> dequeued_at{};
        WorkerAdmission admission;
    };
    struct DeferredCredit {
        aa::channels::AvDeliveryId id;
        Reservation next;
        aa::core::Nanoseconds since;
        bool waited = false;
        std::uint64_t message_sequence{};
        std::size_t bytes{};
    };
    static constexpr std::size_t kAdvertisedRoles = 3;
    static constexpr std::size_t kRoleSlots = 4;
    static constexpr Reservation envelope(std::size_t index) {
        return index == 0 ? Reservation{19200, 19200} : Reservation{6400, 38400};
    }
    static constexpr Reservation capacity(std::size_t index) {
        return index == 0 ? Reservation{38400, 38400} : Reservation{6400, 38400};
    }
    static constexpr std::size_t kMaxSlots = kAdvertisedRoles * kRoleSlots;
    // Each call owns its batch: no shared publication queue or lock-held logging.
    // At most kMaxSlots parents can contribute or receive a deferred credit in
    // one reconciliation. Idle completion retires its ID on the first capture.
    struct SettlementDiagnostics {
        struct Credit { aa::channels::AvDeliveryId id; std::int64_t duration; };
        struct IdleSkip { std::uint64_t id; std::uint64_t frames; std::uint64_t total; };
        struct IdleResume { std::uint64_t id; aa::core::Nanoseconds start; aa::core::Nanoseconds end; };
        struct SinkStall { aa::core::Nanoseconds start; aa::core::Nanoseconds end; };
        std::array<Credit, kMaxSlots> credits{};
        std::size_t credit_count{};
        std::array<AudioDispatchObservation, kMaxSlots> correlated{};
        std::size_t correlated_count{};
        std::optional<aa::audio::WriteCompletion> completion;
        std::optional<SinkStall> stall;
        std::optional<IdleSkip> skipped;
        std::optional<IdleResume> resumed;
        void publish() const {
            if (stall) {
                log_line("{\"event\":\"audio_sink_stall\",\"blocked_from_ns\":" +
                    std::to_string(stall->start.count) + ",\"completed_ns\":" +
                    std::to_string(stall->end.count) + ",\"stall_ns\":" +
                    std::to_string(stall->end.count - stall->start.count) + "}");
            }
            if (resumed) {
                log_line("{\"event\":\"audio_idle_resume\",\"id\":" + std::to_string(resumed->id) +
                    ",\"start_ns\":" + std::to_string(resumed->start.count) + ",\"end_ns\":" +
                    std::to_string(resumed->end.count) + ",\"idle_ns\":" +
                    std::to_string(resumed->end.count - resumed->start.count) + ",\"reason\":\"all_role_idle\"}");
            }
            if (skipped) {
                log_line("{\"event\":\"audio_idle_skipped\",\"id\":" + std::to_string(skipped->id) +
                    ",\"frames_idle_skipped\":" + std::to_string(skipped->frames) +
                    ",\"frames_idle_skipped_total\":" + std::to_string(skipped->total) + "}");
            }
            for (std::size_t i = 0; i < correlated_count; ++i) {
                log_line("{\"event\":\"audio_write_correlated\",\"scheduled_ns\":" +
                    std::to_string(completion->scheduled.count) + ",\"scheduled_end_ns\":" +
                    std::to_string(completion->scheduled_end.count) + ",\"completed_ns\":" +
                    std::to_string(completion->completed.count) + ",\"dispatch\":" +
                    audio_dispatch_json(correlated[i]) + "}");
            }
            for (std::size_t i = 0; i < credit_count; ++i) {
                log_line("{\"event\":\"audio_credit_deferred\",\"role\":" +
                    std::to_string(static_cast<unsigned>(credits[i].id.role)) + ",\"sequence\":" +
                    std::to_string(credits[i].id.sequence) + ",\"deferred_ns\":" +
                    std::to_string(credits[i].duration) + "}");
            }
        }
    };
    static aa::core::Nanoseconds monotonic_now() {
        return aa::core::Nanoseconds{std::chrono::duration_cast<std::chrono::nanoseconds>(
            Clock::now().time_since_epoch()).count()};
    }
    [[nodiscard]] WorkerQueueObservation queue_observation() const {
        WorkerQueueObservation value{{}, in_flight_, output_established_};
        for (const auto &job : queued_) { ++value.queued_by_role[*audio_index(job.id.role)]; }
        return value;
    }
    [[nodiscard]] WorkerFailureObservation worker_observation() const {
        return {queue_observation(), {}, last_idle_close_, last_completion_, last_success_, last_idle_};
    }
    [[nodiscard]] IdleDisposition idle_disposition(aa::core::Nanoseconds receipt) const {
        if (!idle_since_) { return IdleDisposition::absent; }
        if (receipt <= *idle_since_) { return IdleDisposition::receipt_not_after_start; }
        if (receipt.count - idle_since_->count <= 200'000'000) { return IdleDisposition::short_interval; }
        return IdleDisposition::authorized;
    }
    void observe_idle(SettlementDiagnostics &diagnostics) {
        const bool eligible = !stopping_ && !failure_ && output_established_ && queued_.empty() && !in_flight_;
        if (eligible) {
            router_->retire_all_role_idle();
            complete_idle_resume(diagnostics);
            if (!idle_since_) { idle_since_ = idle_clock_(); }
        }
        last_idle_ = WorkerIdleObservation{monotonic_now(), idle_since_, eligible, queue_observation()};
    }
    void complete_idle_resume(SettlementDiagnostics &diagnostics) {
        if (idle_resume_id_ && idle_resume_frames_ != 0) {
            diagnostics.skipped = SettlementDiagnostics::IdleSkip{
                *idle_resume_id_, idle_resume_frames_, router_->report().stats.frames_idle_skipped};
        }
        idle_resume_id_.reset();
        idle_resume_frames_ = 0;
    }
    void close_idle(aa::core::Nanoseconds received_at, SettlementDiagnostics &diagnostics) {
        const auto start = idle_since_;
        idle_since_.reset();
        if (!start || received_at <= *start || received_at.count - start->count <= 200'000'000) {
            return;
        }
        router_->note_all_role_idle(*start, received_at);
        idle_resume_id_ = ++idle_sequence_;
        idle_resume_frames_ = 0;
        diagnostics.resumed = SettlementDiagnostics::IdleResume{*idle_resume_id_, *start, received_at};
    }
    static std::optional<std::size_t> audio_index(aa::protocol::ChannelRole role) {
        using aa::protocol::ChannelRole;
        switch (role) {
        case ChannelRole::media_audio: return 0;
        case ChannelRole::guidance_audio: return 1;
        case ChannelRole::system_audio: return 2;
        case ChannelRole::speech_audio: return 3;
        default: return std::nullopt;
        }
    }
    void release(const Job &job) {
        --slots_;
        --role_slots_[*audio_index(job.id.role)];
        bytes_ -= job.reservation.bytes;
        units_ -= job.reservation.units;
        role_bytes_[*audio_index(job.id.role)] -= job.reservation.bytes;
        role_units_[*audio_index(job.id.role)] -= job.reservation.units;
    }
    // A + P + E must fit every per-role cap. Other roles cannot consume this
    // reservation, including the independent first-arrival allowance.
    [[nodiscard]] bool can_credit(std::size_t index, Reservation next) const {
        const auto permits = credits_[index].size() + 1;
        return role_slots_[index] + permits <= kRoleSlots &&
               role_bytes_[index] + permits * next.bytes <= capacity(index).bytes &&
               role_units_[index] + permits * next.units <= capacity(index).units;
    }
    void grant_deferred(SettlementDiagnostics &diagnostics) {
        for (std::size_t index = 0; index < deferred_.size(); ++index) {
            auto &pending = deferred_[index];
            while (!pending.empty() && can_credit(index, pending.front().next)) {
                const auto granted = pending.front();
                pending.pop_front();
                credits_[index].push_back(granted.next);
                completed_.push_back(granted.id);
                auto observation = credit_event(aa::channels::AvCreditEvent::granted, granted.id);
                observation.message_sequence = granted.message_sequence;
                observation.bytes = granted.bytes;
                record_credit_locked(observation);
                if (!granted.waited) { continue; }
                const auto now = monotonic_now();
                if (router_) { router_->note_backpressure(granted.since, now); }
                diagnostics.credits.at(diagnostics.credit_count++) = {granted.id, now.count - granted.since.count};
            }
            for (auto &entry : pending) {
                if (!entry.waited) {
                    auto observation = credit_event(aa::channels::AvCreditEvent::deferred, entry.id);
                    observation.message_sequence = entry.message_sequence;
                    observation.bytes = entry.bytes;
                    record_credit_locked(observation);
                }
                entry.waited = true;
            }
        }
    }
    template <class Queue, class Predicate>
    void discard_if(Queue &queue, Predicate predicate) {
        for (auto it = queue.begin(); it != queue.end();) {
            if (predicate(*it)) {
                release(*it);
                it = queue.erase(it);
            } else { ++it; }
        }
    }
    void latch(aa::Error error, AudioFailureOrigin origin,
               std::optional<aa::diagnostics::QueueRejection> rejection = {},
               std::optional<AudioDispatchObservation> parent = {},
               std::optional<aa::audio::RenderFailureSnapshot> snapshot = {},
               std::optional<WorkerAdmission> admission = {}) {
        if (!failure_) {
            auto worker = worker_observation();
            worker.admission = admission;
            failure_context_ = FailureContext{origin, monotonic_now(), parent, snapshot, worker};
            failure_ = error;
            failure_queue_rejection_ = rejection;
        }
        stopping_ = true;
        idle_since_.reset();
        idle_resume_id_.reset();
        cancel_all();
        discard_if(queued_, [](const auto &) { return true; });
        credits_ = {};
        deferred_ = {};
        cv_.notify_all();
    }
    void cancel_all() {
        if (!router_) { return; }
        for (const auto role : {aa::protocol::ChannelRole::media_audio,
                               aa::protocol::ChannelRole::guidance_audio,
                               aa::protocol::ChannelRole::system_audio}) {
            router_->cancel_pending(role);
        }
    }
    void run(aa::audio::RouterConfig config, const OpenRouter &open) {
        config.on_sink_write_progress = [this, observe = config.on_sink_write_progress](const auto &value) {
            {
                std::lock_guard lock(mutex_);
                if (!value.readiness_probe) { step_write_ = value; }
            }
            if (observe) { observe(value); }
        };
        const auto completed = config.on_write_completed;
        config.on_write_completed = [this](const auto &value) {
            std::lock_guard lock(mutex_);
            last_completion_ = value;
            step_completion_ = value;
        };
        auto opened = open(config);
        {
            std::lock_guard lock(mutex_);
            initialized_ = true;
            if (!opened) { latch(opened.error(), AudioFailureOrigin::open); }
            else if (opened.value().state() == aa::audio::RouterState::degraded) {
                latch(aa::Error{aa::ErrorCode::audio_sink_unavailable}, AudioFailureOrigin::open);
            }
            cv_.notify_all();
        }
        if (!opened) { return; }
        auto router_ = std::move(opened).value();
        {
            std::lock_guard lock(mutex_);
            this->router_ = &router_;
        }
        const auto emit_media_timing = [](const auto &stats) {
            if (stats.frames_written == 0) { return; }
            log_line(
                "{\"event\":\"media_timing\",\"audio_scheduling_skew_ms\":" + format_ms(stats.max_completion_skew) +
                ",\"audio_latency_ms\":" + format_ms(stats.max_path_latency) + "}");
        };
        std::uint64_t chunks = 0;
        for (;;) {
            std::unique_lock lock(mutex_);
            cv_.wait(lock, [&] { return stopping_ || !queued_.empty(); });
            if (stopping_) { break; }
            struct Parent final {
                AudioDispatchObservation dispatch;
                aa::protocol::MessageHeader header;
                WorkerAdmission admission;
            };
            std::vector<Parent> parents;
            for (auto &job : queued_) {
                if (!job.dequeued_at) {
                    job.dequeued_at = monotonic_now();
                    const auto submitted = router_.submit({job.header, job.pcm}, job.receiver_input_at);
                    if (!submitted) {
                        const AudioDispatchObservation parent{job.id, job.header.sequence,
                            job.header.timestamp, job.receiver_input_at, job.dequeued_at};
                        latch(submitted.error(), AudioFailureOrigin::router_submit,
                            router_.report().queue_rejection, parent, {}, job.admission);
                        break;
                    }
                    // Decoded storage now belongs to the router; retain only
                    // the parent's identity and logical occupancy charge.
                    std::vector<std::byte>{}.swap(job.pcm);
                }
                parents.push_back({{job.id, job.header.sequence, job.header.timestamp,
                    job.receiver_input_at, job.dequeued_at}, job.header, job.admission});
            }
            if (stopping_) { break; }
            active_ = parents.size() == 1 ? std::optional{parents.front().dispatch} : std::nullopt;
            step_write_.reset();
            step_completion_.reset();
            in_flight_ = true;
            const auto before = router_.report().stats.frames_written;
            const auto idle_before = router_.report().stats.frames_idle_skipped;
            lock.unlock();
            const auto step_started = monotonic_now();
            const auto result = router_.render_step();
            const auto returned_at = monotonic_now();
            SettlementDiagnostics diagnostics;
            lock.lock();
            diagnostics.completion = step_completion_;
            if (step_completion_) {
                const auto &completion = *step_completion_;
                const auto piece_duration = completion.scheduled_end.count - completion.scheduled.count;
                const auto elapsed = completion.completed.count - step_started.count;
                if (elapsed > piece_duration) {
                    // Exclude ordinary piece pacing, even on a stale schedule.
                    // elapsed > duration bounds this addition below completed.
                    const auto blocked_from = std::max(completion.scheduled_end,
                        aa::core::Nanoseconds{step_started.count + piece_duration});
                    if (completion.completed > blocked_from) {
                        router_.note_backpressure(blocked_from, completion.completed);
                        diagnostics.stall = SettlementDiagnostics::SinkStall{blocked_from, completion.completed};
                    }
                }
            }
            in_flight_ = false;
            const auto stats = router_.report().stats;
            if (idle_resume_id_) {
                idle_resume_frames_ += stats.frames_idle_skipped - idle_before;
                if (result && !result.value().contributors.empty()) { complete_idle_resume(diagnostics); }
            }
            if (result) {
                for (const auto &header : result.value().contributors) {
                    const auto parent = std::find_if(parents.begin(), parents.end(), [&](const auto &entry) {
                        return entry.dispatch.delivery.role == header.channel && entry.dispatch.message_sequence == header.sequence;
                    });
                    if (parent != parents.end() && step_completion_ && frozen_rejection_) {
                        parent->dispatch.write = step_write_;
                        diagnostics.correlated.at(diagnostics.correlated_count++) = parent->dispatch;
                    }
                }
                for (const auto &header : result.value().completed) {
                    discard_if(queued_, [&](const Job &job) {
                        return job.header.channel == header.channel && job.header.sequence == header.sequence &&
                            std::any_of(parents.begin(), parents.end(), [&](const auto &entry) {
                                return entry.dispatch.delivery == job.id;
                            });
                    });
                }
            }
            if (!stopping_) { grant_deferred(diagnostics); }
            active_.reset();
            if (!result) {
                const auto snapshot = router_.report().render_failure;
                std::optional<AudioDispatchObservation> parent;
                std::optional<WorkerAdmission> admission;
                std::size_t matches = 0;
                if (snapshot && snapshot->offending) {
                    const auto& header = *snapshot->offending;
                    for (const auto& entry : parents) {
                        if (entry.header.channel == header.channel && entry.header.sequence == header.sequence &&
                            entry.header.timestamp == header.timestamp) {
                            parent = entry.dispatch;
                            admission = entry.admission;
                            ++matches;
                        }
                    }
                }
                if (matches != 1) { parent.reset(); admission.reset(); }
                latch(result.error(), AudioFailureOrigin::render_step,
                    router_.report().queue_rejection, parent, snapshot, admission);
            } else {
                output_established_ = output_established_ || stats.frames_written != 0;
                last_success_ = WorkerSuccessfulStep{returned_at, monotonic_now(), result.value().pending,
                    result.value().contributors.size(), result.value().completed.size(), queue_observation()};
                observe_idle(diagnostics);
#ifdef AA_WIRED_RECEIVER_SHUTDOWN_TEST
                if (on_idle_observed_for_test) {
                    on_idle_observed_for_test(role_slots_, in_flight_, idle_since_.has_value(), router_.report());
                }
#endif
            }
            cv_.notify_all();
            lock.unlock();
            if (diagnostics.completion && completed) { completed(*diagnostics.completion); }
            diagnostics.publish();
            if (result && stats.frames_written > before) {
                ++chunks;
                if (chunks % 50 == 1) {
                    log_line("{\"event\":\"audio_chunks\",\"count\":" + std::to_string(chunks) + "}");
                    emit_media_timing(stats);
                }
            }
        }
        {
            std::lock_guard lock(mutex_);
            this->router_ = nullptr;
        }
        // Failed PCM must never be pumped again by flush(). close() only tears
        // down/drains the backend; healthy terminalization commits one EOS.
        if (!failure()) {
            if (auto flushed = router_.flush(); !flushed) {
                std::lock_guard lock(mutex_);
                latch(flushed.error(), AudioFailureOrigin::flush);
            }
        }
        router_.close();
        emit_media_timing(router_.report().stats);
        {
            std::lock_guard lock(mutex_);
            this->router_ = nullptr;
            final_report_ = router_.report();
            stopped_ = true;
            cv_.notify_all();
        }
    }

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<Job> queued_;
    std::deque<aa::channels::AvDeliveryId> completed_;
    std::array<std::uint64_t, 4> generations_{};
    std::size_t slots_{};
    std::size_t bytes_{};
    std::size_t units_{};
    std::array<std::size_t, 4> role_slots_{};
    std::array<std::size_t, 4> role_bytes_{};
    std::array<std::size_t, 4> role_units_{};
    std::array<std::deque<Reservation>, 4> credits_{};
    std::array<std::deque<DeferredCredit>, 4> deferred_{};
    std::optional<aa::Error> failure_;
    std::optional<FailureContext> failure_context_;
    std::optional<aa::diagnostics::QueueRejection> last_queue_rejection_;
    std::optional<aa::diagnostics::QueueRejection> failure_queue_rejection_;
    std::optional<AudioDispatchObservation> active_;
    std::optional<aa::audio::SinkWriteProgress> step_write_;
    std::optional<aa::audio::WriteCompletion> step_completion_;
    std::optional<aa::audio::WriteCompletion> last_completion_;
    std::optional<WorkerSuccessfulStep> last_success_;
    std::optional<WorkerAdmission> last_idle_close_;
    std::optional<WorkerIdleObservation> last_idle_;
    aa::audio::AudioRouter *router_{}; // mutex-protected, outlives concurrent cancellation
    bool in_flight_{}; // one detached <=render_quantum output, separate from parent occupancy
    std::optional<AudioRejectionSnapshot> frozen_rejection_;
    aa::wired_receiver::AvCreditLedger credit_ledger_;
    std::optional<aa::audio::RouterReport> final_report_;
    bool initialized_{};
    bool stopping_{};
    bool stopped_{};
    const IdleClock idle_clock_;
    bool output_established_{};
    std::optional<aa::core::Nanoseconds> idle_since_;
    std::optional<std::uint64_t> idle_resume_id_;
    std::uint64_t idle_resume_frames_{};
    std::uint64_t idle_sequence_{};
    std::thread thread_;
};

std::string poll_error_json(aa::ErrorCode code, const aa::app::WiredSession &session,
                            const ConcreteAudioRenderWorker &audio) {
    const auto failure = audio.failure();
    const auto rejection = failure && failure->code() == aa::ErrorCode::ipc_queue_full
        ? audio.failure_queue_rejection() : session.last_queue_rejection();
    auto line = poll_error_json(code, session.last_reject(), rejection, audio.current_rejection_snapshot());
    if (code == aa::ErrorCode::ipc_queue_full && rejection) {
        if (const auto trace = audio.credit_trace()) {
            line.pop_back();
            line += ",\"av_credit_trace\":" + aa::wired_receiver::av_credit_json(*trace) + "}";
        }
    }
    return line;
}

class InputQueue final {
  public:
    void push(const aa::ipc::InputEvent &event) {
        std::lock_guard lock(mutex_);
        if (queue_.size() < kMaxQueued) {
            queue_.push_back(event);
        }
    }
    std::optional<aa::ipc::InputEvent> pop() {
        std::lock_guard lock(mutex_);
        if (queue_.empty()) {
            return std::nullopt;
        }
        auto event = queue_.front();
        queue_.pop_front();
        return event;
    }

  private:
    static constexpr std::size_t kMaxQueued = 64;
    std::mutex mutex_{};
    std::deque<aa::ipc::InputEvent> queue_{};
};

aa::audio::RouterConfig pcm_router_config() {
    aa::audio::RouterConfig config;
    config.output_format = aa::audio::AudioFormat{48000, 2, 16};
    for (auto &role : config.roles) {
        role.codec = aa::audio::Codec::pcm;
        role.input_format = aa::audio::AudioFormat{16000, 1, 16};
    }
    config.roles[aa::audio::role_index(aa::audio::Role::media)].input_format =
        aa::audio::AudioFormat{48000, 2, 16};
    // Live-PCM drift policy: the only production enable point. The phone's
    // media clock disagrees with the output clock (~100-200 ppm); without
    // bounded head trims the preserved backlog grows without bound.
    config.live_pcm_drift.enabled = true;
    config.on_write_completed = [](const aa::audio::WriteCompletion &completion) {
        log_line("{\"event\":\"audio_write_completed\",\"scheduled_ns\":" +
                 std::to_string(completion.scheduled.count) + ",\"scheduled_end_ns\":" +
                 std::to_string(completion.scheduled_end.count) + ",\"completed_ns\":" +
                 std::to_string(completion.completed.count) + "}");
    };
    return config;
}

std::vector<aa::protocol::ServiceDescriptor>
service_descriptors(const aa::input::CapabilityDescriptor &input) {
    const aa::protocol::VideoConfiguration video{{{1280, 720, 30}, {800, 480, 30}}};
    const aa::protocol::AudioConfiguration media_audio{{{48000, 16, 2}}};
    const aa::protocol::AudioConfiguration voice_audio{{{16000, 16, 1}}};
    aa::protocol::SensorConfiguration sensors;
    if (input.night_mode() == aa::input::NightModePolicy::manual) {
        sensors.kinds.push_back(aa::protocol::SensorKind::night_mode);
    }
    for (const auto kind : input.vehicle_sensors()) {
        // The capability vocabulary follows the pinned sensor kinds from
        // location (1) to toll card (22); unsupported kinds fail validation.
        sensors.kinds.push_back(static_cast<aa::protocol::SensorKind>(
            static_cast<std::int32_t>(kind) + 1));
    }
    return {
        {aa::protocol::ServiceKey{99}, aa::protocol::ChannelRole::video, video},
        {aa::protocol::ServiceKey{100}, aa::protocol::ChannelRole::media_audio, media_audio},
        {aa::protocol::ServiceKey{101}, aa::protocol::ChannelRole::guidance_audio, voice_audio},
        {aa::protocol::ServiceKey{102}, aa::protocol::ChannelRole::system_audio, voice_audio},
        {aa::protocol::ServiceKey{104}, aa::protocol::ChannelRole::input,
         aa::protocol::ButtonConfiguration{input.keycodes()}},
        {aa::protocol::ServiceKey{105}, aa::protocol::ChannelRole::sensors, sensors},
        {aa::protocol::ServiceKey{106}, aa::protocol::ChannelRole::microphone,
         aa::protocol::MicrophoneConfiguration{{16000, 16, 1}}},
    };
}

aa::core::Result<void> register_av_sinks(aa::app::WiredSession &session,
                                        VideoOutlet &video, ConcreteAudioRenderWorker &audio) {
    using aa::protocol::ChannelRole;
    if (auto result = session.register_av_sink(ChannelRole::video, video); !result) {
        return result.error();
    }
    for (const auto role : {ChannelRole::media_audio, ChannelRole::guidance_audio,
                            ChannelRole::system_audio}) {
        if (auto result = session.register_deferred_av_sink(role, {
                [&audio](const auto &message, auto id) { return audio.submit(message, id); },
                 [&audio](auto invalidated_role, auto generation) {
                     audio.invalidate(invalidated_role, generation);
                 }, [&audio] { return audio.last_queue_rejection(); },
                 [&audio](const auto& value) { audio.observe_av(value); }}); !result) { return result.error(); }
    }
    return {};
}

aa::core::Result<void> service_audio_completions(aa::app::WiredSession &session,
                                                ConcreteAudioRenderWorker &audio) {
    for (;;) {
        if (const auto error = audio.failure()) {
            (void)session.fail(*error);
            return *error;
        }
        const auto completion = audio.take_completion();
        if (!completion) { return {}; }
        if (const auto error = audio.failure()) {
            (void)session.fail(*error);
            return *error;
        }
        if (auto delivered = session.complete_av_delivery(*completion, {}); !delivered) {
            return delivered.error();
        }
    }
}

aa::core::Result<bool> receive_with_audio(aa::app::WiredSession &session,
                                         ConcreteAudioRenderWorker &audio) {
    const bool had_audio = audio.outstanding();
    if (auto serviced = service_audio_completions(session, audio); !serviced) {
        return serviced.error();
    }
    auto received = (had_audio || audio.outstanding())
        ? session.poll_transport(aa::core::Deadline::Clock::now() + std::chrono::milliseconds{5})
        : session.poll_transport();
    if (auto serviced = service_audio_completions(session, audio); !serviced) {
        return serviced.error();
    }
    return received;
}

const char *const kUsage =
    "usage: aa-wired-receiver --port <usb-port-context> [--store PATH]\n"
    "                         [--video-socket PATH] [--input-socket PATH]\n"
    "                         [--duration-ms N] [--no-bus]\n"
    "                         [--require-video-consumer]\n"
    "  runs the wired session over USB/AOA with TrustStore admission;\n"
    "  the approved-phones store must already contain the phone (fail closed).\n"
    "  --require-video-consumer defers upstream negotiation until a video\n"
    "  consumer has been accepted and installed (bounded, fail closed).\n"
    "  exit codes: 0 clean stop, 1 runtime failure, 2 usage error, 3 blocked.\n";

int receiver_main(int argc, char **argv) {
    std::string port;
    std::string store_path;
    std::string video_socket = aa::ipc::production_video_socket_path().value;
    std::string input_socket = aa::ipc::production_input_socket_path().value;
    std::int64_t duration_ms = 0;
    bool no_bus = false;
    bool require_video_consumer = false;
    for (int index = 1; index < argc; ++index) {
        if (std::strcmp(argv[index], "--port") == 0 && index + 1 < argc) {
            port = argv[++index];
        } else if (std::strcmp(argv[index], "--store") == 0 && index + 1 < argc) {
            store_path = argv[++index];
        } else if (std::strcmp(argv[index], "--video-socket") == 0 && index + 1 < argc) {
            video_socket = argv[++index];
        } else if (std::strcmp(argv[index], "--input-socket") == 0 && index + 1 < argc) {
            input_socket = argv[++index];
        } else if (std::strcmp(argv[index], "--duration-ms") == 0 && index + 1 < argc) {
            duration_ms = std::strtoll(argv[++index], nullptr, 10);
        } else if (std::strcmp(argv[index], "--no-bus") == 0) {
            no_bus = true;
        } else if (std::strcmp(argv[index], "--require-video-consumer") == 0) {
            require_video_consumer = true;
        } else {
            std::fputs(kUsage, stderr);
            return 2;
        }
    }
    if (port.empty() || duration_ms < 0) {
        std::fputs(kUsage, stderr);
        return 2;
    }

    const auto started = std::chrono::steady_clock::now();
    const auto deadline = duration_ms > 0 ? started + std::chrono::milliseconds{duration_ms}
                                          : std::chrono::steady_clock::time_point::max();
#ifdef AA_WIRED_RECEIVER_SHUTDOWN_TEST
    if (deadline_observation) {
        deadline_observation->cutoff = started + std::chrono::milliseconds{duration_ms};
    }
#endif
    ::signal(SIGINT, on_signal);
    ::signal(SIGTERM, on_signal);

    std::filesystem::path store_file{};
    if (store_path.empty()) {
        auto resolved = aa::trust::ApprovedPhoneStore::default_path();
        if (!resolved.has_value()) {
            log_line("{\"event\":\"err\",\"detail\":\"trust_store_path\"}");
            return 3;
        }
        store_file = resolved.value();
    } else {
        store_file = std::filesystem::path{store_path};
    }
    aa::trust::ApprovedPhoneStore store{store_file};
    if (!store.load().has_value() || !store.usable()) {
        log_line("{\"event\":\"err\",\"detail\":\"trust_store\"}");
        return 3;
    }

    StdoutDiagnostics sink;
    aa::diagnostics::Diagnostics diagnostics{sink};
    aa::session::SteadyClock clock{};
    aa::input::CapabilityDescriptor input = aa::input::CapabilityDescriptor::bench_defaults();

    aa::audio::RouterConfig audio_config = pcm_router_config();
    ConcreteAudioRenderWorker audio_sink{audio_config};
    if (const auto opened = audio_sink.ready(); !opened) {
        log_line(std::string{"{\"event\":\"err\",\"detail\":\"audio_router\",\"code\":\""} +
                 std::string{aa::to_string(opened.error().code())} + "\"}");
        return 3;
    }

    auto video =
        aa::ipc::VideoServer::listen(aa::ipc::VideoSocketPath{video_socket},
                                     aa::ipc::VideoServerOptions{{}, aa::ipc::current_uid()});
    auto input_server =
        aa::ipc::InputServer::listen(aa::ipc::InputSocketPath{input_socket},
                                     aa::ipc::InputServerOptions{aa::ipc::current_uid()});
    if (!video.has_value() || !input_server.has_value()) {
        log_line("{\"event\":\"err\",\"detail\":\"socket\"}");
        return 3;
    }

    aa::app::WiredSessionConfig session_config{};
    session_config.lifecycle.phases.negotiate = Milliseconds{15'000};
    session_config.input_enabled = true;
    session_config.input = input;
    aa::app::WiredSession session{{store, clock, diagnostics}, session_config};
    aa::app::WiredPhoneControl control{store, session};
    aa::ipc::RandomPairingIdSource ids;
    StorePhones phones{control};
    std::optional<aa::ipc::ControlBusService> bus{};
    if (!no_bus) {
        auto started_bus = aa::ipc::ControlBusService::start(
            aa::ipc::ControlBusService::Config{aa::ipc::current_uid()},
            aa::ipc::ControlBusService::Deps{ids, phones});
        if (!started_bus.has_value()) {
            log_line("{\"event\":\"err\",\"detail\":\"dbus\"}");
            return 3;
        }
        bus.emplace(std::move(started_bus.value()));
    }
    if (!session.configure(service_descriptors(input)).has_value()) {
        log_line("{\"event\":\"err\",\"detail\":\"configure\"}");
        return 1;
    }
    VideoOutlet video_outlet{log_line};
    aa::channels::InputSink input_sink;
    aa::channels::SensorSink sensor_sink{input, [&session](std::vector<std::byte> payload) {
        return session.send_channel_payload(aa::protocol::ChannelRole::sensors, std::move(payload));
    }};
    aa::channels::MicrophoneSink microphone_sink{[&session](std::vector<std::byte> payload) {
        return session.send_channel_payload(aa::protocol::ChannelRole::microphone, std::move(payload));
    }};
    if (!register_av_sinks(session, video_outlet, audio_sink).has_value() ||
        !session.register_sink(aa::protocol::ChannelRole::microphone, microphone_sink).has_value() ||
        !session.register_sink(aa::protocol::ChannelRole::input, input_sink).has_value() ||
        !session.register_sink(aa::protocol::ChannelRole::sensors, sensor_sink).has_value()) {
        log_line("{\"event\":\"err\",\"detail\":\"sink\"}");
        return 1;
    }

    aa::transport::UsbDeviceSelector selector{};
    selector.port_context = port;
    aa::transport::AoaAccessoryInfo accessory{};
    log_line("{\"event\":\"connecting\",\"port\":\"" + port + "\"}");

    // Per-link cipher (P1-B/P1-C): the initial attach AND every reconnect build
    // a FRESH cipher bound to the identity the TrustStore admitted for THIS
    // link - never the prior link's cipher object and never the initially
    // captured trust predicate. The predicate re-checks the store on each
    // handshake step, so a mid-handshake revoke fails closed.
    auto bind_link_cipher = [&session, &store](const aa::trust::PhoneIdentity &phone) -> bool {
        auto cipher = aa::transport::make_client_handshake_cipher(
            [&store, phone] { return store.lookup(phone) == aa::trust::Decision::approved; });
        if (!cipher) {
            return false;
        }
        session.attach_cipher(std::move(cipher));
        return true;
    };

    aa::trust::WiredIdentity attempted_identity{};
    auto first_link = aa::app::open_wired_link(selector, accessory, store, {}, &attempted_identity);
    if (!first_link.has_value()) {
        // ADDITIVE attribution at admission rejection (item 2): the link-error
        // record names the rejected attempt's identity+port. The identity is
        // pseudonymized (trust::redacted_label) and the fields only appear when
        // admission actually refused a captured identity; the existing fields
        // (event/detail/code) are unchanged.
        std::string line = std::string{"{\"event\":\"err\",\"detail\":\"link\",\"code\":\""} +
                          std::string{aa::to_string(first_link.error().code())} + "\"";
        if (!attempted_identity.port_context.empty()) {
            line += ",\"phone_id\":\"" +
                    aa::trust::redacted_label(aa::trust::TransportIdentity{attempted_identity}) +
                    "\",\"port\":\"" + attempted_identity.port_context + "\"";
        }
        line += "}";
        log_line(line);
        return 3;
    }
    std::optional<aa::app::WiredLink> link{std::move(first_link.value())};
    session.attach_transport(link->transport.get());
    if (!bind_link_cipher(link->phone)) {
        log_line("{\"event\":\"err\",\"detail\":\"tls_cipher\"}");
        return 3;
    }
    if (!session.apply(aa::session::Event::start_discovery).has_value() ||
        !session.phone_discovered(link->phone).has_value() ||
        !session.apply(aa::session::Event::transport_ready).has_value()) {
        log_line("{\"event\":\"err\",\"detail\":\"lifecycle\"}");
        return 1;
    }
    if (!require_video_consumer) {
        if (!session.send_version_request(1, 6).has_value()) {
            log_line("{\"event\":\"err\",\"detail\":\"version_request\"}");
            return 1;
        }
    }
    log_line("{\"event\":\"ready\",\"state\":\"negotiating\"}");

    InputQueue input_queue;
#ifdef AA_WIRED_RECEIVER_SHUTDOWN_TEST
    ObservedSessionMutex session_mutex;
#else
    std::mutex session_mutex;
#endif
    std::atomic<std::int64_t> active_ms{-1};
    std::atomic<bool> session_failed{false};
    std::atomic<bool> admitted{false};
    std::mutex gate_mutex;
    std::condition_variable gate_cv;
    bool runtime_failed = false;

    auto service_control = [&] {
        if (const auto error = audio_sink.failure()) {
            log_line(audio_failure_event_json("audio_sink_failure", error->code(), audio_sink.failure_context()));
            (void)session.fail(*error);
            session_failed.store(true);
            g_running.store(false);
            return true;
        }
        control.checkpoint();
        if (control.finish()) {
            session_failed.store(true);
            g_running.store(false);
            return true;
        }
        return false;
    };

    auto reconnect = [&]() -> aa::core::Result<void> {
        if (service_control()) {
            return aa::Error{aa::ErrorCode::cancelled};
        }
        if (auto lost = session.apply(aa::session::Event::link_lost); !lost) {
            return lost.error();
        }
        const auto budget = session.io_deadline();
        while (g_running.load()) {
            if (service_control()) {
                return aa::Error{aa::ErrorCode::cancelled};
            }
            if (auto checked = budget.check(); !checked) {
                session.tick();
                return checked.error();
            }
            auto relink = aa::app::open_wired_link(selector, accessory, store, budget);
            if (service_control()) {
                return aa::Error{aa::ErrorCode::cancelled};
            }
            if (relink) {
                if (relink.value().phone.key != link->phone.key) {
                    return aa::Error{aa::ErrorCode::transport_unauthorized_peer};
                }
                link.emplace(std::move(relink.value()));
                control.bind_phone(link->phone);
                link->transport->set_io_checkpoint([&] { control.checkpoint(); });
                session.attach_transport(link->transport.get());
                if (!bind_link_cipher(link->phone)) {
                    return aa::Error{aa::ErrorCode::transport_unauthorized_peer};
                }
                if (auto connected = session.apply(aa::session::Event::reconnect); !connected) {
                    return connected.error();
                }
                if (auto ready = session.apply(aa::session::Event::transport_ready); !ready) {
                    return ready.error();
                }
                const auto sent = session.send_version_request(1, 6);
                if (sent) {
                    return {};
                }
                return sent.error();
            } else if (relink.error().code() != aa::ErrorCode::transport_unavailable &&
                       relink.error().code() != aa::ErrorCode::timeout &&
                       relink.error().code() != aa::ErrorCode::transport_closed &&
                       relink.error().code() != aa::ErrorCode::transport_io) {
                return relink.error();
            }
            std::mutex mutex;
            std::condition_variable_any cv;
            std::unique_lock lock(mutex);
            cv.wait_until(lock, budget.cancellation,
                          std::min(budget.until, std::chrono::steady_clock::now() +
                                                    std::chrono::milliseconds{50}),
                          [] { return false; });
            lock.unlock();
            if (service_control()) {
                return aa::Error{aa::ErrorCode::cancelled};
            }
        }
        return aa::Error{aa::ErrorCode::cancelled};
    };

    std::thread session_thread([&] {
        control.bind_owner();
        control.bind_phone(link->phone);
        link->transport->set_io_checkpoint([&] { control.checkpoint(); });
        if (require_video_consumer) {
            while (!admitted.load() && g_running.load()) {
                {
                    std::lock_guard lock(session_mutex);
                    if (service_control()) {
                        break;
                    }
                    session.tick();
                    if (session.failure()) {
#ifdef AA_WIRED_RECEIVER_SHUTDOWN_TEST
                        if (required_failure_observation) {
                            required_failure_observation->negotiation_timeout =
                                session.failure()->code() == aa::ErrorCode::timeout;
                        }
#endif
                        session_failed.store(true);
                        g_running.store(false);
                        break;
                    }
                    if (std::chrono::steady_clock::now() >= deadline) {
                        g_running.store(false);
                        break;
                    }
                }
                std::unique_lock gate_lock(gate_mutex);
                gate_cv.wait_until(gate_lock, std::min(deadline, std::chrono::steady_clock::now() +
                                                     std::chrono::milliseconds{50}), [&] {
                    return admitted.load() || !g_running.load();
                });
            }
            if (admitted.load() && g_running.load()) {
#ifdef AA_WIRED_RECEIVER_SHUTDOWN_TEST
                if (deadline_observation) {
                    deadline_observation->cross(DeadlineObservation::Edge::owner);
                }
#endif
                const std::lock_guard lock(session_mutex);
                if (!g_running.load() || std::chrono::steady_clock::now() >= deadline) {
                    g_running.store(false);
                } else if (!session.send_version_request(1, 6).has_value()) {
                    log_line("{\"event\":\"err\",\"detail\":\"version_request\"}");
                    session_failed.store(true);
                    g_running.store(false);
                }
#ifdef AA_WIRED_RECEIVER_SHUTDOWN_TEST
                if (deadline_observation) {
                    deadline_observation->finish_owner();
                }
#endif
            }
        }
        while (g_running.load()) {
            aa::core::Result<void> audio_service{};
            {
                std::lock_guard lock(session_mutex);
                audio_service = service_audio_completions(session, audio_sink);
                if (service_control()) {
                    break;
                }
                session.tick();
                if (audio_service) {
                    audio_service = service_audio_completions(session, audio_sink);
                }
                if (session.failure()) {
                    session_failed.store(true);
                    g_running.store(false);
                    break;
                }
                while (auto event = input_queue.pop()) {
                    const auto outcome = session.submit_input(*event);
                    if (service_control()) {
                        break;
                    }
                    log_line(std::string{"{\"event\":\"input_forwarded\",\"ok\":\""} +
                             (outcome.has_value() ? "true" : "false") + "\"}");
                }
            }
            if (!g_running.load()) {
                break;
            }
            auto polled = audio_service ? receive_with_audio(session, audio_sink)
                                        : aa::core::Result<bool>{audio_service.error()};
            {
                std::lock_guard lock(session_mutex);
                if (service_control()) {
                    break;
                }
                if (polled) {
                    if (auto serviced = service_audio_completions(session, audio_sink); !serviced) {
                        polled = serviced.error();
                    }
                }
                if (session.state() == aa::session::State::stopping) {
                    g_running.store(false);
                    break;
                }
                if (!polled.has_value()) {
                    const auto code = polled.error().code();
                    if (!g_running.load()) {
                        break;
                    }
                    log_line(poll_error_json(code, session, audio_sink));
                    if (!session.failure() &&
                        (code == aa::ErrorCode::transport_closed ||
                         code == aa::ErrorCode::transport_io || code == aa::ErrorCode::timeout)) {
                        const auto recovered = reconnect();
                        if (recovered) {
                            continue;
                        }
                        if (!g_running.load()) {
                            break;
                        }
                        (void)session.fail(recovered.error());
                    } else if (!session.failure()) {
                        (void)session.fail(polled.error());
                    }
                    session_failed.store(true);
                    g_running.store(false);
                    break;
                }
                if (session.state() == aa::session::State::negotiating &&
                    session.channel_opens() > 0) {
                    (void)session.apply(aa::session::Event::negotiation_succeeded);
                }
                if (session.state() == aa::session::State::active && active_ms.load() < 0) {
                    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                                             std::chrono::steady_clock::now() - started)
                                             .count();
                    active_ms.store(elapsed);
                    log_line("{\"event\":\"active\",\"startup_ms\":" + std::to_string(elapsed) +
                             "}");
                }
            }
        }
        std::lock_guard lock(session_mutex);
        audio_sink.stop();
        (void)control.finish();
        session.request_stop();
        session.tick();
        if (session.state() == aa::session::State::stopping) {
            (void)session.apply(aa::session::Event::cleanup_finished);
#ifdef AA_WIRED_RECEIVER_SHUTDOWN_TEST
            if (shutdown_observation) {
                shutdown_observation->reset_finished(
                    session.state() == aa::session::State::disconnected);
            }
#endif
        }
        link->transport->set_io_checkpoint({});
        control.close();
    });

    std::thread input_thread([&] {
        while (g_running.load()) {
            auto endpoint = input_server.value().accept_sender(Milliseconds{200});
            if (!endpoint) {
                continue;
            }
            log_line("{\"event\":\"input_accepted\"}");
            while (g_running.load()) {
                auto event = endpoint.value().receive(Milliseconds{200});
                if (!event) {
                    if (event.error().code() == aa::ErrorCode::timeout) {
                        continue;
                    }
                    break;
                }
                input_queue.push(event.value());
            }
            endpoint.value().close();
        }
    });

    const auto runtime_slice = [&] {
        return Milliseconds{std::max<std::int64_t>(0, std::min<std::int64_t>(200,
            std::chrono::ceil<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now()).count()))};
    };
    while (g_running.load() && std::chrono::steady_clock::now() < deadline) {
        if (bus.has_value()) {
            auto step = bus.value().cycle(runtime_slice());
            if (!step.has_value()) {
                runtime_failed = true;
                break;
            }
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds{runtime_slice().count});
        }
#ifdef AA_WIRED_RECEIVER_SHUTDOWN_TEST
        if (deadline_observation) {
            deadline_observation->cross(DeadlineObservation::Edge::dispatch);
        }
#endif
        if (!g_running.load() || std::chrono::steady_clock::now() >= deadline) {
            break;
        }
        const auto accept_timeout = (require_video_consumer && !admitted.load())
                                        ? runtime_slice()
                                        : Milliseconds{0};
        auto producer = video.value().accept_consumer(accept_timeout);
        if (producer.has_value()) {
#ifdef AA_WIRED_RECEIVER_SHUTDOWN_TEST
            if (deadline_observation) {
                deadline_observation->cross(DeadlineObservation::Edge::accepted);
            }
#endif
            if (!g_running.load() || std::chrono::steady_clock::now() >= deadline) {
                break;
            }
            log_line("{\"event\":\"video_accepted\"}");
            video_outlet.install(std::move(producer.value()));
            {
                std::lock_guard gate_lock(gate_mutex);
                if (!g_running.load() || std::chrono::steady_clock::now() >= deadline) {
                    break;
                }
                admitted.store(true);
            }
            gate_cv.notify_all();
#ifdef AA_WIRED_RECEIVER_SHUTDOWN_TEST
            if (deadline_observation) {
                deadline_observation->hold_dispatcher();
            }
#endif
        }
    }

    g_running.store(false);
#ifdef AA_WIRED_RECEIVER_SHUTDOWN_TEST
    if (shutdown_observation) {
        shutdown_observation->published_false();
    }
#endif
    {
        const std::lock_guard lock(session_mutex);
#ifdef AA_WIRED_RECEIVER_SHUTDOWN_TEST
        if (shutdown_observation) {
            shutdown_observation->main_stop();
        }
#endif
        session.request_stop();
    }
#ifdef AA_WIRED_RECEIVER_SHUTDOWN_TEST
    if (shutdown_observation) {
        shutdown_observation->main_join();
    }
#endif
    session_thread.join();
    input_thread.join();

    if (auto finished = audio_sink.finish(); !finished) {
        log_line(audio_failure_event_json("audio_finish", finished.error().code(), audio_sink.failure_context()));
        runtime_failed = true;
    }

    video_outlet.close();
    input_server.value().close();
    video.value().close();
    if (bus.has_value()) {
        bus.value().close();
    }

    const auto total = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now() - started)
                           .count();
    log_line("{\"event\":\"stopping\",\"duration_ms\":" + std::to_string(total) +
             ",\"startup_ms\":" + std::to_string(active_ms.load()) +
             ",\"input_sends\":" + std::to_string(session.input_sends()) +
             ",\"input_failures\":" + std::to_string(session.input_send_failures()) +
             ",\"channel_opens\":" + std::to_string(session.channel_opens()) +
             ",\"version_responses\":" + std::to_string(session.version_responses()) +
             ",\"unhandled_controls\":" + std::to_string(session.unhandled_controls()) + "}");
    return runtime_failed || session_failed.load() ? 1 : 0;
}
} // namespace

int main(int argc, char **argv) {
    aa::wired_receiver::StdoutLogScope logging{g_running};
    const int code = receiver_main(argc, argv);
    const auto error = logging.log.finish();
    return code != 0 ? code : (error ? 1 : 0);
}
