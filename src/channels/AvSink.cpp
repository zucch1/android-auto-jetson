// SPDX-License-Identifier: GPL-3.0-or-later
#include <aa/channels/AvSink.hpp>
#include <aa/ipc/VideoSocket.hpp>
#include <aa/transport/Frames.hpp>

namespace aa::channels {
namespace {
Error malformed() { return Error{ErrorCode::protocol_malformed_message}; }
}

core::Result<void> AvSink::send(core::Result<std::vector<std::byte>> payload) {
    if (!payload) { return payload.error(); }
    return send_(std::move(payload).value());
}

void AvSink::reset() noexcept {
    ready_ = false;
    session_.reset();
    peer_timestamp_.reset();
    codec_cache_.clear();
    staged_credit_ = false;
    focus_ = protocol::VideoFocusMode::projected;
}

bool AvSink::input_focus_allowed() const noexcept {
    return service_.role == protocol::ChannelRole::video && ready_
        && focus_ == protocol::VideoFocusMode::projected;
}

core::Result<void> AvSink::setup(std::span<const std::byte> payload) {
    const auto request = protocol::decode_av_sink_setup(payload);
    if (!request) { return request.error(); }
    if (auto valid = protocol::validate_service(service_); !valid) { return valid.error(); }
    const bool video = service_.role == protocol::ChannelRole::video;
    if (video) {
        const auto* configuration = std::get_if<protocol::VideoConfiguration>(&service_.configuration);
        if (!configuration || configuration->profiles.empty()
            || request.value().codec != protocol::AvSinkCodec::h264) { return malformed(); }
    } else {
        switch (service_.role) {
        case protocol::ChannelRole::media_audio:
        case protocol::ChannelRole::guidance_audio:
        case protocol::ChannelRole::system_audio:
        case protocol::ChannelRole::speech_audio: break;
        default: return Error{ErrorCode::protocol_unsupported_channel};
        }
        const auto* configuration = std::get_if<protocol::AudioConfiguration>(&service_.configuration);
        if (!configuration || configuration->profiles.empty()
            || request.value().codec != protocol::AvSinkCodec::pcm) { return malformed(); }
    }
    reset();
    if (auto result = send(protocol::encode_av_sink_ready()); !result) { return result.error(); }
    if (video) {
        if (auto result = send(protocol::encode(protocol::VideoFocusNotification{focus_, true})); !result) {
            return result.error();
        }
    }
    ready_ = true;
    return {};
}

core::Result<void> AvSink::start(std::span<const std::byte> payload) {
    const auto request = protocol::decode_av_sink_start(payload);
    if (!request) { return request.error(); }
    if (!ready_ || request.value().configuration_index != 0) { return malformed(); }
    if (session_ == request.value().session_id) { return {}; }
    if (session_) {
        codec_cache_.clear();
        staged_credit_ = false;
        peer_timestamp_.reset();
    }
    session_ = request.value().session_id;
    if (std::exchange(staged_credit_, false)) {
        return send(protocol::encode_av_sink_ack(*session_));
    }
    return {};
}

core::Result<void> AvSink::codec_config(std::span<const std::byte> payload) {
    if (!ready_ || (!session_ && staged_credit_)) { return malformed(); }
    const auto decoded = protocol::decode_av_sink_codec_config(payload);
    if (!decoded) { return decoded.error(); }
    const auto body = decoded.value();
    if (service_.role == protocol::ChannelRole::video) {
        if (body.size() > ipc::kMaxAccessUnitBytes - codec_cache_.size()) { return malformed(); }
        codec_cache_.insert(codec_cache_.end(), body.begin(), body.end());
    } else if (!body.empty()) {
        return malformed();
    }
    if (!session_) { staged_credit_ = true; return {}; }
    return send(protocol::encode_av_sink_ack(*session_));
}

core::Result<void> AvSink::data(const protocol::MessageView& message) {
    if (!session_) { return malformed(); }
    const auto decoded = protocol::decode_av_sink_data(message.payload);
    if (!decoded) { return decoded.error(); }
    const auto body = decoded.value().body;
    const bool video = service_.role == protocol::ChannelRole::video;
    if (video) {
        if (body.size() > ipc::kMaxAccessUnitBytes - codec_cache_.size()) { return malformed(); }
    } else {
        const auto& profile = std::get<protocol::AudioConfiguration>(service_.configuration).profiles.front();
        const auto frame_bytes = static_cast<std::size_t>(profile.channels) * profile.bits / 8u;
        if (frame_bytes == 0 || body.size() % frame_bytes != 0) { return malformed(); }
    }
    peer_timestamp_ = decoded.value().peer_timestamp;
    auto combined = std::move(codec_cache_);
    codec_cache_.clear();
    const bool present = !video || focus_ == protocol::VideoFocusMode::projected
        || focus_ == protocol::VideoFocusMode::projected_no_input_focus;
    if (present) {
        auto media = body;
        if (!combined.empty()) {
            combined.insert(combined.end(), body.begin(), body.end());
            media = combined;
        }
        if (auto delivered = raw_.on_message({message.header, media}); !delivered) { return delivered.error(); }
    }
    return send(protocol::encode_av_sink_ack(*session_));
}

core::Result<void> AvSink::focus(std::span<const std::byte> payload) {
    if (service_.role != protocol::ChannelRole::video || !ready_) { return malformed(); }
    const auto request = protocol::decode_video_focus_request(payload);
    if (!request) { return request.error(); }
    const auto granted = request.value().mode.value_or(focus_);
    if (auto result = send(protocol::encode(protocol::VideoFocusNotification{granted, false})); !result) {
        return result.error();
    }
    focus_ = granted;
    return {};
}

core::Result<void> AvSink::on_message(const protocol::MessageView& message) {
    if (message.header.channel != service_.role) { return Error{ErrorCode::protocol_unsupported_channel}; }
    const auto payload = message.payload;
    if (payload.size() < 2 || payload.size() > transport::kMaxMessageBytes) { return malformed(); }
    const auto id = (static_cast<unsigned>(payload[0]) << 8u) | static_cast<unsigned>(payload[1]);
    switch (id) {
    case 0x8000: return setup(payload);
    case 0x8001: return start(payload);
    case 0x8002: {
        if (auto stop = protocol::decode_av_sink_stop(payload); !stop) { return stop.error(); }
        session_.reset();
        peer_timestamp_.reset();
        codec_cache_.clear();
        staged_credit_ = false;
        return {};
    }
    case 0x0000: return data(message);
    case 0x0001: return codec_config(payload);
    case 0x8007: return focus(payload);
    default: return malformed();
    }
}

} // namespace aa::channels
