// SPDX-License-Identifier: GPL-3.0-or-later
// GStreamer-free session admission state for the task-23 consumer decode
// helper (PR #47 fix round, findings 3+4). The two pre-push admission
// contracts live here so tests can drive them deterministically, without
// racing appsrc: (1) feed-backlog admission counts the incoming AU against
// the backlog limit, and (2) session timestamps are valid, strictly
// increasing and session-unique behind a high-water mark that advances only
// after a successful push. This header must stay GStreamer-free.
#pragma once

#include <aa/core/Result.hpp>

#include <cstdint>
#include <limits>
#include <optional>

namespace aa::consumer {

// Named-field input for the feed-backlog admission math: the appsrc backlog's
// current byte level plus the incoming AU's payload size. Named fields because
// the two size-like values are otherwise swappable by mistake.
struct FeedLoad final {
    std::uint64_t queued_bytes{};
    std::size_t payload_size{};
};

struct DecodeSessionState final {
    // Feed-backlog limit: the incoming AU's payload counts against it and
    // exactly filling the remaining capacity is permitted.
    static constexpr std::uint64_t kMaxFeedBacklogBytes = 4ULL * 1024 * 1024;

    // GST_CLOCK_TIME_NONE without <gst/gst.h>: the "no timestamp" sentinel is
    // never a valid access-unit timestamp.
    static constexpr std::uint64_t kClockTimeNone = std::numeric_limits<std::uint64_t>::max();

    // High-water mark of the timestamps accepted in this session. Fresh per
    // open() session, advanced ONLY after a successful appsrc push, and
    // preserved across correlation delivery/invalidation: a delivered
    // timestamp is never reusable and there is no timestamp reset at IDR
    // recovery.
    std::optional<std::uint64_t> last_accepted_timestamp_ns{};

    // Feed-backlog admission arithmetic, overflow-free by construction: the
    // payload is rejected against the full limit before the subtraction, so
    // kMaxFeedBacklogBytes - payload_size never underflows and a huge
    // queued_bytes can never wrap into acceptance. Exactly filling the limit
    // is admitted (queued_bytes + payload_size == limit).
    [[nodiscard]] static bool feed_admission_allows(const FeedLoad& load) noexcept {
        const std::uint64_t payload = static_cast<std::uint64_t>(load.payload_size);
        if (payload > kMaxFeedBacklogBytes) {
            return false;
        }
        return load.queued_bytes <= kMaxFeedBacklogBytes - payload;
    }

    // Pre-push admission decision. The timestamp contract is checked first:
    // invalid_argument is a permanent contract violation, ipc_queue_full is a
    // retryable byte-rejection, and the permanent cause is the truthful one
    // when both would fire. Nothing here mutates state: a rejected AU acquires
    // no correlation mapping and does not consume its timestamp, so a
    // byte-rejected AU may be retried with the same timestamp after the
    // backlog drains.
    [[nodiscard]] core::Result<void> admit(const FeedLoad& load,
                                           std::uint64_t timestamp_ns) const {
        if (timestamp_ns == kClockTimeNone
            || (last_accepted_timestamp_ns.has_value()
                && timestamp_ns <= *last_accepted_timestamp_ns)) {
            return core::Result<void>{Error{ErrorCode::invalid_argument}};
        }
        if (!feed_admission_allows(load)) {
            return core::Result<void>{Error{ErrorCode::ipc_queue_full}};
        }
        return core::Result<void>{};
    }

    // Advance the high-water mark after a successful push only; call solely on
    // the admit() -> push -> GST_FLOW_OK path.
    void accept_timestamp(std::uint64_t timestamp_ns) noexcept {
        last_accepted_timestamp_ns = timestamp_ns;
    }
};

} // namespace aa::consumer
