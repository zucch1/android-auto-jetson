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
#include <atomic>
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

// AAP audio credit window (max_unacked) the receiver advertises on every
// audio channel. 1 is stop-and-wait (wired, and the wireless legacy model):
// one chunk in flight per channel. A window of N lets the phone keep N chunks
// unacknowledged, so a credit round trip up to N chunk periods no longer
// stalls it. For N > 1 the worker reserves all N permits the phone holds at
// every channel reset and bypass exit, and scales each role's slot and byte
// capacity by N, so a chunk the phone may legally send always fits without a
// queue rejection (N == 1 keeps the historical accounting).
struct AudioCreditPolicy final {
    static constexpr std::uint32_t kMaxWindow = 8;
    std::uint32_t window{1};
};

// kWirelessCreditWindow (AudioRouter.hpp) is the wireless default.

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
        std::chrono::milliseconds ack_release_timeout = kDefaultAckReleaseDrainTimeout,
        AudioCreditPolicy credit = {});
    ~AudioRenderWorker();
    AudioRenderWorker(const AudioRenderWorker&) = delete;
    AudioRenderWorker& operator=(const AudioRenderWorker&) = delete;

    [[nodiscard]] core::Result<void> ready();
    // The max_unacked every audio channel advertises (AudioCreditPolicy).
    [[nodiscard]] std::uint32_t credit_window() const noexcept { return credit_window_; }
    // Most chunks one channel has had accepted but not yet released for ACK
    // (our side; the phone's in-flight use shows as max_arrival_burst).
    [[nodiscard]] std::uint64_t max_unacked_held() const;
    // The playout offset audio_write_completed reported for content time
    // `content` (interpolated over the write whose content interval holds it;
    // past the newest write, that write's end offset), or 0 when no playout
    // write of this session is known. Any thread. The wireless video hold is
    // this value at the frame's own timestamp (--video-delay-ms match), which
    // is exactly the audio the A/V contract pairs the frame with: sampling the
    // newest write instead, which pulsesink accepts up to ~200 ms ahead, held
    // frames for future audio's offset (window 12's startup pair, P2-5).
    [[nodiscard]] std::chrono::nanoseconds playout_offset_at(core::Nanoseconds content) const;
    // Lossless playout: these are the offsets the sink really plays with
    // (WriteCompletion::endpoint_offset), and while a write is blocked past its
    // slot the content it holds, and everything after it, is not due before the
    // write returns: the value then keeps that content's due just ahead of now.
    // The newest write's end offset (0 when none).
    [[nodiscard]] std::chrono::nanoseconds current_playout_offset() const;
    // Lossless playout (wired): the video hold may follow it further than
    // wireless's cap (tools/media_playout_options.hpp).
    [[nodiscard]] bool lossless() const noexcept { return lossless_; }
    [[nodiscard]] SinkReport sink_report();

    [[nodiscard]] core::Result<void>
    submit(const protocol::MessageView& message, channels::AvDeliveryId id);
    // The peer stopped (or restarted) the role's stream. Its unreleased credits
    // go with it. Its audio: lossless playout plays out everything accepted
    // before the stop, then starts the next stream (audio that would not leave
    // room for a new stream's credits is dropped and reported instead, see
    // kStopReserveScale); otherwise it is cancelled.
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
    // Accepts nothing more. Lossless playout then plays out what it accepted,
    // for at most kStopDrain; whatever is left (and everything without lossless
    // playout) is discarded.
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
        // First of a stream that follows a stopped one (lossless).
        bool starts_stream{};
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
    // Sink-stall survival (#60): once a blocked write has kept a phone credit
    // waiting this long, the phone may send past max_unacked=1 (HU09 did after
    // ~470 ms), so stalled audio is dropped and ACKed instead of ending the
    // session. A timed-out sink is reopened; repeated reopen failures stay fatal.
    //
    // Lossless playout (#228) draws the line between latency and loss at the
    // router's emergency bound (PlayoutPolicy::emergency_backlog, 2 s), which is
    // also the sink's write deadline: a write that returns within it costs
    // latency only, so the bypass also needs that one write blocked for the
    // whole bound. A sink that fails its write (the deadline, or a lost route)
    // is declared stuck: what is queued is dropped and reported
    // (audio_stall_bypass, trigger sink_write_error) and the sink reopened, so
    // the session survives it as on the arrival model.
    static constexpr std::int64_t kStallBypassAfterNs = 250'000'000;
    static constexpr std::size_t kMaxConsecutiveReopens = 3;

    static constexpr Reservation envelope(std::size_t index) {
        return index == 0 ? Reservation{19200, 19200} : Reservation{6400, 38400};
    }
    static constexpr Reservation capacity(std::size_t index) {
        return index == 0 ? Reservation{38400, 38400} : Reservation{6400, 38400};
    }
    static std::optional<std::size_t> audio_index(protocol::ChannelRole role);
    // Lossless playout (#228) queues by time, not by packet: every role takes
    // the deepest buffer the router keeps (emergency_backlog) plus all that can
    // arrive while one write blocks up to its deadline (sink_write_timeout) plus
    // 400 ms, 4.4 s by default (under 1.2 MB over the three roles). The
    // credit is then granted on receipt through any stall the sink recovers
    // from, whatever the packet size, so the phone never has to hold audio back
    // (its capture buffer is 341 ms). Packets only count against a memory bound,
    // one slot per kSlotNs: of shorter ones a full queue withholds the credit,
    // it never drops.
    static constexpr std::int64_t kEnvelopeNs = 200'000'000; // one capacity() unit
    static constexpr std::int64_t kSlotNs = 1'000'000;
    [[nodiscard]] static std::size_t lossless_scale(const RouterConfig& config) noexcept;
    // Parents of one role handed to the router at a time (lossless): its
    // scheduling cost grows with every pending parent, and a 4.4 s queue of 5 ms
    // packets holds hundreds. The rest waits here, as before its decode.
    static constexpr std::size_t kRouterParents = 64;
    // A stopped stream plays out unless that leaves a new one less than this
    // many envelopes (its largest message); then it is cancelled and reported (audio_stop_discarded).
    static constexpr std::size_t kStopReserveScale = 2;
    // Longest stop() lets accepted audio play out: the emergency bound, the
    // most the router keeps.
    static constexpr std::chrono::milliseconds kStopDrain{2000};
    // Capacity of one role under the credit window (AudioCreditPolicy).
    [[nodiscard]] std::size_t role_slot_cap() const noexcept {
        return lossless_ ? capacity_scale_ * static_cast<std::size_t>(kEnvelopeNs / kSlotNs)
                         : kRoleSlots * capacity_scale_;
    }
    [[nodiscard]] Reservation role_capacity(std::size_t index) const noexcept {
        const auto base = capacity(index);
        return {base.bytes * capacity_scale_, base.units * capacity_scale_};
    }
    // The phone holds the whole window again after a channel reset: all N
    // permits are reserved (stop-and-wait keeps its single implicit permit).
    void seed_credits_locked(std::size_t index);
    void note_unacked_locked(std::size_t index);

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
    [[nodiscard]] bool dequeueable_locked(const Job& job) const;
    [[nodiscard]] bool draining_locked() const;
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
    // Catch-up (#155) totals over the worker's life, across reopens and sessions.
    std::uint64_t catch_up_events_lifetime_{};
    std::uint64_t catch_up_frames_lifetime_{};
    std::uint64_t overflow_frames_lifetime_{};
    const std::uint32_t credit_window_;
    const bool lossless_;
    const std::size_t capacity_scale_;
    std::array<std::uint64_t, 4> unacked_{};
    std::uint64_t max_unacked_held_{};
    std::uint64_t playout_underruns_logged_{};
    std::uint64_t playout_anchors_logged_{};
    // Reported playout writes of the current session, oldest first: content
    // interval and its start/end offsets (playout_offset_at).
    struct PlayoutWrite final {
        std::int64_t content_start;
        std::int64_t content_end;
        std::int64_t start_offset;
        std::int64_t end_offset;
    };
    static constexpr std::size_t kPlayoutHistory = 256; // ~11 s of A26 chunks
    // Lossless: 2 ms packets still cover the longest video hold.
    static constexpr std::size_t kLosslessPlayoutHistory = 1024;
    // The write the sink holds now (lossless).
    struct InFlightWrite final {
        std::int64_t scheduled;
        std::int64_t content_start;
    };
    std::optional<InFlightWrite> write_in_flight_; // under playout_mutex_
    // How far ahead of now content behind a blocked write is kept due: more
    // than the video outlet's release slice (5 ms), so a frame is not released
    // just before the write returns.
    static constexpr std::int64_t kBlockedWriteLeadNs = 10'000'000;
    static constexpr std::int64_t kContentRoundingNs = 50'000;
    // Guards playout_writes_ only (never held with mutex_ the other way round).
    mutable std::mutex playout_mutex_;
    std::deque<PlayoutWrite> playout_writes_{};
    const std::int64_t playout_depth_ns_;
    const std::int64_t emergency_backlog_ns_;
    // Lossless: a stop whose boundary the router has not been given yet.
    std::array<bool, 4> stream_end_pending_{};
    std::array<std::size_t, 4> router_parents_{};
    std::optional<Clock::time_point> drain_until_;
    bool router_busy_{};
    std::uint64_t playout_retimes_logged_{};
    [[nodiscard]] std::chrono::nanoseconds recorded_offset_locked(core::Nanoseconds content) const;
    void clear_playout_writes();
    std::optional<core::Nanoseconds> step_started_;
    core::Nanoseconds step_entered_{}; // step_started_ on the real monotonic clock
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
