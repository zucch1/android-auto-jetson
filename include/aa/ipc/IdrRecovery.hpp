// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>

namespace aa::ipc {

inline constexpr std::chrono::milliseconds kIdrRequestMinInterval{250};
inline constexpr std::chrono::milliseconds kIdrFailureBackoffCap{2000};
inline constexpr std::chrono::milliseconds kIdrEscalationDelay{1000};
inline constexpr std::uint64_t kIdrEscalationMaxNudges{3};
inline constexpr std::chrono::milliseconds kIdrTransitionCooldown{5000};
inline constexpr std::chrono::milliseconds kIdrStartupGrace{1000};
inline constexpr std::chrono::milliseconds kVideoSelectionWarningTimeout{5000};

enum class IdrRecoveryStrategy : std::uint8_t {
    auto_escalate,
    nudge,
    transition,
};

enum class IdrRecoveryStep : std::uint8_t {
    none,
    nudge,
    focus_transition,
};

[[nodiscard]] inline bool parse_idr_recovery_strategy(std::string_view value,
                                                      IdrRecoveryStrategy& out) noexcept {
    if (value == "auto") {
        out = IdrRecoveryStrategy::auto_escalate;
        return true;
    }
    if (value == "nudge") {
        out = IdrRecoveryStrategy::nudge;
        return true;
    }
    if (value == "transition") {
        out = IdrRecoveryStrategy::transition;
        return true;
    }
    return false;
}

[[nodiscard]] inline std::string_view idr_recovery_strategy_name(
    IdrRecoveryStrategy strategy) noexcept {
    switch (strategy) {
    case IdrRecoveryStrategy::auto_escalate:
        return "auto";
    case IdrRecoveryStrategy::nudge:
        return "nudge";
    case IdrRecoveryStrategy::transition:
        return "transition";
    }
    return "auto";
}

[[nodiscard]] inline std::string_view idr_recovery_step_name(IdrRecoveryStep step) noexcept {
    switch (step) {
    case IdrRecoveryStep::none:
        return "none";
    case IdrRecoveryStep::nudge:
        return "nudge";
    case IdrRecoveryStep::focus_transition:
        return "focus_transition";
    }
    return "none";
}

// Coordinates rate-limited video IDR requests, bounded focus-transition escalation,
// and recovery latency tracking. Callers supply monotonic timestamps and projection
// state so tests remain deterministic without sleeping.
//
// Endpoint latency semantics:
// - Producer `idr_recovered` `latency_ms` measures from `begin_wait()` until the
//   producer dispatches the recovery IDR onto the IPC socket (`SendStatus::sent`).
// - Consumer `idr_recovered` `latency_ms` measures from `begin_wait()` until the
//   decoder exits `awaiting_idr` (and socket resync clears) on IDR receipt.
class IdrRecoveryCoordinator final {
public:
    using Clock = std::chrono::steady_clock;

    struct Config final {
        std::chrono::milliseconds min_interval{kIdrRequestMinInterval};
        std::chrono::milliseconds escalation_delay{kIdrEscalationDelay};
        std::uint64_t escalation_max_nudges{kIdrEscalationMaxNudges};
        std::chrono::milliseconds transition_cooldown{kIdrTransitionCooldown};
        IdrRecoveryStrategy strategy{IdrRecoveryStrategy::auto_escalate};
    };

    struct RequestSnapshot final {
        std::optional<Clock::time_point> last_request_at{};
        std::optional<Clock::time_point> first_nudge_at{};
        std::optional<Clock::time_point> last_transition_at{};
        std::uint64_t episode_requests{0};
        std::uint64_t episode_nudges{0};
        std::uint64_t total_requests{0};
        bool escalated{false};
    };

    struct RecoveryResult final {
        bool recovered{false};
        std::int64_t latency_ms{0};
        std::uint64_t episode_requests{0};
        std::uint64_t total_requests{0};
    };

    IdrRecoveryCoordinator() noexcept = default;
    explicit IdrRecoveryCoordinator(std::chrono::milliseconds min_interval) noexcept
        : config_{min_interval} {}
    explicit IdrRecoveryCoordinator(Config config) noexcept : config_(config) {}

    void set_strategy(IdrRecoveryStrategy strategy) noexcept { config_.strategy = strategy; }
    [[nodiscard]] IdrRecoveryStrategy strategy() const noexcept { return config_.strategy; }

    void begin_wait(Clock::time_point now) noexcept {
        if (!waiting_) {
            waiting_ = true;
            wait_started_at_ = now;
            first_nudge_at_.reset();
            episode_requests_ = 0;
            episode_nudges_ = 0;
            escalated_ = false;
        }
    }

