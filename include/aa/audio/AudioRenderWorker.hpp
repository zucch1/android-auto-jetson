// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <aa/audio/AudioRouter.hpp>
#include <aa/audio/AudioTelemetry.hpp>
#include <aa/audio/AvCreditLedger.hpp>
#include <aa/channels/AvObservation.hpp>
#include <aa/channels/AvSink.hpp>
#include <aa/core/Deadline.hpp>
#include <aa/core/Error.hpp>
#include <aa/core/Result.hpp>
#include <aa/core/Time.hpp>
#include <aa/diagnostics/QueueRejection.hpp>
#include <aa/protocol/Protocol.hpp>

#include <array>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace aa::audio {

// Dedicated audio render thread decoupling protocol/session message dispatch
// from blocking GStreamer sink writes. Preserves per-role bounded queues,
// deferred AV credit ACK gating, stall bypass + sink reopen survival, idle gap
// authorization, live-PCM drift telemetry, and failure forensics.
class AudioRenderWorker {
public:
    using Clock = core::Deadline::Clock;
    using IdleClock = std::function<core::Nanoseconds()>;
    using OpenRouter = std::function<core::Result<AudioRouter>(const RouterConfig&)>;

    static constexpr std::chrono::milliseconds kAckReleaseDrainMargin{500};
    static constexpr std::chrono::milliseconds kDefaultAckReleaseDrainTimeout =
        core::kDefaultSendBudget + kAckReleaseDrainMargin;

    std::function<void(const std::array<std::size_t, 4>&, bool, bool,
                       const RouterReport&)> on_idle_observed_for_test;
    std::function<void(const channels::AvDeliveryId&)> on_completion_dequeued_for_test;

    [[nodiscard]] static core::Nanoseconds monotonic_now();

    explicit AudioRenderWorker(
        RouterConfig config,
        AudioTelemetryCallbacks telemetry = {},
        OpenRouter open = AudioRouter::open,
        IdleClock idle_clock = monotonic_now,
        std::chrono::milliseconds ack_release_timeout = kDefaultAckReleaseDrainTimeout);
    ~AudioRenderWorker();
    AudioRenderWorker(const AudioRenderWorker&) = delete;
    AudioRenderWorker& operator=(const AudioRenderWorker&) = delete;

    [[nodiscard]] core::Result<void> ready();
    [[nodiscard]] SinkReport sink_report();

    [[nodiscard]] core::Result<void>
    submit(const protocol::MessageView& message, channels::AvDeliveryId id);
    void invalidate(protocol::ChannelRole role, std::uint64_t generation);
    // Resets per-session AV generation and credit state when a new
    // WiredSession binds on reconnect while keeping the opened audio route.
    void begin_session();

    enum class AckRelease : std::uint8_t { send, deferred, stopped_or_stale, failed };

    [[nodiscard]] std::optional<channels::AvDeliveryId>
    take_completion(Clock::time_point until = Clock::now());
    [[nodiscard]] std::optional<channels::AvDeliveryId>
    take_completion_with_epoch(std::uint64_t& session_epoch,
                               Clock::time_point until = Clock::now());
    [[nodiscard]] AckRelease
    try_begin_ack_release(const channels::AvDeliveryId& completion,
                          std::uint64_t dequeued_epoch,
                          std::optional<Error>& out_failure);
    void end_ack_release();

    [[nodiscard]] std::optional<Error> failure() const;
    [[nodiscard]] std::optional<FailureContext> failure_context() const;
    [[nodiscard]] WorkerFailureObservation worker_observation_for_test() const;
    [[nodiscard]] std::optional<diagnostics::QueueRejection> last_queue_rejection() const;
    [[nodiscard]] std::optional<diagnostics::QueueRejection> failure_queue_rejection() const;
    [[nodiscard]] std::optional<AudioRejectionSnapshot> rejection_snapshot() const;
    [[nodiscard]] std::optional<AudioRejectionSnapshot> current_rejection_snapshot() const;
    [[nodiscard]] bool outstanding() const;
    [[nodiscard]] bool has_completed_for_test() const;
    void observe_av(const channels::AvObservation& value);
    [[nodiscard]] std::optional<AvCreditSnapshot> credit_trace() const;
    [[nodiscard]] AvCreditSnapshot credit_observation_for_test() const;

    [[nodiscard]] bool wait_idle(Clock::time_point until);
    void stop();
    [[nodiscard]] core::Result<void> finish();
    [[nodiscard]] std::optional<RouterReport> final_report() const;

private:
    enum class SinkState : std::uint8_t { ready, reopening };
    struct Reservation {
        std::size_t bytes;
        std::size_t units;
    };
    struct Job {
        protocol::MessageHeader header;
        std::vector<std::byte> pcm;
        channels::AvDeliveryId id;
        core::Nanoseconds receiver_input_at;
        Reservation reservation;
        std::optional<core::Nanoseconds> dequeued_at{};
        WorkerAdmission admission;
        bool rendered_any{};
        std::uint64_t epoch{1};
    };
    struct DeferredCredit {
        channels::AvDeliveryId id;
        Reservation next;
        core::Nanoseconds since;
        bool waited = false;
        std::uint64_t message_sequence{};
        std::size_t bytes{};
    };
    struct InFlightParent {
        protocol::ChannelRole channel;
        std::uint64_t sequence;
        bool rendered_any;
    };
    struct SettlementDiagnostics;

