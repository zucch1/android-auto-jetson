// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <aa/audio/AudioRouter.hpp>
#include <aa/channels/AvObservation.hpp>
#include <aa/core/Error.hpp>
#include <aa/core/Time.hpp>
#include <aa/diagnostics/QueueRejection.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace aa::audio {

// Non-blocking stdout/media callbacks supplied by the receiver binary (or a test).
// Leave empty to suppress stdout telemetry in unit fixtures.
struct AudioTelemetryCallbacks final {
    std::function<void(const std::string&)> log_line{};
    std::function<void(std::string_view kind, const std::string&)> log_media{};
    std::function<void(const std::string&)> log_line_nonblocking{};
};

struct AudioDispatchObservation final {
    channels::AvDeliveryId delivery{};
    std::uint64_t message_sequence{};
    core::Nanoseconds original_scheduled{};
    core::Nanoseconds received_at{};
    std::optional<core::Nanoseconds> dequeued_at{};
    std::optional<SinkWriteProgress> write{};
    [[nodiscard, maybe_unused]] friend bool operator==(const AudioDispatchObservation&,
                                                       const AudioDispatchObservation&) = default;
};

struct AudioRejectionSnapshot final {
    diagnostics::QueueRejection queue;
    core::Nanoseconds rejected_at{};
    AudioDispatchObservation rejected;
    std::optional<AudioDispatchObservation> active{};
    std::optional<core::Nanoseconds> oldest_queued_receipt{};
    std::array<std::size_t, 4> occupied_by_role{};
    std::array<std::size_t, 4> pending_completions_by_role{};
    [[nodiscard, maybe_unused]] friend bool operator==(const AudioRejectionSnapshot&,
                                                       const AudioRejectionSnapshot&) = default;
};

enum class AudioFailureOrigin : std::uint8_t {
    open,
    router_submit,
    render_step,
    flush,
    reopen_ack_timeout
};

enum class IdleDisposition : std::uint8_t {
    absent,
    receipt_not_after_start,
    short_interval,
    authorized
};

struct WorkerQueueObservation final {
    std::array<std::size_t, 4> queued_by_role{};
    bool in_flight{};
    bool output_established{};
};

struct WorkerSuccessfulStep final {
    core::Nanoseconds returned_at{};
    core::Nanoseconds reconciled_at{};
    bool pending{};
    std::size_t contributors{};
    std::size_t completed{};
    WorkerQueueObservation queue{};
};

struct WorkerAdmission final {
    AudioDispatchObservation parent;
    core::Nanoseconds accepted_at{}; // Real monotonic, unlike the injectable receipt/start clock.
    std::optional<core::Nanoseconds> idle_since{};
    IdleDisposition disposition{};
    WorkerQueueObservation queue{};
    std::optional<WriteCompletion> last_completion{};
    std::optional<WorkerSuccessfulStep> last_success{};
};

struct WorkerIdleObservation final {
    core::Nanoseconds observed_at{}; // Real monotonic observation time.
    std::optional<core::Nanoseconds> idle_since{};
    bool eligible{};
    WorkerQueueObservation queue{};
};

struct WorkerFailureObservation final {
    WorkerQueueObservation queue{};
    std::optional<WorkerAdmission> admission{};
    std::optional<WorkerAdmission> last_idle_close{};
    std::optional<WriteCompletion> last_completion{};
    std::optional<WorkerSuccessfulStep> last_success{};
    std::optional<WorkerIdleObservation> last_idle{};
};

struct FailureContext final {
    AudioFailureOrigin origin{};
    core::Nanoseconds latched_at{};
    std::optional<AudioDispatchObservation> parent{};
    std::optional<RenderFailureSnapshot> router{};
    WorkerFailureObservation worker{};
};

[[nodiscard]] std::string json_text(std::string_view text);
[[nodiscard]] std::string format_ms(core::Nanoseconds value);

[[nodiscard]] std::string audio_sink_selected_json(const SinkReport& selected,
                                                   std::string_view origin);
