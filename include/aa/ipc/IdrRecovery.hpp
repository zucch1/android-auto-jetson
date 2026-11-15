// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace aa::ipc {

inline constexpr std::chrono::milliseconds kIdrRequestMinInterval{250};
inline constexpr std::chrono::milliseconds kVideoSelectionWarningTimeout{5000};

// Coordinates rate-limited video IDR requests and recovery latency tracking.
// Callers supply monotonic timestamps and projection state so tests remain
// deterministic without sleeping.
class IdrRecoveryCoordinator final {
public:
    using Clock = std::chrono::steady_clock;

    explicit IdrRecoveryCoordinator(
        std::chrono::milliseconds min_interval = kIdrRequestMinInterval) noexcept
        : min_interval_(min_interval) {}

    struct RecoveryResult final {
        bool recovered{false};
        std::int64_t latency_ms{0};
    };

    void begin_wait(Clock::time_point now) noexcept {
        if (!waiting_) {
            waiting_ = true;
            wait_started_at_ = now;
        }
    }

    [[nodiscard]] bool try_request(Clock::time_point now, bool projecting) noexcept {
        if (!waiting_ || !projecting) {
            return false;
        }
        if (last_request_at_.has_value() && (now - *last_request_at_) < min_interval_) {
            return false;
        }
        last_request_at_ = now;
        ++requests_sent_;
        return true;
    }

    void cancel_last_request(std::optional<Clock::time_point> previous_request_at) noexcept {
        last_request_at_ = previous_request_at;
        if (requests_sent_ > 0) {
            --requests_sent_;
        }
    }

    [[nodiscard]] RecoveryResult on_idr_delivered(Clock::time_point now) noexcept {
        if (!waiting_) {
            return {};
        }
        waiting_ = false;
        const auto raw_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(now - wait_started_at_).count();
        const auto latency_ms = raw_ms < 0 ? std::int64_t{0} : static_cast<std::int64_t>(raw_ms);
        last_request_at_.reset();
        return {true, latency_ms};
    }

    void reset() noexcept {
        waiting_ = false;
        wait_started_at_ = {};
        last_request_at_.reset();
    }

    [[nodiscard]] bool waiting() const noexcept { return waiting_; }
    [[nodiscard]] std::uint64_t requests_sent() const noexcept { return requests_sent_; }
    [[nodiscard]] std::optional<Clock::time_point> last_request_at() const noexcept {
        return last_request_at_;
    }

private:
    std::chrono::milliseconds min_interval_{kIdrRequestMinInterval};
    bool waiting_{false};
    Clock::time_point wait_started_at_{};
    std::optional<Clock::time_point> last_request_at_{};
    std::uint64_t requests_sent_{0};
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

[[nodiscard]] inline std::string idr_recovered_json(std::string_view source,
                                                    std::int64_t latency_ms,
                                                    std::uint64_t requests) {
    return std::string{"{\"event\":\"idr_recovered\",\"source\":\""} + std::string{source} +
           "\",\"latency_ms\":" + std::to_string(latency_ms) +
           ",\"requests\":" + std::to_string(requests) + "}";
}

[[nodiscard]] inline std::string video_selection_timeout_json(
    std::chrono::milliseconds elapsed, std::size_t offered_count) {
    return "{\"event\":\"video_selection_timeout\",\"elapsed_ms\":" +
           std::to_string(elapsed.count()) + ",\"offered_profiles\":" +
           std::to_string(offered_count) + "}";
}

} // namespace aa::ipc
