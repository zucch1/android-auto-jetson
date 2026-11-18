// SPDX-License-Identifier: GPL-3.0-or-later
#include <aa/audio/AudioRenderWorker.hpp>

#include <algorithm>
#include <chrono>
#include <utility>

namespace aa::audio {

// Each call owns its batch: no shared publication queue or lock-held logging.
// At most kMaxSlots parents can contribute or receive a deferred credit in
// one reconciliation. Idle completion retires its ID on the first capture.
struct AudioRenderWorker::SettlementDiagnostics {
    struct Credit {
        channels::AvDeliveryId id;
        std::int64_t duration;
    };
    struct IdleSkip {
        std::uint64_t id;
        std::uint64_t frames;
        std::uint64_t total;
    };
    struct IdleResume {
        std::uint64_t id;
        core::Nanoseconds start;
        core::Nanoseconds end;
    };
    struct CatchUp {
        std::uint64_t frames;
        std::uint64_t total;
        std::uint32_t rate;
    };
    struct SinkStall {
        core::Nanoseconds start;
        core::Nanoseconds end;
        core::Nanoseconds step_started;
        core::Nanoseconds returned_at;
        std::optional<SinkWriteProgress> write;
    };

    const AudioTelemetryCallbacks& telemetry;
    std::array<Credit, kMaxSlots> credits{};
    std::size_t credit_count{};
    std::array<AudioDispatchObservation, kMaxSlots> correlated{};
    std::size_t correlated_count{};
    std::optional<WriteCompletion> completion;
    std::optional<SinkStall> stall;
    std::optional<IdleSkip> skipped;
    std::optional<IdleResume> resumed;
    std::optional<CatchUp> caught_up;

    explicit SettlementDiagnostics(const AudioTelemetryCallbacks& callbacks) noexcept
        : telemetry(callbacks) {}

    void publish() const {
        if (!telemetry.log_media) { return; }
        if (stall) {
            telemetry.log_media("audio_sink_stall",
                "{\"event\":\"audio_sink_stall\",\"blocked_from_ns\":" +
                std::to_string(stall->start.count) + ",\"completed_ns\":" +
                std::to_string(stall->end.count) + ",\"stall_ns\":" +
                std::to_string(stall->end.count - stall->start.count) + ",\"scheduled_ns\":" +
                std::to_string(completion->scheduled.count) + ",\"scheduled_end_ns\":" +
                std::to_string(completion->scheduled_end.count) + ",\"step_started_ns\":" +
                std::to_string(stall->step_started.count) + ",\"returned_at_ns\":" +
                std::to_string(stall->returned_at.count) + ",\"write\":" +
                (stall->write ? sink_write_json(*stall->write) : "null") + "}");
        }
        if (resumed) {
            telemetry.log_media("audio_idle_resume",
                "{\"event\":\"audio_idle_resume\",\"id\":" + std::to_string(resumed->id) +
                ",\"start_ns\":" + std::to_string(resumed->start.count) + ",\"end_ns\":" +
                std::to_string(resumed->end.count) + ",\"idle_ns\":" +
                std::to_string(resumed->end.count - resumed->start.count) +
                ",\"reason\":\"all_role_idle\"}");
        }
        if (caught_up) {
            // #155 catch-up telemetry seam: frames is this step's increment at
            // the output rate (reset-free delta on one router); frames_total
            // resets on a sink reopen.
            telemetry.log_media("audio_catch_up",
                audio_catch_up_json(caught_up->frames, caught_up->total, caught_up->rate));
        }
        if (skipped) {
            telemetry.log_media("audio_idle_skipped",
                "{\"event\":\"audio_idle_skipped\",\"id\":" + std::to_string(skipped->id) +
                ",\"frames_idle_skipped\":" + std::to_string(skipped->frames) +
                ",\"frames_idle_skipped_total\":" + std::to_string(skipped->total) + "}");
        }
        for (std::size_t i = 0; i < correlated_count; ++i) {
            telemetry.log_media("audio_write_correlated",
                "{\"event\":\"audio_write_correlated\",\"scheduled_ns\":" +
                std::to_string(completion->scheduled.count) + ",\"scheduled_end_ns\":" +
                std::to_string(completion->scheduled_end.count) + ",\"completed_ns\":" +
                std::to_string(completion->completed.count) + ",\"dispatch\":" +
                audio_dispatch_json(correlated[i]) + "}");
        }
        for (std::size_t i = 0; i < credit_count; ++i) {
            telemetry.log_media("audio_credit_deferred",
                "{\"event\":\"audio_credit_deferred\",\"role\":" +
                std::to_string(static_cast<unsigned>(credits[i].id.role)) + ",\"sequence\":" +
                std::to_string(credits[i].id.sequence) + ",\"deferred_ns\":" +
                std::to_string(credits[i].duration) + "}");
        }
    }
};

core::Nanoseconds AudioRenderWorker::monotonic_now() {
    return core::Nanoseconds{
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            Clock::now().time_since_epoch()).count()};
}

