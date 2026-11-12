// SPDX-License-Identifier: GPL-3.0-or-later
// Loss-event formatting/rate limiting for the dummy display's existing
// line-oriented telemetry writer. Counts are cumulative per decode session.
#pragma once

#include <aa/consumer/Decode.hpp>

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace aa::display {

class DecodeLossTelemetry final {
public:
    using Clock = std::chrono::steady_clock;

    // Called when the decode factory opens a new session, before its first
    // observation: equal or greater totals must not reuse the old rate limit.
    void begin_session() noexcept { *this = DecodeLossTelemetry{}; }

    // `final_flush` ignores the rate limit so the last cumulative totals are
    // written before shutdown even if they were coalesced a moment ago.
    [[nodiscard]] std::vector<std::string> observe(const consumer::DecodeReport& report,
                                                   Clock::time_point now,
                                                   bool final_flush = false) {
        std::vector<std::string> lines{};
        observe_one(retention_, "expiry", report.frames_lost_retention,
                    report.awaiting_idr, now, final_flush, lines);
        observe_one(capacity_, "capacity", report.frames_lost_capacity,
                    report.awaiting_idr, now, final_flush, lines);
        observe_one(eos_, "end_of_stream", report.frames_lost_eos,
                    report.awaiting_idr, now, final_flush, lines);
        return lines;
    }

private:
    struct ReasonState final {
        std::uint64_t observed{};
        bool pending{};
        std::optional<Clock::time_point> last_emitted{};
    };

    static constexpr std::chrono::seconds kMinimumLogInterval{1};

    static void observe_one(ReasonState& state, const char* reason, std::uint64_t count,
                            bool awaiting_idr, Clock::time_point now, bool force,
                            std::vector<std::string>& lines) {
        if (count < state.observed) {
            // A new decode session starts its cumulative counters at zero:
            // forget the old session's total and rate-limit window, then
            // treat whatever the new session already lost as a fresh episode.
            state = ReasonState{};
        }
        if (count > state.observed) {
            state.observed = count;
            state.pending = true;
        }
        // Suppressed episodes coalesce into the next line, whose count is
        // still cumulative, so rate limiting never hides the final total.
        if (!state.pending || (!force && state.last_emitted.has_value()
                               && now - *state.last_emitted < kMinimumLogInterval)) {
            return;
        }
        std::string line{"{\"event\":\"decode_loss\",\"reason\":\""};
        line.append(reason);
        line.append("\",\"count\":").append(std::to_string(count));
        line.append(",\"awaiting_idr\":").append(awaiting_idr ? "true" : "false").append("}");
        lines.push_back(std::move(line));
        state.pending = false;
        state.last_emitted = now;
    }

    ReasonState retention_{};
    ReasonState capacity_{};
    ReasonState eos_{};
};

} // namespace aa::display
