// SPDX-License-Identifier: GPL-3.0-or-later
#include <aa/ipc/ControlService.hpp>

namespace aa::ipc {
// bond_recorded: the helper may already manage the bond (acknowledgement attempted), so the
// closure retires it through the tracked revocation instead of the plain source cancellation.
void ControlService::close_enrolment(std::string_view reason, bool bond_recorded) {
    if (!open_pairing_ || !registered_) return;
    const auto request = open_pairing_->request;
    phones_.pairing_cancelled(request);
    if (confirmation_) {
        if (bond_recorded) retire_acknowledged_pairing(registered_->consumer, request, open_pairing_->peer);
        else retire_failed_pairing(registered_->consumer, request, open_pairing_->peer);
    }
    (void)gate_.cancel({registered_->consumer, registered_->name}, request);
    open_pairing_.reset();
    events_.emit(PairingClosed{request});
    events_.emit(DiagnosticEmitted{DiagnosticSeverity::warning, reason, request.value});
}
// Deadline, owner and store/snapshot reconciliation. Before the helper is asked to acknowledge
// the bond a failure only closes the enrolment (the helper has recorded nothing); afterwards the
// bond is the helper's managed bond and a failure retires it through tracked revocation.
bool ControlService::reconcile_enrolment(core::PairingRequestId request, bool bond_recorded) {
    if (!open_pairing_ || !registered_ || open_pairing_->request != request) return false;
    if (now() >= open_pairing_->deadline) {
        close_enrolment("pairing_expired", bond_recorded); return false;
    }
    // The policy's own deadline/binding predicate, shared with confirm_phone_pairing and the
    // final approval, so the enrolment cannot outlive the request it was opened for.
    if (!phones_.validate_pairing(request)) {
        close_enrolment("pairing_expired", bond_recorded); return false;
    }
    const auto owner = credentials_.name_owner(registered_->name);
    if (!owner || owner.value() != registered_->connection) {
        if (bond_recorded && confirmation_)
            retire_acknowledged_pairing(registered_->consumer, request, open_pairing_->peer);
        teardown("consumer_owner_lost", DiagnosticSeverity::warning); return false;
    }
    if (phones_.pairing_wireless_identity(request) != open_pairing_->peer) {
        close_enrolment("pairing_snapshot_invalidated", bond_recorded); return false;
    }
    return true;
}
void ControlService::advance_enrolment() {
    if (!open_pairing_ || !open_pairing_->awaiting_acceptance || !registered_) return;
    const auto request = open_pairing_->request;
    if (phones_.pairing_wireless_identity(request) != open_pairing_->peer) {
        close_enrolment("pairing_snapshot_invalidated"); return;
    }
    const auto bonded = confirmation_->bond_confirmed(registered_->consumer, request);
    if (!bonded) { close_enrolment("pairing_helper_failed"); return; }
    if (!bonded.value() || !open_pairing_->accepted) return;
    // Verification recorded nothing in the helper: run every receiver-side check first.
    if (!reconcile_enrolment(request, false)) return;
    const auto acknowledged = confirmation_->bond_acknowledged(registered_->consumer, request);
    // A failed or lost acknowledgement may still have recorded the bond: retire it with tracking.
    if (!acknowledged) { close_enrolment("pairing_helper_failed", true); return; }
    // The acknowledgement RPC may dispatch owner/store changes. Reconcile before the only write.
    if (!reconcile_enrolment(request, true)) return;
    bool durable = true;
    const auto approved = persist_pairing(*registered_, request, &durable);
    if (!approved) { open_pairing_.reset(); return; }
    // Second, explicit step: only a durable trust write is committed to the helper, which admits
    // the bond (services, reconnect) and drops its cleanup obligation only now. A failed or lost
    // commit, or a write that is not durable, retracts the approval: the bond is not admitted, so
    // the phone must not stay approved (tracked revocation; the helper removes the bond).
    const auto phone = open_pairing_ ? open_pairing_->phone : core::PhoneId{};
    const auto committed = durable && phone ? confirmation_->bond_committed(registered_->consumer, request) :
                                              core::Result<void>{Error{ErrorCode::trust_store_not_durable}};
    if (committed || !registered_) return;
    events_.emit(DiagnosticEmitted{DiagnosticSeverity::warning, "pairing_commit_failed", request.value});
    const auto active = *registered_;
    if (durable && confirmation_->reconciles()) {
        // The approval is durable and is the authority: it stays. The enrolment ends here; retiring
        // its source closes the helper's window, which parks the provisional bond, and the next
        // reconciliation admits it (or finds the commit landed and only its reply was lost).
        const auto peer = open_pairing_ ? open_pairing_->peer : std::nullopt;
        if (open_pairing_ && open_pairing_->request == request) open_pairing_.reset();
        retire_failed_pairing(active.consumer, request, peer);
        (void)gate_.cancel({active.consumer, active.name}, request);
        events_.emit(PairingClosed{request});
        request_reconcile();
        return;
    }
    // A non-durable (703) approval is retracted: the store forget and the tracked revocation, then
    // reconciliation. If the retraction cannot be written either, the approval remains in the store
    // and reconciliation keeps the helper consistent with it.
    if (phone) (void)revoke_phone(active, phone);
    request_reconcile();
}

core::Result<std::map<std::string, std::string>> ControlService::pairing_comparison(
    const BusName& caller, core::PairingRequestId request) {
    const auto active = require_consumer();
    if (!active) return active.error();
    const auto admitted = admit(caller, active.value());
    if (!admitted) return admitted.error();
    if (!confirmation_ || !open_pairing_ || open_pairing_->request != request || !open_pairing_->peer ||
        now() >= open_pairing_->deadline) return Error{ErrorCode::trust_rejected};
    const auto value = confirmation_->comparison(active.value().consumer, request);
    if (!value) {
        if (value.error().code() != ErrorCode::helper_rejected) close_enrolment("pairing_helper_failed");
        return value.error();
    }
    if (!value.value().id || value.value().number > 999999 || value.value().peer != *open_pairing_->peer ||
        phones_.pairing_wireless_identity(request) != open_pairing_->peer ||
        now() >= open_pairing_->deadline) {
        close_enrolment("pairing_comparison_failed");
        return Error{ErrorCode::trust_rejected};
    }
    const auto owner = require_consumer();
    if (!owner || owner.value().consumer != active.value().consumer || !admit(caller, owner.value()))
        return Error{ErrorCode::ipc_peer_unauthorized};
    if (!open_pairing_ || open_pairing_->request != request || !open_pairing_->peer ||
        value.value().peer != *open_pairing_->peer ||
        phones_.pairing_wireless_identity(request) != open_pairing_->peer ||
        now() >= open_pairing_->deadline)
        return Error{ErrorCode::trust_rejected};
    open_pairing_->presented_comparison = value.value().id;
    const auto number = std::to_string(value.value().number);
    return std::map<std::string, std::string>{{"challenge_id", std::to_string(value.value().id)},
        {"comparison", std::string(6 - number.size(), '0') + number},
        {"address", value.value().peer.address}, {"adapter", value.value().peer.adapter}};
}
core::Result<void> ControlService::respond_pairing_comparison(const BusName& caller,
    core::PairingRequestId request, helper::ComparisonResponse response) {
    const auto active = require_consumer();
    if (!active) return active.error();
    const auto admitted = admit(caller, active.value());
    if (!admitted) return admitted.error();
    if (!confirmation_ || !open_pairing_ || open_pairing_->request != request || !response.id ||
        open_pairing_->presented_comparison != response.id || !open_pairing_->peer ||
        phones_.pairing_wireless_identity(request) != open_pairing_->peer ||
        now() >= open_pairing_->deadline) return Error{ErrorCode::trust_rejected};
    open_pairing_->presented_comparison.reset();
    const auto current = confirmation_->comparison(active.value().consumer, request);
    if (!current || current.value().id != response.id || current.value().peer != *open_pairing_->peer ||
        now() >= open_pairing_->deadline) {
        close_enrolment("pairing_comparison_failed");
        return Error{ErrorCode::trust_rejected};
    }
    // Reconcile consumer ownership again after the synchronous helper round trip.
    const auto owner = require_consumer();
    if (!owner || owner.value().consumer != active.value().consumer || !admit(caller, owner.value()))
        return Error{ErrorCode::ipc_peer_unauthorized};
    if (!open_pairing_ || open_pairing_->request != request || !open_pairing_->peer ||
        current.value().peer != *open_pairing_->peer ||
        phones_.pairing_wireless_identity(request) != open_pairing_->peer ||
        now() >= open_pairing_->deadline) {
        close_enrolment("pairing_comparison_failed");
        return Error{ErrorCode::trust_rejected};
    }
    const auto answered = confirmation_->respond_comparison(active.value().consumer, request, response);
    if (!answered || !response.accept) {
        close_enrolment(answered ? "pairing_comparison_rejected" : "pairing_helper_failed");
        return answered;
    }
    open_pairing_->accepted = true;
    advance_enrolment();
    return {};

}
}
