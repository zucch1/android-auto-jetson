// SPDX-License-Identifier: GPL-3.0-or-later
#include <aa/ipc/ControlService.hpp>
#include <algorithm>

namespace aa::ipc {
core::Result<core::PairingRequestId> ControlService::request_add_phone(const BusName& caller) {
    const auto active = require_consumer();
    if (!active) return active.error();
    const auto admitted = admit(caller, active.value());
    if (!admitted) return admitted.error();
    // Reap before allocation so expired requests release their admission slots.
    for (const auto expired : phones_.reap_expired()) {
        if (confirmation_ != nullptr) confirmation_->cancel(active.value().consumer, expired);
        (void)gate_.cancel({active.value().consumer, active.value().name}, expired);
        events_.emit(PairingClosed{expired});
    }
    const auto request = gate_.request({active.value().consumer, active.value().name});
    if (!request) {
        events_.emit(DiagnosticEmitted{DiagnosticSeverity::warning, "pairing_request_denied", 0});
        return request.error();
    }
    if (confirmation_ != nullptr) {
        const auto peer = confirmation_->prepare(active.value().consumer, request.value());
        core::Result<void> noted = peer ? core::Result<void>{} : core::Result<void>{peer.error()};
        if (peer && peer.value()) {
            noted = revocation_blocks(*peer.value()) ? core::Result<void>{Error{ErrorCode::trust_rejected}} :
                                                     phones_.note_wireless_candidate(*peer.value());
        }
        if (!noted) {
            confirmation_->cancel(active.value().consumer, request.value());
            (void)gate_.cancel({active.value().consumer, active.value().name}, request.value());
            return noted.error();
        }
    }
    const auto bound = phones_.pairing_requested(request.value());
    if (!bound) {
        (void)gate_.cancel({active.value().consumer, active.value().name}, request.value());
        if (confirmation_ != nullptr) confirmation_->cancel(active.value().consumer, request.value());
        events_.emit(DiagnosticEmitted{DiagnosticSeverity::warning, "pairing_request_denied", 0});
        return bound.error();
    }
    const auto captured_peer = phones_.pairing_wireless_identity(request.value());
    if (captured_peer && revocation_blocks(*captured_peer)) {
        phones_.pairing_cancelled(request.value());
        (void)gate_.cancel({active.value().consumer, active.value().name}, request.value());
        if (confirmation_) confirmation_->cancel(active.value().consumer, request.value());
        return Error{ErrorCode::trust_rejected};
    }
    events_.emit(PairingRequested{request.value()});
    events_.emit(DiagnosticEmitted{DiagnosticSeverity::info, "pairing_requested", request.value().value});
    return request;
}

core::Result<void> ControlService::confirm_phone_pairing(const BusName& caller, core::PairingRequestId request) {
    const auto active = require_consumer();
    if (!active) return active.error();
    const auto admitted = admit(caller, active.value());
    if (!admitted) return admitted.error();
    const auto opened = gate_.confirm({active.value().consumer, active.value().name}, request);
    if (!opened) {
        events_.emit(DiagnosticEmitted{DiagnosticSeverity::warning, "pairing_request_denied", request.value});
        return opened.error();
    }
    const auto approved = phones_.pairing_confirmed(request);
    if (!approved) {
        if (confirmation_ != nullptr) confirmation_->cancel(active.value().consumer, request);
        (void)gate_.cancel({active.value().consumer, active.value().name}, request);
        events_.emit(PairingClosed{request});
        events_.emit(DiagnosticEmitted{DiagnosticSeverity::warning, "pairing_request_denied", request.value});
        return approved.error();
    }
    if (confirmation_ != nullptr) {
        const auto granted = confirmation_->confirmed(active.value().consumer, request);
        if (!granted) {
            // Approval may already be committed; helper failure is not a store rollback.
            confirmation_->cancel(active.value().consumer, request);
            (void)gate_.cancel({active.value().consumer, active.value().name}, request);
            events_.emit(PairingClosed{request});
            return granted.error();
        }
    }
    open_pairing_ = OpenPairing{request, approved.value(), phones_.wireless_identity(approved.value()),
                               std::chrono::steady_clock::now() + std::chrono::seconds(60)};
    events_.emit(PairingOpened{request});
    events_.emit(DiagnosticEmitted{DiagnosticSeverity::info, "pairing_opened", request.value});
    return {};
}

core::Result<void> ControlService::cancel_phone_pairing(const BusName& caller, core::PairingRequestId request) {
    const auto active = require_consumer();
    if (!active) return active.error();
    const auto admitted = admit(caller, active.value());
    if (!admitted) return admitted.error();
    const auto closed = gate_.cancel({active.value().consumer, active.value().name}, request);
    if (!closed) {
        events_.emit(DiagnosticEmitted{DiagnosticSeverity::warning, "pairing_request_denied", request.value});
        return closed.error();
    }
    phones_.pairing_cancelled(request);
    if (open_pairing_ && open_pairing_->request == request) open_pairing_.reset();
    if (confirmation_ != nullptr) confirmation_->cancel(active.value().consumer, request);
    events_.emit(PairingClosed{request});
    events_.emit(DiagnosticEmitted{DiagnosticSeverity::info, "pairing_closed", request.value});
    return {};
}

core::Result<void> ControlService::forget_phone(const BusName& caller, core::PhoneId phone) {
    const auto active = require_consumer();
    if (!active) return active.error();
    const auto admitted = admit(caller, active.value());
    if (!admitted) return admitted.error();
    if (!phone) return Error{ErrorCode::invalid_argument};
    for (auto it = revocations_.begin(); it != revocations_.end(); ++it) {
        if (it->snapshot.phone != phone) continue;
        const auto retried = attempt_revocation(*it);
        const bool not_durable = it->not_durable;
        if (it->progress.complete()) revocations_.erase(it);
        return not_durable ? core::Result<void>{Error{ErrorCode::trust_store_not_durable}} : retried;
    }
    if (revocations_.size() >= 256) return Error{ErrorCode::ipc_queue_full};
    const auto forgotten = phones_.forget(phone);
    const bool landed = forgotten || forgotten.error().code() == ErrorCode::trust_store_not_durable;
    if (!landed) return forgotten.error();
    const auto snapshot = phones_.take_revocation_v1();
    const auto peer = snapshot ? helper::revocation_peer(*snapshot) : std::nullopt;
    auto closed = phones_.take_invalidated_pairings();
    if (open_pairing_ && (open_pairing_->phone == phone || (peer && open_pairing_->peer == peer))) {
        if (std::find(closed.begin(), closed.end(), open_pairing_->request) == closed.end())
            closed.push_back(open_pairing_->request);
        open_pairing_.reset();
    }
    std::sort(closed.begin(), closed.end(), [](auto a, auto b) { return a.value < b.value; });
    closed.erase(std::unique(closed.begin(), closed.end()), closed.end());
    for (const auto request : closed) {
        (void)gate_.cancel({active.value().consumer, active.value().name}, request);
        events_.emit(PairingClosed{request});
    }
    core::Result<void> external;
    {
        if (!snapshot || snapshot->phone != phone) {
            events_.emit(DiagnosticEmitted{DiagnosticSeverity::error, "phone_revocation_snapshot_missing", phone.value});
            external = Error{ErrorCode::helper_protocol_mismatch};
        } else {
            helper::RevocationProgress progress;
            if (peer) progress.bond = helper::RevocationLeg::pending;
            progress.retirements.reserve(closed.size());
            for (const auto request : closed) progress.retirements.push_back({active.value().consumer, request});
            revocations_.push_back({*snapshot, progress, !forgotten.has_value()});
            external = attempt_revocation(revocations_.back());
            if (revocations_.back().progress.complete()) revocations_.pop_back();
        }
    }
    if (!forgotten) return forgotten.error();
    if (!external) return external.error();
    events_.emit(DiagnosticEmitted{DiagnosticSeverity::info, "phone_forgotten", phone.value});
    return {};
}
}