    [[nodiscard]] IdrRecoveryStep try_request_step(Clock::time_point now,
                                                    bool projecting) noexcept {
        if (!waiting_ || !projecting) {
            return IdrRecoveryStep::none;
        }
        if (last_request_at_.has_value() && (now - *last_request_at_) < config_.min_interval) {
            return IdrRecoveryStep::none;
        }
        if (retry_after_.has_value() && now < *retry_after_) {
            return IdrRecoveryStep::none;
        }

        const bool transition_ready =
            !last_transition_at_.has_value() ||
            (now - *last_transition_at_) >= config_.transition_cooldown;

        IdrRecoveryStep chosen = IdrRecoveryStep::none;
        switch (config_.strategy) {
        case IdrRecoveryStrategy::nudge:
            chosen = IdrRecoveryStep::nudge;
            break;
        case IdrRecoveryStrategy::transition:
            if (!transition_ready) {
                return IdrRecoveryStep::none;
            }
            chosen = IdrRecoveryStep::focus_transition;
            break;
        case IdrRecoveryStrategy::auto_escalate: {
            const bool elapsed_exceeded =
                first_nudge_at_.has_value() &&
                (now - *first_nudge_at_) >= config_.escalation_delay;
            const bool nudges_exceeded = episode_nudges_ >= config_.escalation_max_nudges;
            if (episode_nudges_ > 0 && (elapsed_exceeded || nudges_exceeded) && transition_ready) {
                chosen = IdrRecoveryStep::focus_transition;
            } else {
                chosen = IdrRecoveryStep::nudge;
            }
            break;
        }
        }

        pending_attempt_ = PendingAttempt{snapshot(), consecutive_failures_};
        consecutive_failures_ = 0;
        retry_after_.reset();
        last_request_at_ = now;
        ++episode_requests_;
        ++total_requests_;
        if (chosen == IdrRecoveryStep::nudge) {
            if (!first_nudge_at_.has_value()) {
                first_nudge_at_ = now;
            }
            ++episode_nudges_;
        } else if (chosen == IdrRecoveryStep::focus_transition) {
            last_transition_at_ = now;
            escalated_ = true;
        }
        return chosen;
    }

    [[nodiscard]] bool try_request(Clock::time_point now, bool projecting) noexcept {
        return try_request_step(now, projecting) != IdrRecoveryStep::none;
    }

    [[nodiscard]] RequestSnapshot snapshot() const noexcept {
        return {last_request_at_, first_nudge_at_, last_transition_at_, episode_requests_,
                episode_nudges_,  total_requests_, escalated_};
    }

    void restore_snapshot(const RequestSnapshot& prev) noexcept {
        last_request_at_ = prev.last_request_at;
        first_nudge_at_ = prev.first_nudge_at;
        last_transition_at_ = prev.last_transition_at;
        episode_requests_ = prev.episode_requests;
        episode_nudges_ = prev.episode_nudges;
        total_requests_ = prev.total_requests;
        escalated_ = prev.escalated;
        pending_attempt_.reset();
    }

    void note_request_failed(Clock::time_point now) noexcept {
        if (pending_attempt_.has_value()) {
            const auto attempt = *pending_attempt_;
            restore_snapshot(attempt.snapshot);
            consecutive_failures_ = attempt.prior_failures;
        } else if (!waiting_) {
            return;
        }
        const std::uint32_t shift = std::min<std::uint32_t>(consecutive_failures_, 3U);
        const auto cap = std::max(config_.min_interval, kIdrFailureBackoffCap);
        auto backoff = config_.min_interval;
        for (std::uint32_t i = 0; i < shift && backoff < cap; ++i) {
            backoff = (backoff <= cap / 2) ? (backoff * 2) : cap;
        }
        if (consecutive_failures_ < std::numeric_limits<std::uint32_t>::max()) {
            ++consecutive_failures_;
        }
        retry_after_ = now + backoff;
    }

    [[nodiscard]] RecoveryResult on_idr_delivered(Clock::time_point now) noexcept {
        pending_attempt_.reset();
        consecutive_failures_ = 0;
        retry_after_.reset();
        if (!waiting_) {
            return {};
        }
        waiting_ = false;
        const auto raw_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(now - wait_started_at_).count();
        const auto latency_ms = raw_ms < 0 ? std::int64_t{0} : static_cast<std::int64_t>(raw_ms);
        const auto ep_requests = episode_requests_;
        // Preserve last_request_at_ and last_transition_at_ across episode
        // boundaries so phone-bound request cooldowns cannot be bypassed by
        // rapid IDR/drop cycling or interleaved D-Bus RequestIdrRecovery calls.
        wait_started_at_ = {};
        first_nudge_at_.reset();
        episode_requests_ = 0;
        episode_nudges_ = 0;
        escalated_ = false;
        return {true, latency_ms, ep_requests, total_requests_};
    }

    void reset() noexcept {
        waiting_ = false;
        wait_started_at_ = {};
        first_nudge_at_.reset();
        episode_requests_ = 0;
        episode_nudges_ = 0;
        escalated_ = false;
        pending_attempt_.reset();
        consecutive_failures_ = 0;
        retry_after_.reset();
    }

