// SPDX-License-Identifier: GPL-3.0-or-later
#include <aa/channels/MicrophoneSink.hpp>
#include <aa/protocol/Microphone.hpp>

namespace aa::channels {

core::Result<void> MicrophoneSink::on_message(const protocol::MessageView& message) {
    if (message.header.channel != protocol::ChannelRole::microphone) {
        return Error{ErrorCode::protocol_unsupported_channel};
    }
    if (message.payload.size() < 2) { return Error{ErrorCode::protocol_malformed_message}; }
    const auto id = (std::to_integer<unsigned>(message.payload[0]) << 8U)
                  | std::to_integer<unsigned>(message.payload[1]);
    if (id == 0x8004) { return protocol::decode_microphone_ack(message.payload); }
    if (id == 0x8000) {
        if (const auto setup = protocol::decode_microphone_setup(message.payload); !setup) {
            return setup.error();
        }
        auto response = protocol::encode_microphone_ready();
        if (!response) { return response.error(); }
        return send_(std::move(response).value());
    }
    if (id == 0x8005) {
        const auto request = protocol::decode_microphone_request(message.payload);
        if (!request) { return request.error(); }
        auto response = protocol::encode(protocol::MicrophoneResponse{
            request.value().open ? protocol::kMicrophoneCaptureUnsupported : 0});
        if (!response) { return response.error(); }
        return send_(std::move(response).value());
    }
    return Error{ErrorCode::protocol_unsupported_channel};
}

} // namespace aa::channels
