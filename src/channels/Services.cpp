// SPDX-License-Identifier: GPL-3.0-or-later
#include <aa/channels/Services.hpp>

#include <algorithm>
#include <limits>

namespace aa::channels {

CapabilityProfile CapabilityProfile::bench_defaults() {
    using protocol::VideoProfile;
    return CapabilityProfile{
        VideoProfile{1280, 720, 30},
        VideoProfile{800, 480, 30},
        {},
    };
}

bool CapabilityProfile::advertises(protocol::ChannelRole role) const noexcept {
    return std::find(advertised.begin(), advertised.end(), role) != advertised.end();
}

core::Result<void> ServiceRegistry::register_sink(protocol::ChannelRole role,
                                                  protocol::MessageSink& sink) {
    switch (role) {
    case protocol::ChannelRole::video:
    case protocol::ChannelRole::media_audio:
    case protocol::ChannelRole::guidance_audio:
    case protocol::ChannelRole::system_audio:
    case protocol::ChannelRole::speech_audio:
    case protocol::ChannelRole::input:
    case protocol::ChannelRole::microphone:
    case protocol::ChannelRole::sensors:
    case protocol::ChannelRole::bluetooth_projection:
    case protocol::ChannelRole::wifi_projection:
    case protocol::ChannelRole::diagnostics:
        break;
    default:
        return Error{ErrorCode::protocol_unsupported_channel};
    }
    if (next_generation_ == std::numeric_limits<std::uint64_t>::max()) {
        return Error{ErrorCode::channel_dispatch_failed};
    }
    const auto [_, inserted] = sinks_.emplace(role, Registration{&sink, next_generation_});
    if (!inserted) {
        return Error{ErrorCode::channel_dispatch_failed};
    }
    ++next_generation_;
    return {};
}

core::Result<void> ServiceRegistry::unregister_sink(protocol::ChannelRole role) {
    if (sinks_.erase(role) == 0) {
        return Error{ErrorCode::channel_not_registered};
    }
    return {};
}

core::Result<void> ServiceRegistry::dispatch(const protocol::MessageView& message,
                                            std::uint64_t expected_generation) {
    const auto found = sinks_.find(message.header.channel);
    if (found == sinks_.end() || found->second.generation != expected_generation) {
        return Error{ErrorCode::channel_not_registered};
    }
    return found->second.sink->on_message(message);
}

core::Result<void> ServiceRegistry::dispatch_local(const protocol::MessageView& message) {
    if (message.header.channel != protocol::ChannelRole::diagnostics) {
        return Error{ErrorCode::protocol_unsupported_channel};
    }
    const auto token = generation(message.header.channel);
    if (!token) { return Error{ErrorCode::channel_not_registered}; }
    return dispatch(message, *token);
}

std::optional<std::uint64_t> ServiceRegistry::generation(protocol::ChannelRole role) const {
    const auto found = sinks_.find(role);
    if (found == sinks_.end()) { return std::nullopt; }
    return found->second.generation;
}

bool ServiceRegistry::has_sink(protocol::ChannelRole role) const noexcept {
    return sinks_.contains(role);
}

} // namespace aa::channels
