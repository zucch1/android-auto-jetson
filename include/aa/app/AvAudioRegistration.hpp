// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <aa/app/WiredSession.hpp>
#include <aa/audio/AudioRenderWorker.hpp>
#include <aa/audio/AudioTelemetry.hpp>
#include <aa/core/Error.hpp>
#include <aa/core/Result.hpp>
#include <aa/diagnostics/QueueRejection.hpp>
#include <aa/protocol/Protocol.hpp>

#include <optional>
#include <string>

namespace aa::app {

[[nodiscard]] core::Result<void> register_deferred_audio_sinks(WiredSession& session,
                                                               audio::AudioRenderWorker& audio);

[[nodiscard]] core::Result<void> register_av_sinks(WiredSession& session,
                                                   protocol::MessageSink& video,
                                                   audio::AudioRenderWorker& audio);

[[nodiscard]] core::Result<void> service_audio_completions(WiredSession& session,
                                                           audio::AudioRenderWorker& audio);

[[nodiscard]] std::string poll_error_json(
    ErrorCode code,
    std::optional<WiredSession::RejectedFrame> reject,
    std::optional<diagnostics::QueueRejection> queue = {},
    std::optional<audio::AudioRejectionSnapshot> audio = {});

[[nodiscard]] std::string poll_error_json(ErrorCode code,
                                          const WiredSession& session,
                                          const audio::AudioRenderWorker& audio);

[[nodiscard]] std::string poll_error_json(ErrorCode code,
                                          const audio::AudioRenderWorker& audio);

} // namespace aa::app
