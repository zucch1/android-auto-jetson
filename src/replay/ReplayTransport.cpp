// SPDX-License-Identifier: GPL-3.0-or-later
#include <aa/replay/Player.hpp>

#include <algorithm>
#include <utility>

namespace aa::replay {
namespace {

constexpr Error mismatch() { return Error{ErrorCode::invalid_argument}; }

} // namespace

core::Result<void> ReplayTransport::open(core::CancellationToken cancellation) {
    if (closed_) {
        return Error{ErrorCode::transport_closed};
    }
    if (cancellation.stop_requested()) {
        close();
        return Error{ErrorCode::cancelled};
    }
    token_ = cancellation;
    open_ = true;
    return {};
}

core::Result<void> ReplayTransport::send(std::span<const std::byte> frame) {
    if (!open_ || closed_) {
        return Error{ErrorCode::transport_closed};
    }
    if (token_.has_value() && token_->stop_requested()) {
        close();
        return Error{ErrorCode::cancelled};
    }
    if (script_->outbound_pos >= script_->outbound.size()) {
        close();
        return mismatch();
    }
    const auto& expected = script_->outbound[script_->outbound_pos];
    if (frame.size() != expected.size()
        || !std::equal(frame.begin(), frame.end(), expected.begin())) {
        close();
        return mismatch();
    }
    ++script_->outbound_pos;
    ++script_->matched_sends;
    return {};
}

core::Result<std::vector<std::byte>> ReplayTransport::receive() {
    if (!open_ || closed_) {
        return Error{ErrorCode::transport_closed};
    }
    if (token_.has_value() && token_->stop_requested()) {
        close();
        return Error{ErrorCode::cancelled};
    }
    if (script_->inbound_pos >= script_->inbound.size()) {
        close();
        return Error{ErrorCode::transport_closed};
    }
    std::vector<std::byte> frame = script_->inbound[script_->inbound_pos];
    ++script_->inbound_pos;
    return frame;
}

void ReplayTransport::close() noexcept {
    open_ = false;
    closed_ = true;
}

core::Result<void> ReplayPlayer::Observer::on_message(const protocol::MessageView& message) {
    DeliveryOutcome outcome;
    outcome.role = message.header.channel;
    outcome.sequence = message.header.sequence;
    outcome.timestamp = message.header.timestamp;
    outcome.payload.assign(message.payload.begin(), message.payload.end());
    observed->deliveries.push_back(std::move(outcome));
    if (forward != nullptr) {
        return forward->on_message(message);
    }
    return {};
}

} // namespace aa::replay
