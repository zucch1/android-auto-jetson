// SPDX-License-Identifier: GPL-3.0-or-later
// GStreamer-free session admission state for the task-23 consumer decode
// helper (PR #47 fix round, findings 3+4). The two pre-push admission
// contracts live here so tests can drive them deterministically, without
// racing appsrc: (1) feed-backlog admission counts the incoming AU against
// the backlog limit, and (2) session timestamps are valid, strictly
// increasing and session-unique behind a high-water mark that advances only
// after a successful push. The correlation lifecycle lives here too (same
// round): bounded-age pending correlations with an IDR resync. Every aging
// operation takes `now` explicitly (production passes Clock::now(), tests
// pass controlled times) and the public API has no clock knobs or fault
// injection. This header must stay GStreamer-free.
#pragma once

#include <aa/core/Result.hpp>

#include <chrono>
#include <cstdint>
#include <limits>
#include <map>
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
    // Correlation aging clock; operations take its `now` explicitly, so tests
    // drive the exact same methods with controlled times (no clock knobs).
    using Clock = std::chrono::steady_clock;

    // Feed-backlog limit: the incoming AU's payload counts against it and
    // exactly filling the remaining capacity is permitted.
    static constexpr std::uint64_t kMaxFeedBacklogBytes = 4ULL * 1024 * 1024;

    // GST_CLOCK_TIME_NONE without <gst/gst.h>: the "no timestamp" sentinel is
    // never a valid access-unit timestamp.
    static constexpr std::uint64_t kClockTimeNone = std::numeric_limits<std::uint64_t>::max();

    // Fixed FIVE-SECOND correlation retention timeout. This is an OPERATIONAL
    // RETENTION POLICY - how long a submitted AU's framing metadata stays
    // available to correlate with a decoded output - and is NOT a latency
    // budget and NOT a decode qualification claim (task-9 reduced scope). An
    // entry whose age (now - submitted_at) reaches this bound expires the
    // ENTIRE pending set and forces an IDR resync.
    static constexpr std::chrono::seconds kCorrelationRetention{5};

    // Outstanding-correlation cap. A byte-admissible submission meeting this
    // many undelivered entries cannot pretend continuity: the set is
    // invalidated and the session resyncs at an IDR instead of wedging
    // submit() on a permanently full map.
    static constexpr std::size_t kMaxPendingCorrelations = 512;

    // One outstanding submitted-but-undelivered AU: the task-22 framing
    // metadata a decoded output must carry back unchanged, plus the wall age
    // anchor for the retention bound.
    struct PendingCorrelation final {
        std::uint64_t frame{};
        std::uint64_t sequence{};
        std::uint64_t timestamp_ns{};
        Clock::time_point submitted_at{};
    };

    // High-water mark of the timestamps accepted in this session. Fresh per
    // open() session, advanced ONLY after a successful appsrc push, and
    // preserved across correlation delivery/invalidation: a delivered
    // timestamp is never reusable and there is no timestamp reset at IDR
    // recovery.
    std::optional<std::uint64_t> last_accepted_timestamp_ns{};

    // Undelivered correlations keyed by access-unit timestamp. Entries are
    // released on delivery, or sacrificed as a whole on invalidation.
    std::map<std::uint64_t, PendingCorrelation> pending{};

    // Set by invalidation (expiry or capacity), cleared only by a successful
    // post-GST_FLOW_OK recovery record. While set, non-IDR AUs are discarded
    // and no non-IDR submission acquires a mapping.
    bool awaiting_idr{};

    // Terminal runtime failure latch (contract on Decode.hpp): the first
    // fatal error observed after open() is remembered and every later
    // active-session operation fails with it without touching the pipeline.
    // First latch wins; correlation metadata goes with the failed session.
    std::optional<ErrorCode> terminal_error{};

    void latch_terminal(ErrorCode code) noexcept {
        if (!terminal_error.has_value()) {
            terminal_error = code;
            clear_correlations();
        }
    }

    [[nodiscard]] bool terminal() const noexcept { return terminal_error.has_value(); }

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
    // Immutable timestamp contract: the sentinel is never valid and accepted
    // timestamps are strictly increasing behind the session high-water mark.
    // Pure - the rejected-AU side-effect freedom of admit_submission() relies
    // on this being checkable before any state change.
    [[nodiscard]] bool timestamp_admissible(std::uint64_t timestamp_ns) const noexcept {
        return timestamp_ns != kClockTimeNone
            && (!last_accepted_timestamp_ns.has_value()
                || timestamp_ns > *last_accepted_timestamp_ns);
    }

    [[nodiscard]] core::Result<void> admit(const FeedLoad& load,
                                           std::uint64_t timestamp_ns) const {
        if (!timestamp_admissible(timestamp_ns)) {
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

    // True when any outstanding entry reached the retention bound
    // (now - submitted_at >= kCorrelationRetention).
    [[nodiscard]] bool correlations_expired(Clock::time_point now) const noexcept {
        for (const auto& item : pending) {
            if (now - item.second.submitted_at >= kCorrelationRetention) {
                return true;
            }
        }
        return false;
    }

    // Conservative invalidation (expiry and capacity both land here): throw
    // away the ENTIRE pending set and require an IDR before further non-IDR
    // admission. Outstanding callbacks are sacrificed rather than pretending
    // continuity across an unknown decode gap. The timestamp high-water mark
    // is NOT reset (no timestamp reset at IDR recovery) and nothing here
    // switches backends or rebuilds pipelines.
    void invalidate_correlations() noexcept {
        pending.clear();
        awaiting_idr = true;
    }

    // Drop undelivered correlations without changing the resync state: used
    // when appsink reaches EOS (nothing can match anymore) and on close().
    void clear_correlations() noexcept {
        pending.clear();
    }

    // Retention-bound evaluation: invalidate when any entry aged out. Called
    // at submit() entry, during pump() and again after a blocking sample pull
    // before that sample is correlated (a long pump wait must not deliver
    // expired metadata).
    void expire_stale_correlations(Clock::time_point now) noexcept {
        if (correlations_expired(now)) {
            invalidate_correlations();
        }
    }

    // Full pre-submit correlation gate, mutating, run before the push. Order
    // is policy:
    //  (a) expire stale correlations at entry (the retention policy is
    //      preserved: genuinely expired mappings are still sacrificed);
    //  (b) the immutable timestamp contract runs BEFORE any state change: a
    //      duplicate/regressing/sentinel timestamp is a permanent
    //      invalid_argument and leaves the pending set and the resync state
    //      untouched - a rejected invalid AU is side-effect free;
    //  (c) a byte-admissible submission meeting a saturated pending set
    //      invalidates the whole set and resyncs BEFORE the admission
    //      decision - the set can never stay wedged at the cap;
    //  (d) while awaiting_idr a non-IDR AU is discarded with the EXISTING
    //      ipc_consumer_rejected: "discard this AU and resume at an IDR".
    //      This is NOT a retryable queue-full condition.
    //  (e) an IDR (including a recovery IDR) is still subject to the
    //      timestamp + byte admission below; admit() semantics are unchanged.
    [[nodiscard]] core::Result<void> admit_submission(const FeedLoad& load,
                                                      std::uint64_t timestamp_ns, bool idr,
                                                      Clock::time_point now) {
        expire_stale_correlations(now);
        if (!timestamp_admissible(timestamp_ns)) {
            return core::Result<void>{Error{ErrorCode::invalid_argument}};
        }
        if (pending.size() >= kMaxPendingCorrelations && feed_admission_allows(load)) {
            invalidate_correlations();
        }
        if (awaiting_idr && !idr) {
            return core::Result<void>{Error{ErrorCode::ipc_consumer_rejected}};
        }
        return admit(load, timestamp_ns);
    }

    // Acquire the correlation mapping for an admitted AU. Call solely on the
    // admit_submission() -> push -> GST_FLOW_OK path: the mapping is inserted
    // (and awaiting_idr cleared) only once the buffer was really accepted, so
    // a failed recovery push leaves the resync state armed. An ordinary IDR
    // submitted while NOT recovering only ADDS its mapping - the outstanding
    // set is never cleared here; only expiry/capacity invalidation sacrifices
    // it.
    void record_correlation(std::uint64_t frame, std::uint64_t sequence,
                            std::uint64_t timestamp_ns, Clock::time_point now) noexcept {
        pending.emplace(timestamp_ns, PendingCorrelation{frame, sequence, timestamp_ns, now});
        awaiting_idr = false;
    }

    // Correlate one decoded output PTS: erase and return its mapping, or
    // nullopt when no mapping exists (late pre-recovery output, expired or
    // invalidated metadata, unmatched PTS). The caller counts unmatched
    // outputs and releases the sample WITHOUT a callback on nullopt.
    [[nodiscard]] std::optional<PendingCorrelation> take_correlation(
        std::uint64_t timestamp_ns) noexcept {
        const auto it = pending.find(timestamp_ns);
        if (it == pending.end()) {
            return std::nullopt;
        }
        const PendingCorrelation meta = it->second;
        pending.erase(it);
        return meta;
    }

    // Non-destructive lookup (liveness/existence checks).
    [[nodiscard]] const PendingCorrelation* find_correlation(
        std::uint64_t timestamp_ns) const noexcept {
        const auto it = pending.find(timestamp_ns);
        return it == pending.end() ? nullptr : &it->second;
    }

    [[nodiscard]] std::size_t pending_count() const noexcept {
        return pending.size();
    }
};

} // namespace aa::consumer