    [[nodiscard]] bool waiting() const noexcept { return waiting_; }
    [[nodiscard]] bool escalated() const noexcept { return escalated_; }
    [[nodiscard]] std::uint64_t episode_requests() const noexcept { return episode_requests_; }
    [[nodiscard]] std::uint64_t total_requests() const noexcept { return total_requests_; }
    [[nodiscard]] std::uint64_t requests_sent() const noexcept { return total_requests_; }
    [[nodiscard]] std::optional<Clock::time_point> last_request_at() const noexcept {
        return last_request_at_;
    }
    [[nodiscard]] std::optional<Clock::time_point> last_transition_at() const noexcept {
        return last_transition_at_;
    }
    [[nodiscard]] std::optional<Clock::time_point> retry_after() const noexcept {
        return retry_after_;
    }
    [[nodiscard]] std::uint32_t consecutive_failures() const noexcept {
        return consecutive_failures_;
    }

private:
    struct PendingAttempt final {
        RequestSnapshot snapshot{};
        std::uint32_t prior_failures{0};
    };

    Config config_{};
    bool waiting_{false};
    Clock::time_point wait_started_at_{};
    std::optional<Clock::time_point> last_request_at_{};
    std::optional<Clock::time_point> first_nudge_at_{};
    std::optional<Clock::time_point> last_transition_at_{};
    std::optional<Clock::time_point> retry_after_{};
    std::optional<PendingAttempt> pending_attempt_{};
    std::uint64_t episode_requests_{0};
    std::uint64_t episode_nudges_{0};
    std::uint64_t total_requests_{0};
    std::uint32_t consecutive_failures_{0};
    bool escalated_{false};
};

// Emits a one-shot warning when an active session runs for `timeout` without
// receiving an AvSinkStart video profile selection from the phone.
class VideoSelectionWatchdog final {
public:
    using Clock = std::chrono::steady_clock;

    explicit VideoSelectionWatchdog(
        std::chrono::milliseconds timeout = kVideoSelectionWarningTimeout) noexcept
        : timeout_(timeout) {}

    [[nodiscard]] bool check(bool session_active, bool has_selection,
                             Clock::time_point now) noexcept {
        if (!session_active || has_selection) {
            armed_at_.reset();
            warned_ = false;
            return false;
        }
        if (!armed_at_.has_value()) {
            armed_at_ = now;
            return false;
        }
        if (!warned_ && (now - *armed_at_) >= timeout_) {
            warned_ = true;
            return true;
        }
        return false;
    }

    void reset() noexcept {
        armed_at_.reset();
        warned_ = false;
    }

    [[nodiscard]] bool warned() const noexcept { return warned_; }

private:
    std::chrono::milliseconds timeout_{kVideoSelectionWarningTimeout};
    std::optional<Clock::time_point> armed_at_{};
    bool warned_{false};
};

[[nodiscard]] inline std::string idr_requested_json(std::string_view source,
                                                    std::string_view reason) {
    return std::string{"{\"event\":\"idr_requested\",\"source\":\""} + std::string{source} +
           "\",\"reason\":\"" + std::string{reason} + "\"}";
}

[[nodiscard]] inline std::string idr_escalated_json(std::string_view source,
                                                    std::string_view step,
                                                    std::string_view reason,
                                                    std::uint64_t episode_requests,
                                                    std::uint64_t total_requests) {
    return std::string{"{\"event\":\"idr_escalated\",\"source\":\""} + std::string{source} +
           "\",\"step\":\"" + std::string{step} +
           "\",\"reason\":\"" + std::string{reason} +
           "\",\"episode_requests\":" + std::to_string(episode_requests) +
           ",\"total_requests\":" + std::to_string(total_requests) + "}";
}

[[nodiscard]] inline std::string idr_transition_failed_json(std::string_view source,
                                                            std::string_view step,
                                                            std::string_view reason,
                                                            std::uint64_t episode_requests,
                                                            std::uint64_t total_requests,
                                                            std::string_view error) {
    return std::string{"{\"event\":\"idr_transition_failed\",\"source\":\""} + std::string{source} +
           "\",\"step\":\"" + std::string{step} +
           "\",\"reason\":\"" + std::string{reason} +
           "\",\"error\":\"" + std::string{error} +
           "\",\"episode_requests\":" + std::to_string(episode_requests) +
           ",\"total_requests\":" + std::to_string(total_requests) + "}";
}

// Producer `latency_ms` tracks wait -> IPC socket send (`SendStatus::sent`);
// consumer `latency_ms` tracks wait -> decoder exit of `awaiting_idr`.
[[nodiscard]] inline std::string idr_recovered_json(std::string_view source,
                                                    std::int64_t latency_ms,
                                                    std::uint64_t episode_requests,
                                                    std::uint64_t total_requests) {
    return std::string{"{\"event\":\"idr_recovered\",\"source\":\""} + std::string{source} +
           "\",\"latency_ms\":" + std::to_string(latency_ms) +
           ",\"episode_requests\":" + std::to_string(episode_requests) +
           ",\"total_requests\":" + std::to_string(total_requests) + "}";
}

[[nodiscard]] inline std::string video_selection_timeout_json(
    std::chrono::milliseconds elapsed, std::size_t offered_count) {
    return "{\"event\":\"video_selection_timeout\",\"elapsed_ms\":" +
           std::to_string(elapsed.count()) + ",\"offered_profiles\":" +
           std::to_string(offered_count) + "}";
}

} // namespace aa::ipc
