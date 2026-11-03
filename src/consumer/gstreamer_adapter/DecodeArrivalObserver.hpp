// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "DecodeSessionState.hpp"

#include <array>
#include <mutex>

namespace aa::consumer {

// Scalar-only diagnostic state, independent of session admission/correlation.
// No allocation, payload ownership, or GStreamer calls under this mutex.
class DecodeArrivalObserver final {
public:
    using Clock = DecodeSessionState::Clock;
    static constexpr auto kRetention = DecodeSessionState::kCorrelationRetention;
    static constexpr auto kCapacity = DecodeSessionState::kMaxPendingCorrelations;

    void observe(std::uint64_t pts, Clock::time_point now) noexcept {
        if (pts == DecodeSessionState::kClockTimeNone) {
            return;
        }
        const std::lock_guard lock{mutex_};
        expire(now);
        Entry* slot = nullptr;
        Entry* oldest = &entries_[0];
        for (auto& entry : entries_) {
            if (entry.occupied && entry.pts == pts) {
                // Duplicate PTS is ambiguous while this entry is retained.
                // Keep its tombstone after take; expiry/eviction forgets it.
                entry.arrival.reset();
                return;
            }
            if (!entry.occupied) {
                slot = &entry;
            } else if (entry.first_seen < oldest->first_seen) {
                oldest = &entry;
            }
        }
        // At capacity sacrifice the oldest observation, never a video sample.
        if (slot == nullptr) {
            slot = oldest;
        }
        *slot = Entry{pts, now, now, true};
    }

    [[nodiscard]] std::optional<Clock::time_point> take(
        std::uint64_t pts, Clock::time_point submitted, Clock::time_point pulled) noexcept {
        const std::lock_guard lock{mutex_};
        expire(pulled);
        for (auto& entry : entries_) {
            if (entry.occupied && entry.pts == pts) {
                const auto arrival = entry.arrival;
                entry.arrival.reset();
                if (arrival && *arrival >= submitted && *arrival <= pulled) {
                    return arrival;
                }
                break;
            }
        }
        return std::nullopt;
    }

    void clear() noexcept {
        const std::lock_guard lock{mutex_};
        entries_ = {};
    }

private:
    struct Entry final {
        std::uint64_t pts{};
        Clock::time_point first_seen{};
        std::optional<Clock::time_point> arrival{};
        bool occupied{};
    };

    void expire(Clock::time_point now) noexcept {
        for (auto& entry : entries_) {
            if (entry.occupied && now - entry.first_seen >= kRetention) {
                entry = {};
            }
        }
    }

    std::mutex mutex_;
    std::array<Entry, kCapacity> entries_{};
};

} // namespace aa::consumer
