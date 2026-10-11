// SPDX-License-Identifier: GPL-3.0-or-later
#include <aa/trust/PairingPolicy.hpp>

#include <chrono>

namespace aa::trust {

core::Nanoseconds SteadyClock::now() const noexcept {
    const auto ticks = std::chrono::steady_clock::now().time_since_epoch();
    return core::Nanoseconds{
        std::chrono::duration_cast<std::chrono::nanoseconds>(ticks).count()};
}

void PairingPolicy::note_candidate(TransportIdentity identity) {
    candidate_ = std::move(identity);
}

std::size_t PairingPolicy::find(core::PairingRequestId request) const {
    for (std::size_t index = 0; index < pending_.size(); ++index) {
        if (pending_[index].request == request) {
            return index;
        }
    }
    return pending_.size();
}

void PairingPolicy::drop_at(std::size_t index) {
    for (std::size_t tail = index + 1; tail < pending_.size(); ++tail) {
        pending_[tail - 1] = std::move(pending_[tail]);
    }
    pending_.pop_back();
}

bool PairingPolicy::has_pending(core::PairingRequestId request) const {
    return find(request) < pending_.size();
}

std::vector<core::PairingRequestId> PairingPolicy::reap_expired() {
    std::vector<core::PairingRequestId> expired;
    const core::Nanoseconds now = clock_.now();
    for (std::size_t index = 0; index < pending_.size();) {
        if (now.count > pending_[index].deadline.count) {
            expired.push_back(pending_[index].request);
            drop_at(index);  // rejected and never persisted
            continue;
        }
        ++index;
    }
    return expired;
}

core::Result<void> PairingPolicy::forget(core::PhoneId phone) {
    return store_.forget_phone(phone);
}

core::Result<void> PairingPolicy::pairing_requested(core::PairingRequestId request) {
    // Expiry is swept exclusively via reap_expired() so every dropped id is
    // propagated to the caller (gate release + PairingClosed) instead of being
    // discarded here behind the caller's back.
    if (!request) {
        return Error{ErrorCode::invalid_argument};
    }
    if (find(request) < pending_.size()) {
        return Error{ErrorCode::trust_rejected};  // one binding per one-time id
    }
    if (!candidate_.has_value()) {
        // Fail closed: a request with no presented phone approves nothing.
        return Error{ErrorCode::trust_unknown_phone};
    }
    if (pending_.size() >= kMaxPending) {
        return Error{ErrorCode::ipc_queue_full};
    }
    const core::Nanoseconds now = clock_.now();
    pending_.push_back(Pending{
        request, *candidate_,
        core::Nanoseconds{now.count + core::to_nanoseconds(kConfirmWindow).count}});
    return {};
}

core::Result<core::PhoneId> PairingPolicy::pairing_confirmed(core::PairingRequestId request) {
    const std::size_t index = find(request);
    if (index >= pending_.size()) {
        return Error{ErrorCode::trust_unknown_phone};
    }
    const core::Nanoseconds now = clock_.now();
    if (now.count > pending_[index].deadline.count) {
        drop_at(index);  // rejected and never persisted; the caller owns this
                         // id's gate rollback and PairingClosed
        return Error{ErrorCode::session_pairing_timeout};
    }
    // The identity bound at request time is what gets approved; the candidate
    // slot may have changed since and is never consulted here (no TOCTOU).
    const TransportIdentity identity = std::move(pending_[index].identity);
    drop_at(index);
    return store_.approve(identity);
}

void PairingPolicy::pairing_cancelled(core::PairingRequestId request) {
    const std::size_t index = find(request);
    if (index < pending_.size()) {
        drop_at(index);  // cancelled pairings never reach the store
    }
}

} // namespace aa::trust
