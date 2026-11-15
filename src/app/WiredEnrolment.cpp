// SPDX-License-Identifier: GPL-3.0-or-later
#include <aa/app/WiredEnrolment.hpp>

#include <algorithm>
#include <utility>

namespace aa::app {

core::Result<void> WiredEnrolment::note_refused(const trust::WiredIdentity& identity) {
    if (!open_) return Error{ErrorCode::cancelled};
    if (!trust::validated_key(trust::TransportIdentity{identity})) {
        return Error{ErrorCode::invalid_argument};
    }
    refused_ = identity;
    refused_at_ = clock_.now();
    return {};
}

void WiredEnrolment::close() {
    open_ = false;
    refused_.reset();
}

WiredEnrolment::Request* WiredEnrolment::find(core::PairingRequestId request) {
    const auto it = std::find_if(requests_.begin(), requests_.end(),
                                 [request](const Request& entry) { return entry.id == request; });
    return it == requests_.end() ? nullptr : &*it;
}

void WiredEnrolment::untrack(core::PairingRequestId request) {
    std::erase_if(requests_, [request](const Request& entry) { return entry.id == request; });
}

core::Result<void> WiredEnrolment::forget(core::PhoneId phone) {
    if (!open_) return after_.forget(phone);
    auto forgotten = policy_.forget(phone);
    for (const auto request : policy_.take_invalidated_pairings()) {
        untrack(request);
        invalidated_.push_back(request);
    }
    return forgotten;
}

std::optional<helper::RevocationV1> WiredEnrolment::take_revocation_v1() {
    return open_ ? policy_.take_revocation_v1() : after_.take_revocation_v1();
}

core::Result<void> WiredEnrolment::pairing_requested(core::PairingRequestId request) {
    if (!open_) return Error{ErrorCode::trust_rejected};
    // Only a FRESH refused attempt can be enrolled; a stale or absent one binds nothing.
    const auto now = clock_.now();
    if (!refused_ ||
        now.count - refused_at_.count > core::to_nanoseconds(kCandidateFreshness).count) {
        return Error{ErrorCode::trust_unknown_phone};
    }
    const trust::TransportIdentity identity{*refused_};
    policy_.note_candidate(identity);
    const auto bound = policy_.pairing_requested(request);
    if (!bound) return bound;
    requests_.push_back(Request{request, identity, false});
    return {};
}

std::optional<std::map<std::string, std::string>> WiredEnrolment::pairing_candidate(
    core::PairingRequestId request) {
    Request* entry = find(request);
    if (!open_ || entry == nullptr || !policy_.has_pending(request)) return std::nullopt;
    entry->presented = true;
    return std::map<std::string, std::string>{
        {"transport", "wired"},
        {"label", trust::redacted_label(entry->identity)},
        {"strength", std::string{trust::strength_name(trust::strength_of(entry->identity))}}};
}

core::Result<void> WiredEnrolment::validate_pairing(core::PairingRequestId request) {
    const Request* entry = find(request);
    if (!open_ || entry == nullptr) return Error{ErrorCode::trust_unknown_phone};
    auto live = policy_.validate_pairing(request);
    if (!live) untrack(request);  // the policy dropped the expired request
    return live;
}

core::Result<core::PhoneId> WiredEnrolment::pairing_confirmed(core::PairingRequestId request) {
    const Request* entry = find(request);
    if (!open_ || entry == nullptr) return Error{ErrorCode::trust_unknown_phone};
    if (!entry->presented) {
        // No approval of an unseen phone: the one-time request is spent.
        policy_.pairing_cancelled(request);
        untrack(request);
        return Error{ErrorCode::trust_rejected};
    }
    untrack(request);
    auto approved = policy_.pairing_confirmed(request);
    if (approved) {
        completion_ = Completion{approved.value(), true};
        return approved;
    }
    const auto committed = policy_.committed_pairing();
    if (approved.error().code() != ErrorCode::trust_store_not_durable || !committed) return approved;
    // The rename landed; only the directory sync failed. Keep the record and
    // retry the sync (bounded) before calling this a completed enrolment.
    for (int attempt = 0; attempt < kDirectorySyncAttempts; ++attempt) {
        if (store_.sync_directory()) {
            completion_ = Completion{*committed, true};
            return *committed;
        }
    }
    completion_ = Completion{*committed, false};
    return approved;
}

void WiredEnrolment::pairing_cancelled(core::PairingRequestId request) {
    policy_.pairing_cancelled(request);
    untrack(request);
}

std::vector<core::PairingRequestId> WiredEnrolment::reap_expired() {
    auto expired = policy_.reap_expired();
    if (!open_) {
        for (const auto& entry : requests_) {
            policy_.pairing_cancelled(entry.id);
            if (std::find(expired.begin(), expired.end(), entry.id) == expired.end())
                expired.push_back(entry.id);
        }
    }
    for (const auto request : expired) untrack(request);
    return expired;
}

std::vector<core::PairingRequestId> WiredEnrolment::take_invalidated_pairings() {
    std::vector<core::PairingRequestId> drained;
    drained.swap(invalidated_);
    for (const auto request : policy_.take_invalidated_pairings()) {
        untrack(request);
        drained.push_back(request);
    }
    if (!open_) {
        for (const auto request : after_.take_invalidated_pairings()) drained.push_back(request);
    }
    return drained;
}

} // namespace aa::app
