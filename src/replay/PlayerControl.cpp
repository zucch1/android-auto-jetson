// SPDX-License-Identifier: GPL-3.0-or-later
#include <aa/replay/Player.hpp>

#include <aa/protocol/Messages.hpp>

#include <cstdint>
#include <utility>

namespace aa::replay {
namespace {

constexpr Error mismatch() { return Error{ErrorCode::invalid_argument}; }

} // namespace

core::Result<void> ReplayPlayer::send_frames(std::vector<std::byte> payload) {
    transport::Message message{std::uint8_t{0}, transport::MessageKind::control,
                               std::move(payload)};
    auto frames = transport::encode_message(message, transport::Encryption::plain, nullptr);
    if (!frames) {
        return frames.error();
    }
    for (const auto& frame : frames.value()) {
        if (auto sent = active_->send(frame); !sent) {
            return sent.error();
        }
    }
    return {};
}

core::Result<void> ReplayPlayer::send_payload(const protocol::ServiceDiscoveryResponse& response) {
    auto payload = protocol::encode(response);
    if (!payload) {
        return payload.error();
    }
    return send_frames(std::move(payload).value());
}

core::Result<void> ReplayPlayer::send_payload(const protocol::ChannelOpenResponse& response) {
    auto payload = protocol::encode(response);
    if (!payload) {
        return payload.error();
    }
    return send_frames(std::move(payload).value());
}

core::Result<void> ReplayPlayer::handle_control(std::span<const std::byte> payload) {
    if (payload.size() < 2) {
        return mismatch();
    }
    const auto high = static_cast<std::uint16_t>(static_cast<std::uint8_t>(payload[0]));
    const auto low = static_cast<std::uint16_t>(static_cast<std::uint8_t>(payload[1]));
    const auto id = static_cast<protocol::ControlMessageId>((high << 8U) | low);
    switch (id) {
    case protocol::ControlMessageId::service_discovery_request:
        return send_payload(negotiator_->start());
    case protocol::ControlMessageId::channel_open_request: {
        auto request = protocol::decode_channel_open_request(payload);
        if (!request) {
            return request.error();
        }
        auto opened = negotiator_->open(request.value());
        if (!opened) {
            return opened.error();
        }
        return send_payload(opened.value());
    }
    default: return mismatch();
    }
}

core::Result<void> ReplayPlayer::route_message(const transport::Message& message,
                                               std::uint64_t sequence, core::Nanoseconds at) {
    if (message.channel == 0) {
        return handle_control(message.payload);
    }
    return negotiator_->dispatch(protocol::WireMessageView{
        message.channel, sequence, at, message.payload});
}

} // namespace aa::replay
