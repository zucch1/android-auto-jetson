// SPDX-License-Identifier: GPL-3.0-or-later
#include <aa/app/AvAudioRegistration.hpp>

#include <aa/audio/AvCreditJson.hpp>

#include <string>

namespace aa::app {

core::Result<void> register_deferred_audio_sinks(WiredSession& session,
                                                 audio::AudioRenderWorker& audio) {
    using protocol::ChannelRole;
    for (const auto role : {ChannelRole::media_audio, ChannelRole::guidance_audio,
                            ChannelRole::system_audio}) {
        if (auto result = session.register_deferred_av_sink(role, {
                [&audio](const auto& message, auto id) { return audio.submit(message, id); },
                [&audio](auto invalidated_role, auto generation) {
                    audio.invalidate(invalidated_role, generation);
                },
                [&audio] { return audio.last_queue_rejection(); },
                [&audio](const auto& value) { audio.observe_av(value); }});
            !result) {
            return result.error();
        }
    }
    return {};
}

core::Result<void> register_av_sinks(WiredSession& session, protocol::MessageSink& video,
                                     audio::AudioRenderWorker& audio) {
    if (auto result = session.register_av_sink(protocol::ChannelRole::video, video); !result) {
        return result.error();
    }
    return register_deferred_audio_sinks(session, audio);
}

core::Result<void> service_audio_completions(WiredSession& session,
                                             audio::AudioRenderWorker& audio) {
    for (;;) {
        if (const auto error = audio.failure()) {
            (void)session.fail(*error);
            return *error;
        }
        const auto completion = audio.take_completion();
        if (!completion) { return {}; }
        if (const auto error = audio.failure()) {
            (void)session.fail(*error);
            return *error;
        }
        if (auto delivered = session.complete_av_delivery(*completion, {}); !delivered) {
            return delivered.error();
        }
    }
}

std::string poll_error_json(ErrorCode code,
                            std::optional<WiredSession::RejectedFrame> reject,
                            std::optional<diagnostics::QueueRejection> queue,
                            std::optional<audio::AudioRejectionSnapshot> audio) {
    std::string line = std::string{"{\"event\":\"err\",\"detail\":\"poll\",\"code\":\""} +
                       std::string{aa::to_string(code)} + "\"";
    if (code == ErrorCode::channel_not_registered && reject) {
        line += ",\"reject_channel\":" + std::to_string(reject->channel);
        if (reject->msg_id) {
            line += ",\"reject_msg_id\":" + std::to_string(*reject->msg_id);
        }
    }
    if (code == ErrorCode::ipc_queue_full && queue) {
        line += ",\"queue_rejection\":{\"source\":\"" +
                std::string{diagnostics::to_string(queue->source)} + "\",\"reason\":\"" +
                std::string{diagnostics::to_string(queue->reason)} + "\",\"role\":" +
                std::to_string(static_cast<unsigned>(queue->role)) +
                ",\"items\":" + std::to_string(queue->items) +
                ",\"capacity\":" + std::to_string(queue->capacity) +
                ",\"bytes\":" + std::to_string(queue->bytes) +
                ",\"byte_capacity\":" + std::to_string(queue->byte_capacity) +
                ",\"units\":" + std::to_string(queue->units) +
                ",\"unit_capacity\":" + std::to_string(queue->unit_capacity) +
                ",\"requested_bytes\":" + std::to_string(queue->requested_bytes) +
                ",\"requested_units\":" + std::to_string(queue->requested_units);
        if (queue->source == diagnostics::QueueSource::audio_worker) {
            line += ",\"requested_items\":" + std::to_string(queue->requested_items) +
                    ",\"remaining_permits\":" + std::to_string(queue->remaining_permits) +
                    ",\"reserved_bytes\":" + std::to_string(queue->reserved_bytes) +
                    ",\"reserved_units\":" + std::to_string(queue->reserved_units);
        }
        line += "}";
        if (audio && audio->queue == *queue) {
            line += ",\"audio_rejection\":" + audio::audio_rejection_json(*audio);
        }
    }
    return line + "}";
}

std::string poll_error_json(ErrorCode code,
                            const WiredSession& session,
                            const audio::AudioRenderWorker& audio) {
    const auto failure = audio.failure();
    const auto rejection = failure && failure->code() == ErrorCode::ipc_queue_full
        ? audio.failure_queue_rejection()
        : session.last_queue_rejection();
    auto line = poll_error_json(code, session.last_reject(), rejection,
                                audio.current_rejection_snapshot());
    if (code == ErrorCode::ipc_queue_full && rejection) {
        if (const auto trace = audio.credit_trace()) {
            line.pop_back();
            line += ",\"av_credit_trace\":" + audio::av_credit_json(*trace) + "}";
        }
    }
    return line;
}

std::string poll_error_json(ErrorCode code,
                            const audio::AudioRenderWorker& audio,
                            std::optional<WiredSession::RejectedFrame> reject,
                            std::optional<diagnostics::QueueRejection> session_queue_rejection) {
    const auto failure = audio.failure();
    const auto rejection = failure && failure->code() == ErrorCode::ipc_queue_full
        ? audio.failure_queue_rejection()
        : (session_queue_rejection ? session_queue_rejection : audio.last_queue_rejection());
    auto line = poll_error_json(code, reject, rejection,
                                audio.current_rejection_snapshot());
    if (code == ErrorCode::ipc_queue_full && rejection) {
        if (const auto trace = audio.credit_trace()) {
            line.pop_back();
            line += ",\"av_credit_trace\":" + audio::av_credit_json(*trace) + "}";
        }
    }
    return line;
}

std::string poll_error_json(ErrorCode code, const audio::AudioRenderWorker& audio) {
    return poll_error_json(code, audio, std::nullopt, std::nullopt);
}

} // namespace aa::app
