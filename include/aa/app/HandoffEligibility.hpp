// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <aa/helper/BusClient.hpp>
#include <aa/trust/Trust.hpp>
#include <chrono>
#include <functional>
#include <memory>
#include <optional>

namespace aa::app {
struct HandoffObservation final {
    helper::ObservedPeer peer;
    std::optional<helper::ApSnapshot> snapshot;
};

struct HandoffOperations final {
    struct Validation final {
        helper::Source source;
        std::uint64_t handle{};
        bool credentials_allowed{};
    };
    std::function<core::Result<HandoffObservation>(Validation)> validate;
    std::function<core::Result<void>(helper::Source, std::uint64_t, bool)> authorize;
    // Succeeds only for completed bootstrap; a pending exchange is retired.
    std::function<core::Result<void>(helper::Source, std::uint64_t)> withdraw;
};
HandoffOperations handoff_operations(helper::BusClient& client);

// Bounds of one handoff attempt (#161). Tests shorten them; production uses the defaults.
struct HandoffTiming final {
    // Waiting for the phone's RFCOMM connection after begin().
    std::chrono::milliseconds establishment{10'000};
    // An observed bootstrap, from its first observation: the helper's own budget
    // (Bootstrap::kBudget), which starts earlier, so the receiver never gives up first.
    std::chrono::milliseconds bootstrap{10'000};
    // After that budget, for the phone to open the TCP session of a completed exchange.
    std::chrono::milliseconds connect{5'000};
};

// Owner-thread policy; only bounded helper calls run on the worker. Collaborators
// must outlive this object. tick() must run at least every 25ms, including I/O.
class HandoffEligibilityCoordinator final {
public:
    using Clock = std::chrono::steady_clock;
    struct Listener final {
        std::function<core::Result<void>(const helper::ApSnapshot&)> start;
        std::function<void()> stop;
    };
    using Timing = HandoffTiming;
    HandoffEligibilityCoordinator(const trust::TrustStore&, HandoffOperations, Listener, Timing = {});
    ~HandoffEligibilityCoordinator();
    HandoffEligibilityCoordinator(const HandoffEligibilityCoordinator&) = delete;
    HandoffEligibilityCoordinator& operator=(const HandoffEligibilityCoordinator&) = delete;
    // Starts an attempt. A retry with the same source while this coordinator's bootstrap is
    // still in progress keeps it (no retirement, no denial, no re-observation, #161). A fresh
    // attempt's caller first retires the previous one with retire(), which settles its denial
    // with the helper; begin()'s own retirement only queues it (#169 review).
    void begin(helper::Source source);
    void consumer(std::optional<core::ConsumerId> live);
    void tick(bool active = false);
    // Retires the attempt and, before returning, sends its denial to the helper (after any
    // operation still in flight), so a fresh attempt never starts against the retired one.
    void retire();
    // Retires a still-pending bootstrap; ACTIVE and consumer grace require success.
    void confirm_completion();
    [[nodiscard]] bool bootstrap_completed() const;
    // An observed bootstrap is preparing, authorizing, or authorized but not yet confirmed,
    // within its bounded hold (Timing::bootstrap + Timing::connect from its observation).
    // The attempt must not be deferred or retried meanwhile (#161).
    [[nodiscard]] bool bootstrapping() const;
    [[nodiscard]] bool admitting() const;
    [[nodiscard]] bool valid_session() const;
    [[nodiscard]] std::optional<trust::PhoneIdentity> phone() const;
    [[nodiscard]] std::uint64_t epoch() const;
    // Generation of the validated AP activation, or 0 without one.
    [[nodiscard]] std::uint64_t snapshot_generation() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
