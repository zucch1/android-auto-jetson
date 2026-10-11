// SPDX-License-Identifier: GPL-3.0-or-later
#include <aa/ipc/PairingGate.hpp>

#include <random>

namespace aa::ipc {

namespace {

constexpr int kRandomDraws = 4;

} // namespace

core::PairingRequestId RandomPairingIdSource::next() {
    std::random_device device;
    std::uint64_t value = 0;
    for (int draw = 0; draw < kRandomDraws; ++draw) {
        value = (value << 16U) | (static_cast<std::uint64_t>(device()) & 0xFFFFU);
    }
    if (value == 0) {
        value = 1;  // id 0 is the unset id and never a valid one-time id
    }
    return core::PairingRequestId{value};
}

bool PairingGate::issued(core::PairingRequestId request) const noexcept {
    for (std::size_t index = 0; index < issued_count_; ++index) {
        if (issued_[index] == request) {
            return true;
        }
    }
    return false;
}

core::Result<core::PairingRequestId> PairingGate::request(const BoundConsumer& consumer) {
    if (!consumer.consumer) {
        return Error{ErrorCode::invalid_argument};
    }
    if (count_ == kMaxPending || issued_count_ == kIssuedBudget) {
        return Error{ErrorCode::ipc_queue_full};
    }
    const auto request = ids_.next();
    if (!request) {
        return Error{ErrorCode::internal};
    }
    if (issued(request)) {
        return Error{ErrorCode::internal};
    }
    issued_[issued_count_++] = request;
    entries_[count_++] = Entry{request, consumer, false};
    return request;
}

core::Result<void> PairingGate::confirm(const BoundConsumer& consumer,
                                        core::PairingRequestId request) {
    if (!request) {
        return Error{ErrorCode::ipc_malformed_request};
    }
    for (std::size_t index = 0; index < count_; ++index) {
        Entry& entry = entries_[index];
        if (entry.request != request) {
            continue;
        }
        if (!(entry.consumer == consumer)) {
            return Error{ErrorCode::ipc_peer_unauthorized};
        }
        if (entry.open || open_.has_value()) {
            return Error{ErrorCode::ipc_consumer_rejected};
        }
        entry.open = true;
        open_ = request;
        return {};
    }
    if (issued(request)) {
        return Error{ErrorCode::ipc_consumer_rejected};
    }
    return Error{ErrorCode::ipc_malformed_request};
}

core::Result<void> PairingGate::cancel(const BoundConsumer& consumer,
                                       core::PairingRequestId request) {
    if (!request) {
        return Error{ErrorCode::ipc_malformed_request};
    }
    for (std::size_t index = 0; index < count_; ++index) {
        if (entries_[index].request != request) {
            continue;
        }
        if (!(entries_[index].consumer == consumer)) {
            return Error{ErrorCode::ipc_peer_unauthorized};
        }
        for (std::size_t tail = index + 1; tail < count_; ++tail) {
            entries_[tail - 1] = entries_[tail];
        }
        --count_;
        if (open_ == request) {
            open_.reset();
        }
        return {};
    }
    if (issued(request)) {
        return Error{ErrorCode::ipc_consumer_rejected};
    }
    return Error{ErrorCode::ipc_malformed_request};
}

std::vector<core::PairingRequestId> PairingGate::close_all() {
    std::vector<core::PairingRequestId> closed;
    closed.reserve(count_);
    for (std::size_t index = 0; index < count_; ++index) {
        closed.push_back(entries_[index].request);
    }
    count_ = 0;
    open_.reset();
    return closed;
}

} // namespace aa::ipc
