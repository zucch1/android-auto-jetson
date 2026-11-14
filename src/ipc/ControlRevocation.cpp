// SPDX-License-Identifier: GPL-3.0-or-later
#include <aa/ipc/ControlService.hpp>
#include <algorithm>
#include <iterator>

namespace aa::ipc {
void ControlService::retire_failed_pairing(core::ConsumerId consumer, core::PairingRequestId request,
                                            const std::optional<helper::PeerIdentity>& peer) {
    if (!confirmation_) return;
    const auto retired = confirmation_->cancel(consumer, request);
    if (retired) return;
    // No phone id was allocated. Reuse identity blocking and acknowledged
    // retirement progress. Invalidate retires the source-bound helper lease;
    // there is no stored phone/AP approval to revoke.
    helper::RevocationProgress progress;
    progress.ap = helper::RevocationLeg::complete;
    progress.retirements.push_back({consumer, request});
    trust::TransportIdentity identity = peer ?
        trust::TransportIdentity{trust::WirelessIdentity{peer->address, peer->adapter}} :
        trust::TransportIdentity{trust::OpaqueIdentity{""}};
    revocations_.push_back({{core::PhoneId{}, std::move(identity)}, std::move(progress), false});
    events_.emit(DiagnosticEmitted{DiagnosticSeverity::error, "phone_revocation_retirement_pending", request.value});
}
void ControlService::retire_acknowledged_pairing(core::ConsumerId consumer, core::PairingRequestId request,
                                                  const std::optional<helper::PeerIdentity>& peer) {
    if (!confirmation_) return;
    if (confirmation_->reconciles()) {
        // The helper's journal holds the provisional bond and the store does not approve it:
        // retiring the source closes the helper's window (parking the bond), and the next
        // reconciliation removes it. No receiver-side bond record is needed for correctness.
        retire_failed_pairing(consumer, request, peer);
        request_reconcile();
        return;
    }
    if (!peer) { retire_failed_pairing(consumer, request, peer); return; }
    // No phone id exists and no AP approval was ever stored: only the bond and the helper's
    // source lease remain. The captured wireless identity blocks re-pairing until both are retired.
    helper::RevocationProgress progress;
    progress.ap = helper::RevocationLeg::complete;
    progress.bond = helper::RevocationLeg::pending;
    progress.retirements.push_back({consumer, request});
    revocations_.push_back({{core::PhoneId{}, trust::TransportIdentity{trust::WirelessIdentity{peer->address, peer->adapter}}},
                            std::move(progress), false});
    (void)attempt_revocation(revocations_.back());
    if (revocations_.back().progress.complete()) revocations_.pop_back();
    else {
        // The helper just failed: first retry after the one-second backoff step of the shared,
        // bounded retry loop in refresh_consumer_owner().
        revocations_.back().failures = 2;
        revocations_.back().next_attempt = now() + std::chrono::seconds{1};
    }
}
void ControlService::reconcile_bonds() {
    next_reconcile_ = now() + std::chrono::seconds{1};
    const auto approved = phones_.approved_wireless();
    if (!approved) return;  // unusable store: no authority to send; the helper stays fail-closed
    const auto result = confirmation_->reconcile_bonds(*approved, reconcile_due_);
    if (!result) {
        reconcile_due_ = true;
        if (!reconcile_failing_) events_.emit(DiagnosticEmitted{DiagnosticSeverity::warning, "bond_reconcile_pending", 0});
        reconcile_failing_ = true;
        return;
    }
    reconcile_due_ = false;
    reconcile_failing_ = false;
    const auto unadmitted = result.value() & 0xFFFFFFFFU;
    if (unadmitted && unadmitted != reconcile_unadmitted_)
        events_.emit(DiagnosticEmitted{DiagnosticSeverity::warning, "bond_reconcile_unadmitted", unadmitted});
    reconcile_unadmitted_ = unadmitted;
    // The helper's state now matches the store durably: a bond leg whose identity the store no
    // longer approves is complete (the helper demoted it and owns its removal), so such a record can
    // never pin capacity (finding 3). AP legs and source retirements stay with the retry paths.
    for (auto it = revocations_.begin(); it != revocations_.end();) {
        const auto peer = helper::revocation_peer(it->snapshot);
        if (peer && it->progress.bond == helper::RevocationLeg::pending &&
            std::none_of(approved->begin(), approved->end(),
                         [&](const helper::PeerIdentity& kept) { return phones_.same_wireless_identity(kept, *peer); }))
            it->progress.bond = helper::RevocationLeg::complete;
        if (it->progress.complete()) {
            events_.emit(DiagnosticEmitted{DiagnosticSeverity::info, "phone_revocation_external_complete", it->snapshot.phone.value});
            it = revocations_.erase(it);
        } else ++it;
    }
    source_retry_cursor_ = revocations_.empty() ? 0 : source_retry_cursor_ % revocations_.size();
}
std::optional<helper::RevocationProgress> ControlService::revocation_progress(core::PhoneId phone) const {
    for (const auto& record : revocations_) {
        if (record.snapshot.phone == phone) return record.progress;
    }
    return std::nullopt;
}
bool ControlService::revocation_blocks(const helper::PeerIdentity& peer) const {
    for (const auto& record : revocations_) {
        const auto revoked = helper::revocation_peer(record.snapshot);
        if (revoked && phones_.same_wireless_identity(*revoked, peer)) return true;
    }
    return false;
}
core::Result<void> ControlService::attempt_revocation(PendingRevocation& record) {
    helper::RevocationAttempt report;
    if (confirmation_) report = confirmation_->forgotten_v1(record.snapshot, record.progress);
    else if (record.progress.bond) report.bond = Error{ErrorCode::helper_unavailable};
    if (report.ap) record.progress.ap = helper::RevocationLeg::complete;
    if (record.progress.bond && report.bond && *report.bond)
        record.progress.bond = helper::RevocationLeg::complete;
    record.progress.retirements.erase(record.progress.retirements.begin(),
        std::next(record.progress.retirements.begin(), static_cast<std::ptrdiff_t>(report.retired)));
    const auto phone = record.snapshot.phone.value;
    if (record.progress.ap != helper::RevocationLeg::complete)
        events_.emit(DiagnosticEmitted{DiagnosticSeverity::error, "phone_revocation_ap_pending", phone});
    if (record.progress.bond == helper::RevocationLeg::pending)
        events_.emit(DiagnosticEmitted{DiagnosticSeverity::error, "phone_revocation_bond_pending", phone});
    if (!record.progress.retirements.empty())
        events_.emit(DiagnosticEmitted{DiagnosticSeverity::error, "phone_revocation_retirement_pending", phone});
    if (record.progress.complete()) {
        events_.emit(DiagnosticEmitted{DiagnosticSeverity::info, "phone_revocation_external_complete", phone});
        return {};
    }
    if (record.progress.ap != helper::RevocationLeg::complete) return report.ap.error();
    if (record.progress.bond == helper::RevocationLeg::pending)
        return report.bond ? report.bond->error() : Error{ErrorCode::helper_protocol_mismatch};
    return report.retirement.error();
}
} // namespace aa::ipc
