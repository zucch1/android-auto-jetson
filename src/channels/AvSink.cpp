// SPDX-License-Identifier: GPL-3.0-or-later
#include <aa/channels/AvSink.hpp>
#include <aa/ipc/VideoSocket.hpp>
#include <aa/transport/Frames.hpp>

#include <algorithm>
#include <limits>

namespace aa::channels {
namespace {
Error malformed() { return Error{ErrorCode::protocol_malformed_message}; }
}

core::Result<void> AvSink::send(core::Result<std::vector<std::byte>> payload) {
    if (!payload) { return payload.error(); }
    return send_(std::move(payload).value());
}

void AvSink::observe(AvCreditEvent event, std::optional<ErrorCode> error, std::uint32_t count) const {
    if (!deferred_ || !deferred_->observe) { return; }
    auto value = current_observation_;
    value.event = event;
    value.delivery.role = service_.role;
    value.delivery.generation = generation_;
    value.delivery.session_id = session_.value_or(0);
    value.session_active = session_.has_value();
    value.pending = pending_.empty() ? std::nullopt : std::optional{pending_.front().id};
    value.error = error;
    value.count = count;
    value.at_ns = av_observation_now();
    deferred_->observe(value);
}

void AvSink::observe_receive(bool entering) const {
    observe(entering ? AvCreditEvent::receive_entry : AvCreditEvent::receive_return);
}

core::Result<void> AvSink::send_ack(std::int32_t session, AckFor for_message) {
    auto result = send(protocol::encode_av_sink_ack(session));
    current_observation_.ack_for = static_cast<std::uint16_t>(for_message);
    observe(AvCreditEvent::ack, result ? std::nullopt : std::optional{result.error().code()}, 1);
    return result;
}

void AvSink::invalidate_delivery() noexcept {
    pending_.clear();
    auto& counter = generation_counter_ ? *generation_counter_ : generation_;
    // Exhaustion fails closed in data(); never wrap into a retired generation.
    if (counter != std::numeric_limits<std::uint64_t>::max()) { ++counter; }
    generation_ = counter;
    if (deferred_ && deferred_->invalidate) { deferred_->invalidate(service_.role, generation_); }
    observe(AvCreditEvent::reset);
}

core::Result<void> AvSink::complete_av_delivery(AvDeliveryId id, core::Result<void> result) {
    if (id.generation != generation_ || session_ != id.session_id) { return {}; }
    // Completions arrive oldest first; a stale or duplicate ID matches nothing.
    const auto match = std::find_if(pending_.begin(), pending_.end(),
                                    [&](const PendingDelivery& entry) { return entry.id == id; });
    if (match == pending_.end()) { return {}; }
    current_observation_ = match->observation;
    pending_.erase(match);
    if (!result) { return result.error(); }
    return send_ack(id.session_id, AckFor::data);
}

void AvSink::reset() noexcept {
    ready_ = false;
    announced_.clear();
    selected_profile_.reset();
    session_.reset();
    peer_timestamp_.reset();
    codec_cache_.clear();
    staged_credit_ = false;
    focus_ = protocol::VideoFocusMode::projected;
    invalidate_delivery();
}

bool AvSink::input_focus_allowed() const noexcept {
    return service_.role == protocol::ChannelRole::video && ready_
        && focus_ == protocol::VideoFocusMode::projected;
}

bool AvSink::video_projecting() const noexcept {
    return service_.role == protocol::ChannelRole::video && ready_ && session_.has_value()
        && (focus_ == protocol::VideoFocusMode::projected
            || focus_ == protocol::VideoFocusMode::projected_no_input_focus);
}

core::Result<void> AvSink::request_video_idr(bool focus_transition) {
    if (!video_projecting()) {
        return Error{ErrorCode::session_illegal_transition};
    }
    if (!focus_transition) {
        return send(protocol::encode(protocol::VideoFocusNotification{focus_, true}));
    }
    // Workaround for phones/encoders that ignore a repeated projected focus
    // notification: perform an ordered native -> prior projected focus
    // transition (see aa-proxy/aa-proxy-rs commit 2af9270, issue #142).
    const auto prior_focus = focus_;
    if (auto to_native = send(protocol::encode(
            protocol::VideoFocusNotification{protocol::VideoFocusMode::native, true}));
        !to_native) {
        return to_native.error();
    }
    if (auto restore = send(protocol::encode(protocol::VideoFocusNotification{prior_focus, true}));
        !restore) {
        // The phone may have already received native focus while restore failed;
        // do not claim projected focus or allow replay — escalate as terminal.
        focus_ = protocol::VideoFocusMode::native;
        return Error{ErrorCode::channel_focus_restore_failed};
    }
    return {};
}

core::Result<void> AvSink::setup(std::span<const std::byte> payload) {
    constexpr protocol::AvSinkCodec codecs[] = {protocol::AvSinkCodec::pcm, protocol::AvSinkCodec::h264, protocol::AvSinkCodec::h265, protocol::AvSinkCodec::vp9, protocol::AvSinkCodec::av1};
    const auto request = protocol::decode_av_sink_setup_for_codecs(payload, codecs);
    if (!request) { return request.error(); }
    if (auto valid = protocol::validate_service(service_); !valid) { return valid.error(); }
    const bool video = service_.role == protocol::ChannelRole::video;
    if (video) {
        const auto* configuration = std::get_if<protocol::VideoConfiguration>(&service_.configuration);
        if (!configuration || configuration->profiles.empty()
            || std::none_of(configuration->profiles.begin(), configuration->profiles.end(),
                [&](const auto& p) { return static_cast<unsigned>(p.codec) == static_cast<unsigned>(request.value().codec); })) { return malformed(); }
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
    if (video) {
        const auto& profiles = std::get<protocol::VideoConfiguration>(service_.configuration).profiles;
        for (std::uint32_t i = 0; i < profiles.size(); ++i) {
            if (static_cast<unsigned>(profiles[i].codec) == static_cast<unsigned>(request.value().codec)) { announced_.push_back(i); }
        }
    } else { announced_.push_back(0); }
    const auto result = send(protocol::encode_av_sink_ready(announced_, video ? 1U : window()));
    observe(AvCreditEvent::ready, result ? std::nullopt : std::optional{result.error().code()},
            video ? 1U : window());
    if (!result) { return result.error(); }
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
    if (!ready_ || std::find(announced_.begin(), announced_.end(), request.value().configuration_index) == announced_.end()) { return malformed(); }
    if (service_.role == protocol::ChannelRole::video) {
        const auto profile = std::get<protocol::VideoConfiguration>(service_.configuration).profiles[request.value().configuration_index];
        if (session_ == request.value().session_id && selected_profile_ != profile) { return malformed(); }
        selected_profile_ = profile;
    }
    if (session_ == request.value().session_id) { return {}; }
    if (session_) {
        codec_cache_.clear();
        staged_credit_ = false;
        peer_timestamp_.reset();
        invalidate_delivery();
    }
    session_ = request.value().session_id;
    if (std::exchange(staged_credit_, false)) {
        if (auto ack = send_ack(*session_, AckFor::codec_config); !ack) { return ack.error(); }
    }
    if (on_video_start_ && video_projecting()) { return on_video_start_(); }
    return {};
}

core::Result<void> AvSink::codec_config(std::span<const std::byte> payload) {
    if (!ready_ || (!session_ && staged_credit_)) { return malformed(); }
    if (!pending_.empty()) {
        last_queue_rejection_ = diagnostics::QueueRejection{
            diagnostics::QueueSource::av_pending, diagnostics::QueueReason::codec_config,
            service_.role, pending_.size(), window(), 0, 0, 0, 0, payload.size()};
        return Error{ErrorCode::ipc_queue_full};
    }
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
    return send_ack(*session_, AckFor::codec_config);
}

core::Result<void> AvSink::data(const protocol::MessageView& message) {
    if (!session_) { return malformed(); }
    if (pending_.size() >= window()) {
        // The phone sent past the advertised credit window.
        last_queue_rejection_ = diagnostics::QueueRejection{
            diagnostics::QueueSource::av_pending, diagnostics::QueueReason::data,
            service_.role, pending_.size(), window(), 0, 0, 0, 0, message.payload.size()};
        return Error{ErrorCode::ipc_queue_full};
    }
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
        auto header = message.header;
        header.video_profile = selected_profile_;
        header.video_session = session_;
        header.video_generation = generation_;
        auto media = body;
        if (!combined.empty()) {
            const auto cached_size = combined.size();
            combined.resize(cached_size + body.size());
            std::copy(body.begin(), body.end(), combined.data() + cached_size);
            media = combined;
        }
        if (deferred_) {
            if (!deferred_->submit) { return Error{ErrorCode::invalid_argument}; }
            if (generation_ == std::numeric_limits<std::uint64_t>::max()
                || delivery_sequence_ == std::numeric_limits<std::uint64_t>::max()) {
                return Error{ErrorCode::internal};
            }
            const AvDeliveryId id{service_.role, generation_, ++delivery_sequence_, *session_};
            current_observation_.delivery.sequence = id.sequence;
            pending_.push_back({id, current_observation_});
            if (auto submitted = deferred_->submit({header, media}, id); !submitted) {
                if (submitted.error().code() == ErrorCode::ipc_queue_full && deferred_->rejection) {
                    last_queue_rejection_ = deferred_->rejection();
                }
                pending_.pop_back();
                return submitted.error();
            }
            return {};
        }
        if (auto delivered = raw_->on_message({header, media}); !delivered) { return delivered.error(); }
    }
    return send_ack(*session_, AckFor::data);
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
    last_queue_rejection_.reset();
    if (message.header.channel != service_.role) { return Error{ErrorCode::protocol_unsupported_channel}; }
    const auto payload = message.payload;
    if (payload.size() < 2 || payload.size() > transport::kMaxMessageBytes) { return malformed(); }
    const auto id = (static_cast<unsigned>(payload[0]) << 8u) | static_cast<unsigned>(payload[1]);
    current_observation_ = {};
    current_observation_.message_sequence = message.header.sequence;
    current_observation_.message_id = static_cast<std::uint16_t>(id);
    current_observation_.bytes = payload.size();
    observe(AvCreditEvent::arrival);
    const auto result = [&]() -> core::Result<void> { switch (id) {
    case 0x8000: return setup(payload);
    case 0x8001: return start(payload);
    case 0x8002: {
        if (auto stop = protocol::decode_av_sink_stop(payload); !stop) { return stop.error(); }
        session_.reset();
        peer_timestamp_.reset();
        codec_cache_.clear();
        staged_credit_ = false;
        invalidate_delivery();
        return {};
    }
    case 0x0000: return data(message);
    case 0x0001: return codec_config(payload);
    case 0x8007: return focus(payload);
    default: return malformed();
    }}();
    observe(result ? AvCreditEvent::accepted : AvCreditEvent::rejected,
            result ? std::nullopt : std::optional{result.error().code()});
    return result;
}

} // namespace aa::channels
