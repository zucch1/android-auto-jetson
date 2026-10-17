// SPDX-License-Identifier: GPL-3.0-or-later
#include <aa/channels/InputSink.hpp>

namespace aa::channels {

core::Result<void> InputSink::on_message(const protocol::MessageView& message) {
    if (message.header.channel != protocol::ChannelRole::input) {
        return Error{ErrorCode::protocol_unsupported_channel};
    }
    if (message.payload.size() >= 2 && message.payload[0] == std::byte{0x80} &&
        message.payload[1] == std::byte{0x04}) {
        ++feedback_;
    } else {
        ++unhandled_;
    }
    return {};
}

} // namespace aa::channels
