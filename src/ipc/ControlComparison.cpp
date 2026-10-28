// SPDX-License-Identifier: GPL-3.0-or-later
#include <aa/ipc/ControlService.hpp>

namespace aa::ipc {
core::Result<std::map<std::string, std::string>> ControlService::pairing_comparison(
    const BusName& caller, core::PairingRequestId request) {
    const auto active = require_consumer();
    if (!active) return active.error();
    const auto admitted = admit(caller, active.value());
    if (!admitted) return admitted.error();
    if (!confirmation_ || !open_pairing_ || open_pairing_->request != request || !open_pairing_->peer ||
        std::chrono::steady_clock::now() >= open_pairing_->deadline) return Error{ErrorCode::trust_rejected};
    const auto value = confirmation_->comparison(active.value().consumer, request);
    if (!value) return value.error();
    if (!value.value().id || value.value().number > 999999 || value.value().peer != *open_pairing_->peer ||
        phones_.wireless_identity(open_pairing_->phone) != open_pairing_->peer ||
        std::chrono::steady_clock::now() >= open_pairing_->deadline) {
        confirmation_->cancel(active.value().consumer, request);
        return Error{ErrorCode::trust_rejected};
    }
    const auto owner = require_consumer();
    if (!owner || owner.value().consumer != active.value().consumer || !admit(caller, owner.value()))
        return Error{ErrorCode::ipc_peer_unauthorized};
    if (!open_pairing_ || open_pairing_->request != request || !open_pairing_->peer ||
        value.value().peer != *open_pairing_->peer ||
        phones_.wireless_identity(open_pairing_->phone) != open_pairing_->peer ||
        std::chrono::steady_clock::now() >= open_pairing_->deadline)
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
        phones_.wireless_identity(open_pairing_->phone) != open_pairing_->peer ||
        std::chrono::steady_clock::now() >= open_pairing_->deadline) return Error{ErrorCode::trust_rejected};
    open_pairing_->presented_comparison.reset();
    const auto current = confirmation_->comparison(active.value().consumer, request);
    if (!current || current.value().id != response.id || current.value().peer != *open_pairing_->peer ||
        std::chrono::steady_clock::now() >= open_pairing_->deadline) {
        confirmation_->cancel(active.value().consumer, request);
        return Error{ErrorCode::trust_rejected};
    }
    // Reconcile consumer ownership again after the synchronous helper round trip.
    const auto owner = require_consumer();
    if (!owner || owner.value().consumer != active.value().consumer || !admit(caller, owner.value()))
        return Error{ErrorCode::ipc_peer_unauthorized};
    if (!open_pairing_ || open_pairing_->request != request || !open_pairing_->peer ||
        current.value().peer != *open_pairing_->peer ||
        phones_.wireless_identity(open_pairing_->phone) != open_pairing_->peer ||
        std::chrono::steady_clock::now() >= open_pairing_->deadline) {
        confirmation_->cancel(active.value().consumer, request);
        return Error{ErrorCode::trust_rejected};
    }
    return confirmation_->respond_comparison(active.value().consumer, request, response);
}
}
