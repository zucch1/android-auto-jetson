// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <aa/protocol/Protocol.hpp>

#include <cstddef>
#include <string_view>

namespace aa::diagnostics {

enum class QueueSource { audio_worker, av_pending, audio_router };
enum class QueueReason { payload_oversize, slots, bytes, weighted_units, data, codec_config, pending_runs };

[[nodiscard]] constexpr std::string_view to_string(QueueSource source) noexcept {
    switch (source) {
    case QueueSource::audio_worker: return "audio_worker";
    case QueueSource::av_pending: return "av_pending";
    case QueueSource::audio_router: return "audio_router";
    }
    return "unknown";
}

[[nodiscard]] constexpr std::string_view to_string(QueueReason reason) noexcept {
    switch (reason) {
    case QueueReason::payload_oversize: return "payload_oversize";
    case QueueReason::slots: return "slots";
    case QueueReason::bytes: return "bytes";
    case QueueReason::weighted_units: return "weighted_units";
    case QueueReason::data: return "data";
    case QueueReason::codec_config: return "codec_config";
    case QueueReason::pending_runs: return "pending_runs";
    }
    return "unknown";
}

// Failure-site observation, not Error metadata or a rejected wire-frame claim.
// No payload, peer text, allocation or unbounded history. Zero capacity means
// that dimension is not used by this gate. AV requested_bytes counts the wire
// message payload; worker/router requested_bytes counts stripped media bytes.
// Worker units are rounded input-frame bytes (voice weighted 6x).
struct QueueRejection final {
    QueueSource source;
    QueueReason reason;
    protocol::ChannelRole role;
    std::size_t items{};
    std::size_t capacity{};
    std::size_t bytes{};
    std::size_t byte_capacity{};
    std::size_t units{};
    std::size_t unit_capacity{};
    std::size_t requested_bytes{};
    std::size_t requested_units{};
    [[nodiscard]] friend bool operator==(const QueueRejection&, const QueueRejection&) = default;
};

} // namespace aa::diagnostics
