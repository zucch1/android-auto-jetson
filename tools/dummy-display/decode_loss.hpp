// SPDX-License-Identifier: GPL-3.0-or-later
// Loss-event formatting/rate limiting for the dummy display's existing
// line-oriented telemetry writer. Counts are cumulative per decode session.
#pragma once

#include <aa/consumer/Decode.hpp>
#include <aa/display/Display.hpp>

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace aa::display {

class DecodeLossTelemetry final {
public:
    using Clock = std::chrono::steady_clock;

    // Called when the decode factory opens a new session, before its first
    // observation: flush any un-emitted pending totals from the prior session,
    // then reset so equal or greater totals do not reuse the old rate limit.
    std::vector<std::string> begin_session() {
        auto lines = flush();
        *this = DecodeLossTelemetry{};
        return lines;
    }

    // Flushes any rate-limited pending totals (for example at session teardown)
    // without needing a live DecodeReport.
    [[nodiscard]] std::vector<std::string> flush(Clock::time_point now = Clock::now()) {
        std::vector<std::string> lines{};
        flush_one(retention_, "expiry", now, lines);
        flush_one(capacity_, "capacity", now, lines);
        flush_one(eos_, "end_of_stream", now, lines);
        return lines;
    }

    // Captures any final report updates, flushes pending totals unconditionally,
    // and resets state for the next decode session.
    [[nodiscard]] std::vector<std::string> end_session(const consumer::DecodeReport& report,
                                                       Clock::time_point now = Clock::now()) {
        auto lines = observe(report, now, true);
        *this = DecodeLossTelemetry{};
        return lines;
    }

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
        bool awaiting_idr{};
        std::optional<Clock::time_point> last_emitted{};
    };

    static constexpr std::chrono::seconds kMinimumLogInterval{1};

    static std::string format_line(const char* reason, std::uint64_t count, bool awaiting_idr) {
        std::string line{"{\"event\":\"decode_loss\",\"reason\":\""};
        line.append(reason);
        line.append("\",\"count\":").append(std::to_string(count));
        line.append(",\"awaiting_idr\":").append(awaiting_idr ? "true" : "false").append("}");
        return line;
    }

    static void flush_one(ReasonState& state, const char* reason, Clock::time_point now,
                          std::vector<std::string>& lines) {
        if (!state.pending) {
            return;
        }
        lines.push_back(format_line(reason, state.observed, state.awaiting_idr));
        state.pending = false;
        state.last_emitted = now;
    }

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
        if (state.pending) {
            state.awaiting_idr = awaiting_idr;
        }
        // Suppressed episodes coalesce into the next line, whose count is
        // still cumulative, so rate limiting never hides the final total.
        if (!state.pending || (!force && state.last_emitted.has_value()
                               && now - *state.last_emitted < kMinimumLogInterval)) {
            return;
        }
        lines.push_back(format_line(reason, state.observed, state.awaiting_idr));
        state.pending = false;
        state.last_emitted = now;
    }

    ReasonState retention_{};
    ReasonState capacity_{};
    ReasonState eos_{};
};

// Wraps a DecodeSession so media-session teardown flushes any pending
// decode_loss totals before DisplayController releases the decoder and its
// report becomes inaccessible.
class LossReportingDecodeSession final : public DecodeSession {
public:
    using LineSink = std::function<void(std::string)>;

    LossReportingDecodeSession(std::unique_ptr<DecodeSession> inner, DecodeLossTelemetry& loss,
                               LineSink sink) noexcept
        : inner_(std::move(inner)), loss_(loss), sink_(std::move(sink)) {}

    ~LossReportingDecodeSession() override { flush_on_teardown(); }

    [[nodiscard]] core::Result<void> submit(const ipc::VideoAccessUnit& unit) override {
        return inner_->submit(unit);
    }
    [[nodiscard]] core::Result<void> end_of_stream() override { return inner_->end_of_stream(); }
    [[nodiscard]] core::Result<std::size_t> pump(core::Milliseconds wait) override {
        return inner_->pump(wait);
    }
    [[nodiscard]] const consumer::DecodeReport& report() const noexcept override {
        return inner_->report();
    }
    void close() noexcept override {
        flush_on_teardown();
        if (inner_) {
            inner_->close();
        }
    }

private:
    void flush_on_teardown() noexcept {
        if (flushed_ || !inner_) {
            return;
        }
        flushed_ = true;
        try {
            for (auto& line :
                 loss_.end_session(inner_->report(), DecodeLossTelemetry::Clock::now())) {
                if (sink_) {
                    sink_(std::move(line));
                }
            }
        } catch (...) {
        }
    }

    std::unique_ptr<DecodeSession> inner_;
    DecodeLossTelemetry& loss_;
    LineSink sink_;
    bool flushed_{false};
};

} // namespace aa::display