void log_sink_selected(const SinkReport& selected, std::string_view origin,
                       const AudioTelemetryCallbacks& telemetry);

[[nodiscard]] std::string audio_write_completed_json(const WriteCompletion& completion);
[[nodiscard]] std::string audio_chunks_json(std::uint64_t count);
// Receiver-level context of a media_timing record (render worker).
struct MediaTimingContext final {
    // Sink routes replaced so far; the counters cover every route.
    std::uint64_t sink_reopens{};
    // Set when the playout clock is on: its target depth.
    std::optional<core::Nanoseconds> playout_target{};
    // Advertised max_unacked per audio channel and the most chunks one
    // channel actually had unacknowledged.
    std::uint32_t credit_window{1};
    std::uint64_t max_unacked_held{};
    // Lossless playout (wired): adds its own counters to the record.
    bool lossless{};
};
// Sum of a receiver's earlier routes (lifetime) and the current one: counters
// add, maxima fold, the latest observations come from `route`.
[[nodiscard]] RouterStats accumulate_router_stats(const RouterStats& lifetime,
                                                  const RouterStats& route);
[[nodiscard]] std::string media_timing_json(const RouterStats& stats,
                                            const MediaTimingContext& context = {});
// #155 catch-up record: one render step's LivePcmDriftPolicy::catch_up_watermark
// trim (RouterStats::frames_caught_up delta and total) plus the worker's
// lifetime event/frame totals across sink reopens.
[[nodiscard]] std::string audio_catch_up_json(std::uint64_t frames, std::uint64_t total,
                                              std::uint32_t rate, std::uint64_t events_lifetime,
                                              std::uint64_t frames_lifetime);

[[nodiscard]] std::string sink_write_json(const SinkWriteProgress& value);
[[nodiscard]] std::string audio_dispatch_json(const AudioDispatchObservation& value);
[[nodiscard]] std::string render_failure_json(const RenderFailureSnapshot& value);
[[nodiscard]] std::string role_counts_json(const std::array<std::size_t, 4>& counts);
[[nodiscard]] std::string worker_queue_json(const WorkerQueueObservation& value);
[[nodiscard]] std::string worker_completion_json(const std::optional<WriteCompletion>& value);
[[nodiscard]] std::string worker_success_json(const std::optional<WorkerSuccessfulStep>& value);
[[nodiscard]] std::string worker_admission_json(const std::optional<WorkerAdmission>& value);
[[nodiscard]] std::string worker_failure_json(const WorkerFailureObservation& value);
[[nodiscard]] std::string audio_failure_json(const FailureContext& value);
[[nodiscard]] std::string audio_failure_event_json(const char* detail, ErrorCode code,
                                                   const std::optional<FailureContext>& context);
[[nodiscard]] std::string audio_rejection_json(const AudioRejectionSnapshot& value);

// Shared router config for both wired and wireless receivers: pulsesink via
// pipewire-pulse, live-PCM drift policy enabled, playout clock enabled by
// default, and playback-time completion telemetry routed through
// `telemetry.log_media`. The defaults are wired's: kWiredPlayoutDepth and
// lossless (PlayoutPolicy::lossless). Wireless passes kWirelessPlayoutDepth and
// stays lossless too (#230). A zero depth restores the legacy
// arrival model, where catch_up_watermark may be set.
[[nodiscard]] RouterConfig pcm_router_config(
    AudioTelemetryCallbacks telemetry = {},
    SinkBackend backend = SinkBackend::pulse,
    core::Nanoseconds playout_depth = kWiredPlayoutDepth,
    core::Nanoseconds catch_up_watermark = core::Nanoseconds{0});
[[nodiscard]] RouterConfig pcm_router_config(
    SinkBackend backend,
    AudioTelemetryCallbacks telemetry,
    core::Nanoseconds playout_depth = kWiredPlayoutDepth,
    core::Nanoseconds catch_up_watermark = core::Nanoseconds{0});

} // namespace aa::audio
