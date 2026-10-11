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
    // The revocation invalidates every outstanding request bound to the SAME
    // canonical identity. Matching is by identity_key (never variant equality
    // or phone id) and is prepared BEFORE persistence: the ids are dropped
    // and exposed only after the revocation lands (durable success or the
    // committed-but-not-durable error), so a pre-replacement failure leaves
    // the pending table and the invalidated buffer untouched.
    std::optional<std::string> key;
    for (const auto& entry : store_.phones()) {
        if (entry.phone_id == phone) {
            key = identity_key(entry.identity);
            break;
        }
    }
    std::vector<core::PairingRequestId> doomed;
    if (key.has_value()) {
        for (const auto& pending : pending_) {
            if (identity_key(pending.identity) == key.value()) {
                doomed.push_back(pending.request);
            }
        }
    }
    const auto saved = store_.forget_phone(phone);
    const bool landed = saved.has_value() ||
                        saved.error().code() == ErrorCode::trust_store_not_durable;
    if (landed) {
        for (const auto request : doomed) {
            const std::size_t index = find(request);
            if (index < pending_.size()) {
                drop_at(index);
            }
            invalidated_.push_back(request);
        }
    }
    return saved;
}

std::vector<core::PairingRequestId> PairingPolicy::take_invalidated_pairings() {
    std::vector<core::PairingRequestId> drained;
    drained.swap(invalidated_);
    return drained;
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