AudioRenderWorker::AudioRenderWorker(RouterConfig config,
                                     AudioTelemetryCallbacks telemetry,
                                     OpenRouter open,
                                     IdleClock idle_clock)
    : telemetry_(std::move(telemetry)),
      idle_clock_(std::move(idle_clock)),
      thread_([this, config = std::move(config), open = std::move(open)] {
          run(config, open);
      }) {}

AudioRenderWorker::~AudioRenderWorker() { (void)finish(); }

core::Result<void> AudioRenderWorker::ready() {
    std::unique_lock lock(mutex_);
    cv_.wait(lock, [&] { return initialized_; });
    if (failure_) { return *failure_; }
    return {};
}

SinkReport AudioRenderWorker::sink_report() {
    std::lock_guard lock(mutex_);
    return sink_report_;
}

core::Result<void>
AudioRenderWorker::submit(const protocol::MessageView& message, channels::AvDeliveryId id) {
    const auto receiver_input_at = idle_clock_();
    SettlementDiagnostics diagnostics{telemetry_};
    std::unique_lock lock(mutex_);
    if (failure_) {
        last_queue_rejection_ = failure_queue_rejection_;
        return *failure_;
    }
    if (stopping_) { return Error{ErrorCode::cancelled}; }
    last_queue_rejection_.reset();
    const auto role = audio_index(message.header.channel);
    if (!role || *role >= kAdvertisedRoles || id.role != message.header.channel ||
        id.generation != generations_[*role]) {
        return Error{ErrorCode::invalid_argument};
    }
    const auto bytes = message.payload.size();
    const auto reject = [&](diagnostics::QueueReason reason, std::size_t units) {
        last_queue_rejection_ = diagnostics::QueueRejection{
            diagnostics::QueueSource::audio_worker, reason, message.header.channel,
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
        for (const auto& job : queued_) {
            ++snapshot.occupied_by_role[*audio_index(job.id.role)];
            if (!snapshot.oldest_queued_receipt ||
                job.receiver_input_at < *snapshot.oldest_queued_receipt) {
                snapshot.oldest_queued_receipt = job.receiver_input_at;
            }
        }
        for (const auto& completion : completed_) {
            ++snapshot.pending_completions_by_role[*audio_index(completion.role)];
        }
        frozen_rejection_ = snapshot;
        channels::AvObservation observation{};
        observation.event = channels::AvCreditEvent::rejected;
        observation.delivery = id;
        observation.session_active = true;
        observation.message_sequence = message.header.sequence;
        observation.bytes = bytes;
        observation.error = ErrorCode::ipc_queue_full;
        record_credit_locked(observation);
        return Error{ErrorCode::ipc_queue_full};
    };
    if (bytes > envelope(*role).bytes) {
        return Error{ErrorCode::protocol_payload_oversize};
    }
    maybe_bypass_locked(monotonic_now());
    if (bypass_since_) {
        ++bypass_dropped_;
        completed_.push_back(id);
        auto observation = credit_event(channels::AvCreditEvent::granted, id);
        observation.message_sequence = message.header.sequence;
        observation.bytes = bytes;
        record_credit_locked(observation);
        cv_.notify_all();
        publish_bypass_logs(lock);
        return {};
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
        return reject(
            role_slots_[*role] + permits >= kRoleSlots
                ? diagnostics::QueueReason::slots
                : role_bytes_[*role] + permits * promised.bytes + bytes > capacity(*role).bytes
                      ? diagnostics::QueueReason::bytes
                      : diagnostics::QueueReason::weighted_units,
            units);
    }
    const WorkerAdmission admission{
        {id, message.header.sequence, message.header.timestamp, receiver_input_at},
        monotonic_now(), idle_since_, idle_disposition(receiver_input_at), queue_observation(),
        last_completion_, last_success_};
    queued_.push_back(Job{message.header,
                          {message.payload.begin(), message.payload.end()},
                          id,
                          receiver_input_at,
                          {bytes, units},
                          {},
                          admission});
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

void AudioRenderWorker::invalidate(protocol::ChannelRole role, std::uint64_t generation) {
    SettlementDiagnostics diagnostics{telemetry_};
    std::unique_lock lock(mutex_);
    const auto index = audio_index(role);
    if (!index) { return; }
    if (generations_[*index] == generation) { return; }
    if (router_) { router_->cancel_pending(role); }
    credits_[*index].clear();
    generations_[*index] = generation;
    const auto stale = [&](const auto& entry) {
        return entry.id.role == role && entry.id.generation != generation;
    };
    discard_if(queued_, stale);
    std::erase_if(completed_, [&](const channels::AvDeliveryId& entry) {
        return entry.role == role && entry.generation != generation;
    });
    std::erase_if(deferred_[*index], [&](const DeferredCredit& entry) {
        return entry.id.generation != generation;
    });
    observe_idle(diagnostics);
    grant_deferred(diagnostics);
    cv_.notify_all();
    lock.unlock();
    diagnostics.publish();
}

void AudioRenderWorker::begin_session() {
    SettlementDiagnostics diagnostics{telemetry_};
    std::unique_lock lock(mutex_);
    if (failure_ || stopping_) { return; }
    cancel_all();
    discard_if(queued_, [](const auto&) { return true; });
    completed_.clear();
    credits_ = {};
    deferred_ = {};
    generations_ = {};
    last_queue_rejection_.reset();
    credit_ledger_.reset();
    observe_idle(diagnostics);
    cv_.notify_all();
    lock.unlock();
    diagnostics.publish();
}

std::optional<channels::AvDeliveryId>
AudioRenderWorker::take_completion(Clock::time_point until) {
    std::unique_lock lock(mutex_);
    maybe_bypass_locked(monotonic_now());
    cv_.wait_until(lock, until, [&] { return !completed_.empty() || failure_ || stopped_; });
    std::optional<channels::AvDeliveryId> completion;
    if (!completed_.empty()) {
        completion = completed_.front();
        record_credit_locked(credit_event(channels::AvCreditEvent::taken, *completion));
        completed_.pop_front();
    }
    publish_bypass_logs(lock);
    return completion;
}

std::optional<Error> AudioRenderWorker::failure() const {
    std::lock_guard lock(mutex_);
    return failure_;
}

std::optional<FailureContext> AudioRenderWorker::failure_context() const {
    std::lock_guard lock(mutex_);
    return failure_context_;
}

WorkerFailureObservation AudioRenderWorker::worker_observation_for_test() const {
    std::lock_guard lock(mutex_);
    auto observation = worker_observation();
    if (!queued_.empty()) { observation.admission = queued_.back().admission; }
    return observation;
}

std::optional<diagnostics::QueueRejection> AudioRenderWorker::last_queue_rejection() const {
    std::lock_guard lock(mutex_);
    return last_queue_rejection_;
}

std::optional<diagnostics::QueueRejection> AudioRenderWorker::failure_queue_rejection() const {
    std::lock_guard lock(mutex_);
    return failure_queue_rejection_;
}

std::optional<AudioRejectionSnapshot> AudioRenderWorker::rejection_snapshot() const {
    std::lock_guard lock(mutex_);
    return frozen_rejection_;
}

std::optional<AudioRejectionSnapshot> AudioRenderWorker::current_rejection_snapshot() const {
    std::lock_guard lock(mutex_);
    if (frozen_rejection_ && last_queue_rejection_ &&
        frozen_rejection_->queue == *last_queue_rejection_) {
        return frozen_rejection_;
    }
    return std::nullopt;
}

bool AudioRenderWorker::outstanding() const {
    std::lock_guard lock(mutex_);
    return slots_ != 0 || in_flight_ || failure_.has_value() || bypass_since_.has_value() ||
           !completed_.empty();
}

void AudioRenderWorker::observe_av(const channels::AvObservation& value) {
    std::lock_guard lock(mutex_);
    record_credit_locked(value);
}

std::optional<AvCreditSnapshot> AudioRenderWorker::credit_trace() const {
    std::lock_guard lock(mutex_);
    return credit_ledger_.frozen();
}

AvCreditSnapshot AudioRenderWorker::credit_observation_for_test() const {
    std::lock_guard lock(mutex_);
    return credit_ledger_.snapshot();
}

bool AudioRenderWorker::wait_idle(Clock::time_point until) {
    std::unique_lock lock(mutex_);
    return cv_.wait_until(lock, until, [&] { return slots_ == 0 && !in_flight_; });
}

void AudioRenderWorker::stop() {
    std::lock_guard lock(mutex_);
    stopping_ = true;
    idle_since_.reset();
    idle_resume_id_.reset();
    cancel_all();
    discard_if(queued_, [](const auto&) { return true; });
    completed_.clear();
    credits_ = {};
    deferred_ = {};
    cv_.notify_all();
}

core::Result<void> AudioRenderWorker::finish() {
    stop();
    if (thread_.joinable()) { thread_.join(); }
    if (const auto error = failure()) { return *error; }
    return {};
}

std::optional<RouterReport> AudioRenderWorker::final_report() const {
    std::lock_guard lock(mutex_);
    return final_report_;
}

channels::AvObservation
AudioRenderWorker::credit_event(channels::AvCreditEvent event, channels::AvDeliveryId id) const {
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

void AudioRenderWorker::record_credit_locked(const channels::AvObservation& value) {
    if (credit_ledger_.frozen()) { return; }
    AvCreditState state{};
    for (std::size_t i = 0; i < state.roles.size(); ++i) {
        state.roles[i] = {role_slots_[i], role_bytes_[i], role_units_[i], credits_[i].size(),
                          deferred_[i].size(), 0, kRoleSlots, capacity(i).bytes,
                          capacity(i).units, envelope(i).bytes};
        for (const auto& pending : deferred_[i]) {
            if (value.pending == pending.id) { state.pending_deferred = true; }
        }
    }
    for (const auto& completion : completed_) {
        if (const auto role = AvCreditLedger::role_index(completion.role)) {
            ++state.roles[*role].completions;
        }
        if (value.pending == completion) { state.pending_available = true; }
    }
    credit_ledger_.record(value, state);
}

WorkerQueueObservation AudioRenderWorker::queue_observation() const {
    WorkerQueueObservation value{{}, in_flight_, output_established_};
    for (const auto& job : queued_) { ++value.queued_by_role[*audio_index(job.id.role)]; }
    return value;
}

WorkerFailureObservation AudioRenderWorker::worker_observation() const {
    return {queue_observation(), {}, last_idle_close_, last_completion_, last_success_, last_idle_};
}

IdleDisposition AudioRenderWorker::idle_disposition(core::Nanoseconds receipt) const {
    if (!idle_since_) { return IdleDisposition::absent; }
    if (receipt <= *idle_since_) { return IdleDisposition::receipt_not_after_start; }
    if (receipt.count - idle_since_->count <= 200'000'000) {
        return IdleDisposition::short_interval;
    }
    return IdleDisposition::authorized;
}

void AudioRenderWorker::observe_idle(SettlementDiagnostics& diagnostics) {
    const bool eligible =
        router_ && !stopping_ && !failure_ && output_established_ && queued_.empty() && !in_flight_;
    if (eligible) {
        router_->retire_all_role_idle();
        complete_idle_resume(diagnostics);
        if (!idle_since_) { idle_since_ = idle_clock_(); }
    }
    last_idle_ = WorkerIdleObservation{monotonic_now(), idle_since_, eligible, queue_observation()};
}

void AudioRenderWorker::complete_idle_resume(SettlementDiagnostics& diagnostics) {
    if (router_ && idle_resume_id_ && idle_resume_frames_ != 0) {
        diagnostics.skipped = SettlementDiagnostics::IdleSkip{
            *idle_resume_id_, idle_resume_frames_, router_->report().stats.frames_idle_skipped};
    }
    idle_resume_id_.reset();
    idle_resume_frames_ = 0;
}

void AudioRenderWorker::close_idle(core::Nanoseconds received_at,
                                   SettlementDiagnostics& diagnostics) {
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

std::optional<std::size_t> AudioRenderWorker::audio_index(protocol::ChannelRole role) {
    using protocol::ChannelRole;
    switch (role) {
    case ChannelRole::media_audio: return 0;
    case ChannelRole::guidance_audio: return 1;
    case ChannelRole::system_audio: return 2;
    case ChannelRole::speech_audio: return 3;
    default: return std::nullopt;
    }
}

void AudioRenderWorker::release(const Job& job) {
    --slots_;
    --role_slots_[*audio_index(job.id.role)];
    bytes_ -= job.reservation.bytes;
    units_ -= job.reservation.units;
    role_bytes_[*audio_index(job.id.role)] -= job.reservation.bytes;
    role_units_[*audio_index(job.id.role)] -= job.reservation.units;
}

bool AudioRenderWorker::can_credit(std::size_t index, Reservation next) const {
    const auto permits = credits_[index].size() + 1;
    return role_slots_[index] + permits <= kRoleSlots &&
           role_bytes_[index] + permits * next.bytes <= capacity(index).bytes &&
           role_units_[index] + permits * next.units <= capacity(index).units;
}

void AudioRenderWorker::grant_deferred(SettlementDiagnostics& diagnostics) {
    for (std::size_t index = 0; index < deferred_.size(); ++index) {
        auto& pending = deferred_[index];
        while (!pending.empty() && can_credit(index, pending.front().next)) {
            const auto granted = pending.front();
            pending.pop_front();
            credits_[index].push_back(granted.next);
            completed_.push_back(granted.id);
            auto observation = credit_event(channels::AvCreditEvent::granted, granted.id);
            observation.message_sequence = granted.message_sequence;
            observation.bytes = granted.bytes;
            record_credit_locked(observation);
            if (!granted.waited) { continue; }
            const auto now = monotonic_now();
            if (router_) { router_->note_backpressure(granted.since, now); }
            diagnostics.credits.at(diagnostics.credit_count++) = {granted.id,
                                                                  now.count - granted.since.count};
        }
        for (auto& entry : pending) {
            if (!entry.waited) {
                auto observation = credit_event(channels::AvCreditEvent::deferred, entry.id);
                observation.message_sequence = entry.message_sequence;
                observation.bytes = entry.bytes;
                record_credit_locked(observation);
            }
            entry.waited = true;
        }
    }
}

void AudioRenderWorker::latch(Error error, AudioFailureOrigin origin,
                              std::optional<diagnostics::QueueRejection> rejection,
                              std::optional<AudioDispatchObservation> parent,
                              std::optional<RenderFailureSnapshot> snapshot,
                              std::optional<WorkerAdmission> admission) {
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
    discard_if(queued_, [](const auto&) { return true; });
    credits_ = {};
    deferred_ = {};
    cv_.notify_all();
}

void AudioRenderWorker::maybe_bypass_locked(core::Nanoseconds now) {
    if (bypass_since_ || !in_flight_ || stopping_ || failure_) { return; }
    for (const auto& pending : deferred_) {
        if (!pending.empty() && now.count - pending.front().since.count >= kStallBypassAfterNs) {
            enter_bypass_locked("credit_wait");
            return;
        }
    }
}

void AudioRenderWorker::enter_bypass_locked(const char* trigger) {
    const auto at = idle_clock_();
    bypass_since_ = step_started_.value_or(at);
    bypass_dropped_ = 0;
    bypass_discarded_ = 0;
    bypass_partial_ = 0;
    bypass_rendered_ = 0;
    bypass_in_flight_.clear();
    for (const auto& job : queued_) {
        if (job.dequeued_at) {
            bypass_in_flight_.push_back({job.header.channel, job.header.sequence, job.rendered_any});
        } else {
            ++bypass_discarded_;
        }
    }
    ++bypass_total_;
    cancel_all();
    discard_if(queued_, [](const auto&) { return true; });
    credits_ = {};
    std::size_t granted = 0;
    for (auto& pending : deferred_) {
        for (const auto& entry : pending) {
            completed_.push_back(entry.id);
            auto observation = credit_event(channels::AvCreditEvent::granted, entry.id);
            observation.message_sequence = entry.message_sequence;
            observation.bytes = entry.bytes;
            record_credit_locked(observation);
            ++granted;
        }
        pending.clear();
    }
    idle_since_.reset();
    idle_resume_id_.reset();
    idle_resume_frames_ = 0;
    bypass_logs_.push_back(
        "{\"event\":\"audio_stall_bypass\",\"phase\":\"begin\",\"trigger\":\"" +
        std::string{trigger} + "\",\"since_ns\":" + std::to_string(bypass_since_->count) +
        ",\"at_ns\":" + std::to_string(at.count) + ",\"discarded\":" +
        std::to_string(bypass_discarded_) + ",\"in_flight\":" +
        std::to_string(bypass_in_flight_.size()) + ",\"granted\":" + std::to_string(granted) + "}");
    cv_.notify_all();
}

void AudioRenderWorker::reconcile_bypass_locked(const RenderStep& step) {
    const auto has = [](const auto& headers, const InFlightParent& parent) {
        return std::any_of(headers.begin(), headers.end(), [&](const auto& header) {
            return header.channel == parent.channel && header.sequence == parent.sequence;
        });
    };
    std::erase_if(bypass_in_flight_, [&](InFlightParent& parent) {
        if (has(step.delivered, parent)) {
            ++bypass_rendered_;
            return true;
        }
        parent.rendered_any = parent.rendered_any || has(step.contributors, parent);
        return false;
    });
}

void AudioRenderWorker::exit_bypass_locked(const char* outcome) {
    const auto at = idle_clock_();
    for (const auto& parent : bypass_in_flight_) {
        ++(parent.rendered_any ? bypass_partial_ : bypass_discarded_);
    }
    bypass_in_flight_.clear();
    dropped_total_ += bypass_dropped_;
    bypass_logs_.push_back(
        "{\"event\":\"audio_stall_bypass\",\"phase\":\"end\",\"outcome\":\"" +
        std::string{outcome} + "\",\"since_ns\":" + std::to_string(bypass_since_->count) +
        ",\"at_ns\":" + std::to_string(at.count) + ",\"stall_ns\":" +
        std::to_string(at.count - bypass_since_->count) + ",\"dropped\":" +
        std::to_string(bypass_dropped_) + ",\"discarded\":" + std::to_string(bypass_discarded_) +
        ",\"partial\":" + std::to_string(bypass_partial_) + ",\"rendered\":" +
        std::to_string(bypass_rendered_) + ",\"bypasses_total\":" +
        std::to_string(bypass_total_) + ",\"dropped_total\":" +
        std::to_string(dropped_total_) + "}");
    bypass_since_.reset();
}

void AudioRenderWorker::publish_bypass_logs(std::unique_lock<std::mutex>& lock) {
    auto lines = std::exchange(bypass_logs_, {});
    lock.unlock();
    if (telemetry_.log_line) {
        for (const auto& line : lines) { telemetry_.log_line(line); }
    }
}

void AudioRenderWorker::cancel_all() {
    if (!router_) { return; }
    for (const auto role : {protocol::ChannelRole::media_audio,
                            protocol::ChannelRole::guidance_audio,
                            protocol::ChannelRole::system_audio}) {
        router_->cancel_pending(role);
    }
}

void AudioRenderWorker::run(RouterConfig config, const OpenRouter& open) {
    config.on_sink_write_progress =
        [this, observe = config.on_sink_write_progress](const auto& value) {
            {
                std::lock_guard lock(mutex_);
                if (!value.readiness_probe) { step_write_ = value; }
            }
            if (observe) { observe(value); }
        };
    const auto completed = config.on_write_completed;
    config.on_write_completed = [this](const auto& value) {
        std::lock_guard lock(mutex_);
        last_completion_ = value;
        step_completion_ = value;
    };
    auto opened = open(config);
    {
        std::lock_guard lock(mutex_);
        initialized_ = true;
        if (!opened) {
            latch(opened.error(), AudioFailureOrigin::open);
        } else {
            sink_report_ = opened.value().report().sink;
            if (opened.value().state() == RouterState::degraded) {
                latch(Error{ErrorCode::audio_sink_unavailable}, AudioFailureOrigin::open);
            }
        }
        cv_.notify_all();
    }
    if (!opened) { return; }
    auto local_router = std::move(opened).value();
    {
        std::lock_guard lock(mutex_);
        router_ = &local_router;
    }
    const auto emit_media_timing = [this](const auto& stats) {
        if (stats.frames_written == 0 || !telemetry_.log_media) { return; }
        telemetry_.log_media("media_timing", media_timing_json(stats));
    };
    std::uint64_t chunks = 0;
    for (;;) {
        std::unique_lock lock(mutex_);
        cv_.wait(lock, [&] { return stopping_ || !queued_.empty(); });
        if (stopping_) { break; }
        struct Parent final {
            AudioDispatchObservation dispatch;
            protocol::MessageHeader header;
            WorkerAdmission admission;
        };
        std::vector<Parent> parents;
        for (auto& job : queued_) {
            if (!job.dequeued_at) {
                job.dequeued_at = monotonic_now();
                const auto submitted = local_router.submit({job.header, job.pcm}, job.receiver_input_at);
                if (!submitted) {
                    const AudioDispatchObservation parent{job.id, job.header.sequence,
                                                        job.header.timestamp, job.receiver_input_at,
                                                        job.dequeued_at};
                    latch(submitted.error(), AudioFailureOrigin::router_submit,
                          local_router.report().queue_rejection, parent, {}, job.admission);
                    break;
                }
                // Decoded storage now belongs to the router; retain only
                // the parent's identity and logical occupancy charge.
                std::vector<std::byte>{}.swap(job.pcm);
            }
            parents.push_back({{job.id, job.header.sequence, job.header.timestamp,
                                job.receiver_input_at, job.dequeued_at},
                               job.header,
                               job.admission});
        }
        if (stopping_) { break; }
        active_ = parents.size() == 1 ? std::optional{parents.front().dispatch} : std::nullopt;
        step_write_.reset();
        step_completion_.reset();
        in_flight_ = true;
        step_started_ = idle_clock_();
        const auto before = local_router.report().stats.frames_written;
        const auto idle_before = local_router.report().stats.frames_idle_skipped;
        // #155 catch-up seam: capture `caught_up_before = local_router.report().stats.frames_caught_up` here.
        lock.unlock();
        const auto step_started = monotonic_now();
        const auto result = local_router.render_step();
        const auto returned_at = monotonic_now();
        SettlementDiagnostics diagnostics{telemetry_};
        lock.lock();
        diagnostics.completion = step_completion_;
        if (step_completion_) {
            const auto& completion = *step_completion_;
            const auto piece_duration = completion.scheduled_end.count - completion.scheduled.count;
            const auto elapsed = completion.completed.count - step_started.count;
            if (elapsed > piece_duration) {
                // Exclude ordinary piece pacing, even on a stale schedule.
                // elapsed > duration bounds this addition below completed.
                const auto blocked_from = std::max(
                    completion.scheduled_end,
                    core::Nanoseconds{step_started.count + piece_duration});
                if (completion.completed > blocked_from) {
                    local_router.note_backpressure(blocked_from, completion.completed);
                    diagnostics.stall = SettlementDiagnostics::SinkStall{
                        blocked_from, completion.completed, step_started, returned_at, {}};
                    if (result && step_write_ && !step_write_->readiness_probe &&
                        step_write_->effective_scheduled == completion.scheduled &&
                        step_write_->completed_at && !step_write_->outcome) {
                        diagnostics.stall->write = step_write_;
                    }
                }
            }
        }
        in_flight_ = false;
        const auto stats = local_router.report().stats;
        // #155 catch-up seam: when `stats.frames_caught_up > caught_up_before`, populate
        // `diagnostics.caught_up = SettlementDiagnostics::CatchUp{stats.frames_caught_up - caught_up_before, stats.frames_caught_up, config.output_format.sample_rate_hz};`.
        if (idle_resume_id_) {
            idle_resume_frames_ += stats.frames_idle_skipped - idle_before;
            if (result && !result.value().contributors.empty()) { complete_idle_resume(diagnostics); }
        }
        if (result) {
            for (const auto& header : result.value().contributors) {
                const auto parent = std::find_if(parents.begin(), parents.end(), [&](const auto& entry) {
                    return entry.dispatch.delivery.role == header.channel &&
                           entry.dispatch.message_sequence == header.sequence;
                });
                if (parent != parents.end() && step_completion_ && frozen_rejection_) {
                    parent->dispatch.write = step_write_;
                    diagnostics.correlated.at(diagnostics.correlated_count++) = parent->dispatch;
                }
            }
            for (const auto& header : result.value().contributors) {
                for (auto& job : queued_) {
                    if (job.dequeued_at && job.header.channel == header.channel &&
                        job.header.sequence == header.sequence) {
                        job.rendered_any = true;
                    }
                }
            }
            for (const auto& header : result.value().completed) {
                discard_if(queued_, [&](const Job& job) {
                    return job.header.channel == header.channel &&
                           job.header.sequence == header.sequence &&
                           std::any_of(parents.begin(), parents.end(), [&](const auto& entry) {
                               return entry.dispatch.delivery == job.id;
                           });
                });
            }
        }
        if (!stopping_) { grant_deferred(diagnostics); }
        active_.reset();
        // A failed write is survived by reopening the sink; if that fails
        // too, the latched context stays the original failed write's.
        std::function<void()> latch_render_failure;
        bool reopen = false;
        if (!result) {
            const auto snapshot = local_router.report().render_failure;
            std::optional<AudioDispatchObservation> parent;
            std::optional<WorkerAdmission> admission;
            std::size_t matches = 0;
            if (snapshot && snapshot->offending) {
                const auto& header = *snapshot->offending;
                for (const auto& entry : parents) {
                    if (entry.header.channel == header.channel &&
                        entry.header.sequence == header.sequence &&
                        entry.header.timestamp == header.timestamp) {
                        parent = entry.dispatch;
                        admission = entry.admission;
                        ++matches;
                    }
                }
            }
            if (matches != 1) {
                parent.reset();
                admission.reset();
            }
            latch_render_failure = [this, error = result.error(),
                                    rejection = local_router.report().queue_rejection,
                                    parent, snapshot, admission] {
                latch(error, AudioFailureOrigin::render_step, rejection, parent, snapshot,
                      admission);
            };
            reopen = snapshot && snapshot->reason == RenderFailureReason::sink_write_error &&
                     !stopping_ && consecutive_reopens_ < kMaxConsecutiveReopens;
            if (reopen) {
                if (!bypass_since_) { enter_bypass_locked("sink_write_error"); }
                router_ = nullptr; // no concurrent cancel_pending during reopen
                in_flight_ = true; // wait_idle() covers the reopen
            } else {
                latch_render_failure();
            }
        } else {
            if (bypass_since_) {
                // The dropped span becomes an authorized all-role idle gap:
                // the next arrival re-anchors instead of failing the gap cap.
                if (output_established_) { idle_since_ = *bypass_since_; }
                reconcile_bypass_locked(result.value());
                exit_bypass_locked("write_returned");
            }
            if (stats.frames_written > before) { consecutive_reopens_ = 0; }
            output_established_ = output_established_ || stats.frames_written != 0;
            last_success_ = WorkerSuccessfulStep{
                returned_at, monotonic_now(), result.value().pending,
                result.value().contributors.size(), result.value().completed.size(),
                queue_observation()};
            observe_idle(diagnostics);
            if (on_idle_observed_for_test) {
                on_idle_observed_for_test(role_slots_, in_flight_, idle_since_.has_value(),
                                          local_router.report());
            }
        }
        cv_.notify_all();
        publish_bypass_logs(lock);
        if (diagnostics.completion && completed) { completed(*diagnostics.completion); }
        diagnostics.publish();
        if (result && stats.frames_written > before) {
            ++chunks;
            if (chunks % 50 == 1) {
                if (telemetry_.log_media) {
                    telemetry_.log_media("audio_chunks", audio_chunks_json(chunks));
                }
                emit_media_timing(stats);
            }
        }
        if (reopen) {
            // Arrivals keep dropping and ACKing while the failed sink is
            // replaced; the new route anchors on its first arrival.
            local_router.close();
            auto reopened = open(config);
            lock.lock();
            ++consecutive_reopens_;
            in_flight_ = false;
            const bool healthy =
                reopened && reopened.value().state() != RouterState::degraded;
            if (healthy && stopping_) {
                // stop() ran while the lock was released for open(): keep the
                // new route private so shutdown below flushes and closes it,
                // and never republish it for concurrent cancellation.
                local_router = std::move(reopened).value();
                exit_bypass_locked("stopped");
            } else if (healthy) {
                local_router = std::move(reopened).value();
                router_ = &local_router;
                sink_report_ = local_router.report().sink;
                log_sink_selected(sink_report_, "reopen", telemetry_);
                output_established_ = false;
                idle_since_.reset();
                idle_resume_id_.reset();
                idle_resume_frames_ = 0;
                exit_bypass_locked("sink_reopened");
            } else {
                exit_bypass_locked("reopen_failed");
                latch_render_failure();
            }
            cv_.notify_all();
            publish_bypass_logs(lock);
        }
    }
    {
        std::lock_guard lock(mutex_);
        router_ = nullptr;
    }
    // Failed PCM must never be pumped again by flush(). close() only tears
    // down/drains the backend; healthy terminalization commits one EOS.
    if (!failure()) {
        if (auto flushed = local_router.flush(); !flushed) {
            std::lock_guard lock(mutex_);
            latch(flushed.error(), AudioFailureOrigin::flush);
        }
    }
    local_router.close();
    emit_media_timing(local_router.report().stats);
    {
        std::lock_guard lock(mutex_);
        router_ = nullptr;
        final_report_ = local_router.report();
        stopped_ = true;
        cv_.notify_all();
    }
}

} // namespace aa::audio