    static constexpr std::size_t kAdvertisedRoles = 3;
    static constexpr std::size_t kRoleSlots = 4;
    static constexpr std::size_t kMaxSlots = kAdvertisedRoles * kRoleSlots;
    // Sink-stall survival (#60): once a blocked write has kept a phone credit
    // waiting this long, the phone may send past max_unacked=1 (HU09 did after
    // ~470 ms), so stalled audio is dropped and ACKed instead of ending the
    // session. A timed-out sink is reopened; repeated reopen failures stay fatal.
    static constexpr std::int64_t kStallBypassAfterNs = 250'000'000;
    static constexpr std::size_t kMaxConsecutiveReopens = 3;

    static constexpr Reservation envelope(std::size_t index) {
        return index == 0 ? Reservation{19200, 19200} : Reservation{6400, 38400};
    }
    static constexpr Reservation capacity(std::size_t index) {
        return index == 0 ? Reservation{38400, 38400} : Reservation{6400, 38400};
    }
    static std::optional<std::size_t> audio_index(protocol::ChannelRole role);

    [[nodiscard]] channels::AvObservation
    credit_event(channels::AvCreditEvent event, channels::AvDeliveryId id) const;
    void record_credit_locked(const channels::AvObservation& value);
    [[nodiscard]] WorkerQueueObservation queue_observation() const;
    [[nodiscard]] WorkerFailureObservation worker_observation() const;
    [[nodiscard]] IdleDisposition idle_disposition(core::Nanoseconds receipt) const;
    void observe_idle(SettlementDiagnostics& diagnostics);
    void complete_idle_resume(SettlementDiagnostics& diagnostics);
    void close_idle(core::Nanoseconds received_at, SettlementDiagnostics& diagnostics);
    void release(const Job& job);
    [[nodiscard]] bool can_credit(std::size_t index, Reservation next) const;
    void grant_deferred(SettlementDiagnostics& diagnostics);
    template <class Queue, class Predicate>
    void discard_if(Queue& queue, Predicate predicate) {
        for (auto it = queue.begin(); it != queue.end();) {
            if (predicate(*it)) {
                release(*it);
                it = queue.erase(it);
            } else {
                ++it;
            }
        }
    }
    void latch(Error error, AudioFailureOrigin origin,
               std::optional<diagnostics::QueueRejection> rejection = {},
               std::optional<AudioDispatchObservation> parent = {},
               std::optional<RenderFailureSnapshot> snapshot = {},
               std::optional<WorkerAdmission> admission = {});
    void maybe_bypass_locked(core::Nanoseconds now);
    void enter_bypass_locked(const char* trigger);
    void reconcile_bypass_locked(const RenderStep& step);
    void exit_bypass_locked(const char* outcome);
    void publish_bypass_logs(std::unique_lock<std::mutex>& lock);
    void cancel_all();
    void run(RouterConfig config, const OpenRouter& open);

    AudioTelemetryCallbacks telemetry_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<Job> queued_;
    std::deque<channels::AvDeliveryId> completed_;
    std::array<std::uint64_t, 4> generations_{};
    std::size_t slots_{};
    std::size_t bytes_{};
    std::size_t units_{};
    std::array<std::size_t, 4> role_slots_{};
    std::array<std::size_t, 4> role_bytes_{};
    std::array<std::size_t, 4> role_units_{};
    std::array<std::deque<Reservation>, 4> credits_{};
    std::array<std::deque<DeferredCredit>, 4> deferred_{};
    std::optional<Error> failure_;
    std::optional<FailureContext> failure_context_;
    std::optional<diagnostics::QueueRejection> last_queue_rejection_;
    std::optional<diagnostics::QueueRejection> failure_queue_rejection_;
    std::optional<AudioDispatchObservation> active_;
    std::optional<SinkWriteProgress> step_write_;
    std::optional<WriteCompletion> step_completion_;
    std::optional<WriteCompletion> last_completion_;
    std::optional<WorkerSuccessfulStep> last_success_;
    std::optional<WorkerAdmission> last_idle_close_;
    std::optional<WorkerIdleObservation> last_idle_;
    AudioRouter* router_{};
    bool in_flight_{};
    SinkState sink_state_{SinkState::ready};
    unsigned ack_sending_{0};
    std::optional<AudioRejectionSnapshot> frozen_rejection_;
    AvCreditLedger credit_ledger_;
    std::optional<RouterReport> final_report_;
    bool initialized_{};
    SinkReport sink_report_{};
    bool stopping_{};
    bool stopped_{};
    const IdleClock idle_clock_;
    const std::chrono::milliseconds ack_release_timeout_;
    bool output_established_{};
    std::optional<core::Nanoseconds> idle_since_;
    std::optional<std::uint64_t> idle_resume_id_;
    std::uint64_t idle_resume_frames_{};
    std::uint64_t idle_sequence_{};
    // Catch-up (#155) totals over the worker's life, across sink reopens.
    std::uint64_t catch_up_events_lifetime_{};
    std::uint64_t catch_up_frames_lifetime_{};
    std::optional<core::Nanoseconds> step_started_;
    std::optional<core::Nanoseconds> bypass_since_;
    std::uint64_t bypass_epoch_{1};
    std::uint64_t bypass_dropped_{};
    std::uint64_t bypass_discarded_{};
    std::uint64_t bypass_partial_{};
    std::uint64_t bypass_rendered_{};
    std::vector<InFlightParent> bypass_in_flight_;
    std::uint64_t bypass_total_{};
    std::uint64_t dropped_total_{};
    std::size_t consecutive_reopens_{};
    std::uint64_t session_epoch_{1};
    std::uint64_t applied_session_epoch_{1};
    std::uint64_t active_step_epoch_{1};
    std::vector<std::string> bypass_logs_;
    std::thread thread_;
};

} // namespace aa::audio
