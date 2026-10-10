// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <aa/channels/Negotiator.hpp>

namespace channeltest {
namespace proto = aa::protocol;
inline proto::ServiceDescriptor video(std::int32_t id = 99) {
    return {proto::ServiceKey{id}, proto::ChannelRole::video,
            proto::VideoConfiguration{{{1280, 720, 30}, {800, 480, 30}}}};
}
inline proto::ServiceDescriptor audio(proto::ChannelRole role, std::int32_t id) {
    return {proto::ServiceKey{id}, role, proto::AudioConfiguration{{{48000, 16, 2}}}};
}
struct Sink final : proto::MessageSink {
    int calls{};
    proto::MessageHeader last{};
    std::vector<std::byte> bytes{};
    aa::core::Result<void> on_message(const proto::MessageView& message) override {
        ++calls;
        last = message.header;
        bytes.assign(message.payload.begin(), message.payload.end());
        return {};
    }
};
} // namespace channeltest
