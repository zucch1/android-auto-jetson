// SPDX-License-Identifier: GPL-3.0-or-later
#include <aa/ipc/ControlService.hpp>
#include <algorithm>
#include <iterator>

namespace aa::ipc {
void ControlService::retire_acknowledged_pairing(core::ConsumerId consumer, core::PairingRequestId request,
                                                  const std::optional<helper::PeerIdentity>& peer) {
    if (!confirmation_) return;
    if (!peer) { confirmation_->cancel(consumer, request); return; }
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
    else revocations_.back().next_attempt = std::chrono::steady_clock::now() + std::chrono::seconds(2);
}
// Enrolment retirements have no phone id, so ForgetPhone can never retry them: retry on owner
// reconciliation (control calls and idle cycles), paced so a failing helper is not hammered.
void ControlService::retry_enrolment_retirements() {
    if (std::none_of(revocations_.begin(), revocations_.end(), [](const auto& record) { return !record.snapshot.phone; })) return;
    const auto now = std::chrono::steady_clock::now();
    for (auto it = revocations_.begin(); it != revocations_.end();) {
        if (it->snapshot.phone || now < it->next_attempt) { ++it; continue; }
        (void)attempt_revocation(*it);
        if (it->progress.complete()) { it = revocations_.erase(it); continue; }
        it->next_attempt = now + std::chrono::seconds(2);
        ++it;
    }
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
