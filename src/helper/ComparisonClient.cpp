// SPDX-License-Identifier: GPL-3.0-or-later
#include "BusClientImpl.hpp"

namespace aa::helper {
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
}
