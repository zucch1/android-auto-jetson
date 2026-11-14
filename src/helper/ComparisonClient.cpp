// SPDX-License-Identifier: GPL-3.0-or-later
#include "BusClientImpl.hpp"

namespace aa::helper {
core::Result<bool> BusClient::bond_confirmed(core::ConsumerId generation, core::PairingRequestId request) {
    const auto until = Clock::now() + std::chrono::milliseconds(1000);
    std::unique_lock lock{impl_->mutex, std::defer_lock};
    if (!lock.try_lock_until(until)) return Error{ErrorCode::timeout};
    MessageWriter writer; writer.put_u64(generation.value).put_u64(request.value);
    const auto result = impl_->rpc("PairingBondConfirmedV1", "tt", writer, until);
    if (!result) return result.error();
    if (result.value() > 1) return Error{ErrorCode::helper_protocol_mismatch};
    return result.value() == 1;
}
core::Result<void> BusClient::bond_acknowledged(core::ConsumerId generation, core::PairingRequestId request) {
    const auto until = Clock::now() + std::chrono::milliseconds(1000);
    std::unique_lock lock{impl_->mutex, std::defer_lock};
    if (!lock.try_lock_until(until)) return Error{ErrorCode::timeout};
    MessageWriter writer; writer.put_u64(generation.value).put_u64(request.value);
    const auto result = impl_->rpc("PairingBondAcknowledgedV1", "tt", writer, until);
    return result ? core::Result<void>{} : core::Result<void>{result.error()};
}
core::Result<void> BusClient::bond_committed(core::ConsumerId generation, core::PairingRequestId request) {
    const auto until = Clock::now() + std::chrono::milliseconds(1000);
    std::unique_lock lock{impl_->mutex, std::defer_lock};
    if (!lock.try_lock_until(until)) return Error{ErrorCode::timeout};
    MessageWriter writer; writer.put_u64(generation.value).put_u64(request.value);
    const auto result = impl_->rpc("PairingBondCommittedV1", "tt", writer, until);
    return result ? core::Result<void>{} : core::Result<void>{result.error()};
}
core::Result<PairingComparison> BusClient::comparison(core::ConsumerId generation, core::PairingRequestId request) {
    const auto until = Clock::now() + std::chrono::milliseconds(1000);
    std::unique_lock lock{impl_->mutex, std::defer_lock};
    if (!lock.try_lock_until(until)) return Error{ErrorCode::timeout};
    MessageWriter writer; writer.put_u64(generation.value).put_u64(request.value);
    PairingComparison value;
    const auto result = impl_->rpc("GetPairingComparisonV1", "tt", writer, until, nullptr, nullptr, nullptr, &value);
    if (!result) return result.error();
    if (!value.id) return Error{ErrorCode::helper_rejected};
    return value;
}
core::Result<void> BusClient::respond_comparison(core::ConsumerId generation, core::PairingRequestId request,
                                                ComparisonResponse response) {
    const auto until = Clock::now() + std::chrono::milliseconds(1000);
    std::unique_lock lock{impl_->mutex, std::defer_lock};
    if (!lock.try_lock_until(until)) return Error{ErrorCode::timeout};
    MessageWriter writer;
    writer.put_u64(generation.value).put_u64(request.value).put_u64(response.id).put_u32(response.accept ? 1U : 0U);
    const auto result = impl_->rpc("RespondPairingComparisonV1", "tttu", writer, until);
    return result ? core::Result<void>{} : core::Result<void>{result.error()};
}
std::vector<PeerIdentity> BusClient::approved_bonds(const trust::ApprovedPhoneStore& store) {
    std::vector<PeerIdentity> approved;
    for (const auto& phone : store.phones())
        if (const auto* wireless = std::get_if<trust::WirelessIdentity>(&phone.identity))
            approved.push_back({wireless->bond_address, wireless->adapter});
    return approved;
}
// Short lock wait: this runs from the receiver's paced refresh and must not stall control calls
// behind an in-flight helper transaction; a miss is retried on the next pacing step.
core::Result<std::uint64_t> BusClient::reconcile_bonds(const std::vector<PeerIdentity>& approved, bool force) {
    const auto until = Clock::now() + std::chrono::milliseconds(500);
    std::unique_lock lock{impl_->mutex, std::defer_lock};
    if (!lock.try_lock_until(std::min(until, Clock::now() + std::chrono::milliseconds(100)))) return Error{ErrorCode::timeout};
    return impl_->reconcile(approved, force, until);
}
}
