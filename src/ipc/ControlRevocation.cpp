// SPDX-License-Identifier: GPL-3.0-or-later
#include <aa/ipc/ControlService.hpp>
#include <iterator>

namespace aa::ipc {
std::optional<helper::RevocationProgress> ControlService::revocation_progress(core::PhoneId phone) const {
    for (const auto& record : revocations_) {
        if (record.snapshot.phone == phone) return record.progress;
    }
    return std::nullopt;
}
bool ControlService::revocation_blocks(const helper::PeerIdentity& peer) const {
    for (const auto& record : revocations_) {
        if (helper::revocation_peer(record.snapshot) == peer) return true;
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
