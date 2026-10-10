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
    if (projection_ == ProjectionState::projecting) {
        projection_ = ProjectionState::stopped;
        events_.emit(StateChanged{ProjectionState::stopped, consumer});
    }
    for (const auto request : gate_.close_all()) {
        events_.emit(PairingClosed{request});
    }
    registered_.reset();
    viewport_.reset();
    events_.emit(StateChanged{ProjectionState::stopped, core::ConsumerId{}});
    events_.emit(DiagnosticEmitted{severity, reason, consumer.value});
}

void ControlService::refresh_consumer_owner() {
    if (!registered_.has_value()) {
        return;
    }
    const auto owner = credentials_.name_owner(registered_->name);
    if (!owner.has_value() || !(owner.value() == registered_->connection)) {
        teardown("consumer_owner_lost", DiagnosticSeverity::warning);
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
    if (projection_ == ProjectionState::projecting) {
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
    if (projection_ == ProjectionState::stopped) {
        return Error{ErrorCode::session_illegal_transition};
    }
    projection_ = ProjectionState::stopped;
    events_.emit(StateChanged{ProjectionState::stopped, active.value().consumer});
    events_.emit(DiagnosticEmitted{DiagnosticSeverity::info, "projection_stopped",
                                   active.value().consumer.value});
    return {};
}

core::Result<core::PairingRequestId> ControlService::request_add_phone(const BusName& caller) {
    const auto active = require_consumer();
    if (!active.has_value()) {
        return active.error();
    }
    const auto admitted = admit(caller, active.value());
    if (!admitted.has_value()) {
        return admitted.error();
    }
    const auto request = gate_.request({active.value().consumer, active.value().name});
    if (!request.has_value()) {
        events_.emit(DiagnosticEmitted{DiagnosticSeverity::warning, "pairing_request_denied", 0});
        return request.error();
    }
    events_.emit(PairingRequested{request.value()});
    events_.emit(DiagnosticEmitted{DiagnosticSeverity::info, "pairing_requested",
                                   request.value().value});
    return request;
}

core::Result<void> ControlService::confirm_phone_pairing(const BusName& caller,
                                                         core::PairingRequestId request) {
    const auto active = require_consumer();
    if (!active.has_value()) {
        return active.error();
    }
    const auto admitted = admit(caller, active.value());
    if (!admitted.has_value()) {
        return admitted.error();
    }
    const auto opened =
        gate_.confirm({active.value().consumer, active.value().name}, request);
    if (!opened.has_value()) {
        events_.emit(
            DiagnosticEmitted{DiagnosticSeverity::warning, "pairing_request_denied", request.value});
        return opened.error();
    }
    events_.emit(PairingOpened{request});
    events_.emit(
        DiagnosticEmitted{DiagnosticSeverity::info, "pairing_opened", request.value});
    return {};
}

core::Result<void> ControlService::cancel_phone_pairing(const BusName& caller,
                                                        core::PairingRequestId request) {
    const auto active = require_consumer();
    if (!active.has_value()) {
        return active.error();
    }
    const auto admitted = admit(caller, active.value());
    if (!admitted.has_value()) {
        return admitted.error();
    }
    const auto closed = gate_.cancel({active.value().consumer, active.value().name}, request);
    if (!closed.has_value()) {
        events_.emit(
            DiagnosticEmitted{DiagnosticSeverity::warning, "pairing_request_denied", request.value});
        return closed.error();
    }
    events_.emit(PairingClosed{request});
    events_.emit(DiagnosticEmitted{DiagnosticSeverity::info, "pairing_closed", request.value});
    return {};
}

core::Result<void> ControlService::forget_phone(const BusName& caller, core::PhoneId phone) {
    const auto active = require_consumer();
    if (!active.has_value()) {
        return active.error();
    }
    const auto admitted = admit(caller, active.value());
    if (!admitted.has_value()) {
        return admitted.error();
    }
    if (!phone) {
        return Error{ErrorCode::invalid_argument};
    }
    const auto forgotten = phones_.forget(phone);
    if (!forgotten.has_value()) {
        return forgotten.error();
    }
    events_.emit(DiagnosticEmitted{DiagnosticSeverity::info, "phone_forgotten", phone.value});
    return {};
}

StateSnapshot ControlService::state() {
    refresh_consumer_owner();
    return StateSnapshot{projection_,
                         registered_.has_value() ? registered_->consumer : core::ConsumerId{}};
}

std::map<std::string, std::string> ControlService::capabilities() const {
    return {
        {"contract.interface", std::string{kContractInterface}},
        {"contract.semver", std::string{contract_semver()}},
        {"consumer.exclusive", "true"},
        {"pairing.one_time_request_ids", "true"},
        {"display.viewport", "true"},
        {"diagnostics.events", "true"},
    };
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
