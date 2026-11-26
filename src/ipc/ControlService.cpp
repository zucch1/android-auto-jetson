// SPDX-License-Identifier: GPL-3.0-or-later
#include <aa/ipc/ControlService.hpp>

#include <aa/ipc/Contract.hpp>

#include <chrono>

namespace aa::ipc {

namespace {

constexpr std::uint16_t kMaxViewportDimension = 8192;
constexpr std::uint32_t kMaxViewportDpi = 2000;

} // namespace

core::Result<ControlService::Registered> ControlService::require_consumer() {
    refresh_consumer_owner();
    if (!registered_.has_value()) {
        return Error{ErrorCode::ipc_consumer_rejected};
    }
    return *registered_;
}

void ControlService::teardown(std::string_view reason, DiagnosticSeverity severity) {
    const auto consumer = registered_->consumer;
    if (confirmation_ != nullptr) {
        confirmation_->teardown(consumer);
    }
    if (projection_ == ProjectionState::projecting) {
        projection_ = ProjectionState::stopped;
        events_.emit(StateChanged{ProjectionState::stopped, consumer});
    }
    for (const auto request : gate_.close_all()) {
        phones_.pairing_cancelled(request);
        events_.emit(PairingClosed{request});
        finalize_pairing_outcome(request, "cancelled");
    }
    registered_.reset();
    open_pairing_.reset();
    viewport_.reset();
    idr_recovery_requested_.store(false);
    events_.emit(StateChanged{ProjectionState::stopped, core::ConsumerId{}});
    events_.emit(DiagnosticEmitted{severity, reason, consumer.value});
}

std::chrono::steady_clock::time_point ControlService::now() const {
    return clock_ ? clock_() : std::chrono::steady_clock::now();
}

void ControlService::refresh_consumer_owner() {
    // Reconcile first: a success resolves bond legs, so those records never reach the retry loop.
    if (confirmation_ != nullptr && confirmation_->reconciles() && now() >= next_reconcile_) reconcile_bonds();
    if (!revocations_.empty()) {
        const auto started = now();
        const auto deadline = started + kSourceRetirementTimeBudget;
        std::size_t index = source_retry_cursor_ % revocations_.size();
        std::size_t remaining = revocations_.size();
        std::size_t attempts = 0;
        while (remaining > 0 && !revocations_.empty() &&
               attempts < kSourceRetirementAttemptBudget &&
               (attempts == 0 || now() < deadline)) {
            index %= revocations_.size();
            auto& record = revocations_[index];
            const auto attempt_now = now();
            if (record.snapshot.phone || record.next_attempt > attempt_now) {
                ++index;
                --remaining;
                continue;
            }
            ++attempts;
            (void)attempt_revocation(record);
            if (record.progress.complete()) {
                revocations_.erase(revocations_.begin() + static_cast<std::ptrdiff_t>(index));
                --remaining;
            } else {
                if (record.failures < 16) ++record.failures;
                if (record.failures >= 2) {
                    const auto shift = std::min<std::uint32_t>(record.failures - 2, 5);
                    const auto delay = std::chrono::seconds{1U << shift};
                    record.next_attempt = now() + std::min(delay, std::chrono::seconds{30});
                } else {
                    record.next_attempt = now();
                }
                ++index;
                --remaining;
            }
        }
        source_retry_cursor_ = revocations_.empty() ? 0 : (index % revocations_.size());
    }
    if (!registered_.has_value()) {
        return;
    }
    if (open_pairing_ && now() >= open_pairing_->deadline) {
        phones_.pairing_cancelled(open_pairing_->request);
        events_.emit(DiagnosticEmitted{DiagnosticSeverity::warning, "pairing_expired", open_pairing_->request.value});
        const auto request = open_pairing_->request;
        const auto peer = open_pairing_->peer;
        open_pairing_.reset();
        retire_failed_pairing(registered_->consumer, request, peer);
        (void)gate_.cancel({registered_->consumer, registered_->name}, request);
        events_.emit(PairingClosed{request});
        finalize_pairing_outcome(request, "expired");
    }
    const auto owner = credentials_.name_owner(registered_->name);
    if (!owner.has_value() || !(owner.value() == registered_->connection)) {
        teardown("consumer_owner_lost", DiagnosticSeverity::warning);
    }
    if (registered_) {
        advance_enrolment();
        if (!registered_) return;
        for (const auto request : phones_.reap_expired()) {
            const auto peer = phones_.take_expired_wireless_identity(request);
            retire_failed_pairing(registered_->consumer, request, peer);
            (void)gate_.cancel({registered_->consumer, registered_->name}, request);
            if (open_pairing_ && open_pairing_->request == request) open_pairing_.reset();
            events_.emit(PairingClosed{request});
            finalize_pairing_outcome(request, "expired");
            events_.emit(DiagnosticEmitted{DiagnosticSeverity::warning, "pairing_expired", request.value});
        }
    }
}

core::Result<void> ControlService::admit(const BusName& caller, const Registered& active) {
    const auto uid = credentials_.unix_uid(caller);
    if (!uid.has_value()) {
        events_.emit(DiagnosticEmitted{DiagnosticSeverity::warning, "access_denied", 0});
        return Error{ErrorCode::ipc_peer_unauthorized};
    }
    const auto owner = credentials_.name_owner(active.name);
    if (!owner.has_value() || !(owner.value() == active.connection)) {
        teardown("consumer_owner_lost", DiagnosticSeverity::warning);
        return Error{ErrorCode::ipc_peer_unauthorized};
    }
    const bool owns_name = owner.value() == caller;
    const ConsumerIdentity caller_identity{
        owns_name && caller == active.connection ? active.consumer : core::ConsumerId{},
        uid.value()};
    const ConsumerIdentity active_identity{active.consumer, config_.session_unix_uid};
    const auto decision = aa::ipc::authorize({caller_identity, active_identity, owns_name});
    if (decision != Authorization::allowed) {
        events_.emit(DiagnosticEmitted{DiagnosticSeverity::warning, "access_denied", 0});
        return Error{ErrorCode::ipc_peer_unauthorized};
    }
    return {};
}

core::Result<core::ConsumerId> ControlService::register_consumer(const BusName& caller,
                                                                 const BusName& consumer_name) {
    const auto uid = credentials_.unix_uid(caller);
    if (!uid.has_value() || uid.value() != config_.session_unix_uid) {
        events_.emit(DiagnosticEmitted{DiagnosticSeverity::warning, "access_denied", 0});
        return Error{ErrorCode::ipc_peer_unauthorized};
    }
    const auto owner = credentials_.name_owner(consumer_name);
    if (!owner.has_value() || !(owner.value() == caller)) {
        events_.emit(DiagnosticEmitted{DiagnosticSeverity::warning, "access_denied", 0});
        return Error{ErrorCode::ipc_peer_unauthorized};
    }
    refresh_consumer_owner();
    if (registered_.has_value()) {
        events_.emit(DiagnosticEmitted{DiagnosticSeverity::warning, "consumer_rejected", 0});
        return Error{ErrorCode::ipc_consumer_rejected};
    }
    const core::ConsumerId consumer{next_consumer_id_++};
    registered_ = Registered{consumer, consumer_name, caller};
    events_.emit(StateChanged{ProjectionState::stopped, consumer});
    events_.emit(DiagnosticEmitted{DiagnosticSeverity::info, "consumer_registered", consumer.value});
    return consumer;
}

core::Result<void> ControlService::unregister_consumer(const BusName& caller) {
    const auto active = require_consumer();
    if (!active.has_value()) {
        return active.error();
    }
    const auto admitted = admit(caller, active.value());
    if (!admitted.has_value()) {
        return admitted.error();
    }
    teardown("consumer_unregistered", DiagnosticSeverity::info);
    return {};
}

core::Result<void> ControlService::start_projection(const BusName& caller) {
    const auto active = require_consumer();
    if (!active.has_value()) {
        return active.error();
    }
    const auto admitted = admit(caller, active.value());
    if (!admitted.has_value()) {
        return admitted.error();
    }
    if (projection_ == ProjectionState::projecting ||
        (projection_policy_ && (!projection_policy_->ready ||
                                (!projection_policy_->wired && !projection_policy_->wireless)))) {
        return Error{ErrorCode::session_illegal_transition};
    }
    projection_ = ProjectionState::projecting;
    events_.emit(StateChanged{ProjectionState::projecting, active.value().consumer});
    events_.emit(DiagnosticEmitted{DiagnosticSeverity::info, "projection_started",
                                   active.value().consumer.value});
    return {};
}

core::Result<void> ControlService::stop_projection(const BusName& caller) {
    const auto active = require_consumer();
    if (!active.has_value()) {
        return active.error();
    }
    const auto admitted = admit(caller, active.value());
    if (!admitted.has_value()) {
        return admitted.error();
    }
    const bool stopped_legacy = stop_legacy_projection_ && stop_legacy_projection_();
    if (projection_ == ProjectionState::stopped && !stopped_legacy) {
        return Error{ErrorCode::session_illegal_transition};
    }
    publish_projection_stop(active.value().consumer);
    return {};
}

void ControlService::publish_projection_stop(core::ConsumerId consumer) {
    projection_ = ProjectionState::stopped;
    idr_recovery_requested_.store(false);
    events_.emit(StateChanged{ProjectionState::stopped, consumer});
    events_.emit(DiagnosticEmitted{DiagnosticSeverity::info, "projection_stopped", consumer.value});
}

core::Result<void> ControlService::set_projection_mode(const BusName& caller, std::string_view mode) {
    const auto active = require_consumer();
    if (!active) return active.error();
    if (auto admitted = admit(caller, active.value()); !admitted) return admitted.error();
    if (!projection_policy_) return Error{ErrorCode::session_illegal_transition};
    if (mode != "pairing_only" && mode != "projection_ready") {
        return Error{ErrorCode::invalid_argument};
    }
    const bool ready = mode == "projection_ready";
    if (ready && !projection_policy_->ready && projection_policy_->check_ready) {
        if (auto checked = projection_policy_->check_ready(); !checked) return checked.error();
    }
    projection_policy_->ready = ready;
    if (!ready && projection_ == ProjectionState::projecting) {
        publish_projection_stop(active.value().consumer);
    }
    return {};
}

core::Result<void> ControlService::set_transport_enabled(const BusName& caller,
                                                         std::string_view transport, bool enabled) {
    const auto active = require_consumer();
    if (!active) return active.error();
    if (auto admitted = admit(caller, active.value()); !admitted) return admitted.error();
    if (!projection_policy_) return Error{ErrorCode::session_illegal_transition};
    ProjectionTransport changed;
    if (transport == "wired") {
        projection_policy_->wired = enabled;
        changed = ProjectionTransport::wired;
    } else if (transport == "wireless") {
        projection_policy_->wireless = enabled;
        changed = ProjectionTransport::wireless;
    } else {
        return Error{ErrorCode::invalid_argument};
    }
    if (!enabled && projection_ == ProjectionState::projecting &&
        (projection_policy_->active_transport == changed ||
         (!projection_policy_->wired && !projection_policy_->wireless))) {
        publish_projection_stop(active.value().consumer);
    }
    return {};
}

void ControlService::projection_ended() {
    if (registered_ && projection_ == ProjectionState::projecting) {
        publish_projection_stop(registered_->consumer);
    }
}

core::Result<void> ControlService::request_idr_recovery(const BusName& caller) {
    const auto active = require_consumer();
    if (!active.has_value()) {
        return active.error();
    }
    const auto admitted = admit(caller, active.value());
    if (!admitted.has_value()) {
        return admitted.error();
    }
    if (projection_ != ProjectionState::projecting) {
        return Error{ErrorCode::session_illegal_transition};
    }
    if (session_active_ && !session_active_()) {
        return Error{ErrorCode::session_illegal_transition};
    }
    idr_recovery_requested_.store(true);
    events_.emit(DiagnosticEmitted{DiagnosticSeverity::info, "idr_recovery_requested",
                                   active.value().consumer.value});
    return {};
}

StateSnapshot ControlService::state() {
    refresh_consumer_owner();
    return StateSnapshot{projection_,
                         registered_.has_value() ? registered_->consumer : core::ConsumerId{}};
}

std::map<std::string, std::string> ControlService::capabilities() const {
    std::map<std::string, std::string> result{
        {"contract.interface", std::string{kContractInterface}},
        {"contract.semver", std::string{contract_semver()}},
        {"consumer.exclusive", "true"},
        {"pairing.one_time_request_ids", "true"},
        {"pairing.outcomes", "true"},
        {"phones.list", "true"},
        {"display.viewport", "true"},
        {"diagnostics.events", "true"},
    };
    if (projection_policy_) {
        result.emplace("projection.mode", "true");
        result.emplace("projection.mode.current", projection_policy_->ready ? "projection_ready" : "pairing_only");
        result.emplace("transport.control", "true");
        result.emplace("transport.wired.enabled", projection_policy_->wired ? "true" : "false");
        result.emplace("transport.wireless.enabled", projection_policy_->wireless ? "true" : "false");
    }
    return result;
}

core::Result<std::vector<std::map<std::string, std::string>>> ControlService::list_phones(
    const BusName& caller) {
    const auto active = require_consumer();
    if (!active.has_value()) {
        return active.error();
    }
    const auto admitted = admit(caller, active.value());
    if (!admitted.has_value()) {
        return admitted.error();
    }
    return phones_.list_phones();
}

core::Result<std::string> ControlService::pairing_outcome(const BusName& caller,
                                                          core::PairingRequestId request) {
    const auto active = require_consumer();
    if (!active.has_value()) {
        return active.error();
    }
    const auto admitted = admit(caller, active.value());
    if (!admitted.has_value()) {
        return admitted.error();
    }
    if (!request) {
        return Error{ErrorCode::ipc_malformed_request};
    }
    for (const auto& record : pairing_outcomes_) {
        if (record.request != request) {
            continue;
        }
        if (record.consumer != active.value().consumer) {
            return Error{ErrorCode::ipc_peer_unauthorized};
        }
        return record.outcome;
    }
    return Error{ErrorCode::ipc_malformed_request};
}

void ControlService::record_pairing_pending(core::PairingRequestId request,
                                            core::ConsumerId consumer) {
    if (pairing_outcomes_.size() >= PairingGate::kIssuedBudget) {
        pairing_outcomes_.erase(pairing_outcomes_.begin());
    }
    pairing_outcomes_.push_back(PairingOutcomeRecord{request, consumer, "pending", false});
}

void ControlService::finalize_pairing_outcome(core::PairingRequestId request,
                                              std::string_view outcome) {
    for (auto& record : pairing_outcomes_) {
        if (record.request != request) {
            continue;
        }
        if (record.terminal) {
            return;
        }
        record.outcome = std::string{outcome};
        record.terminal = true;
        events_.emit(PairingCompleted{request, record.outcome});
        return;
    }
}

std::uint64_t ControlService::ping() const noexcept {
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
}

core::Result<void> ControlService::set_display_viewport(const BusName& caller, Viewport viewport) {
    const auto active = require_consumer();
    if (!active.has_value()) {
        return active.error();
    }
    const auto admitted = admit(caller, active.value());
    if (!admitted.has_value()) {
        return admitted.error();
    }
    if (viewport.width == 0 || viewport.width > kMaxViewportDimension ||
        viewport.height == 0 || viewport.height > kMaxViewportDimension ||
        viewport.dpi == 0 || viewport.dpi > kMaxViewportDpi) {
        return Error{ErrorCode::invalid_argument};
    }
    viewport_ = viewport;
    events_.emit(DiagnosticEmitted{DiagnosticSeverity::info, "viewport_set", 0});
    return {};
}

} // namespace aa::ipc
