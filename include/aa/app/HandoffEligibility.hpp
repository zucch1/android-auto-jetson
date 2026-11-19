// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <aa/helper/BusClient.hpp>
#include <aa/trust/Trust.hpp>
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

// Owner-thread policy; only bounded helper calls run on the worker. Collaborators
// must outlive this object. tick() must run at least every 25ms, including I/O.
class HandoffEligibilityCoordinator final {
public:
    using Clock = std::chrono::steady_clock;
    struct Listener final {
        std::function<core::Result<void>(const helper::ApSnapshot&)> start;
        std::function<void()> stop;
    };
    HandoffEligibilityCoordinator(const trust::TrustStore&, HandoffOperations, Listener);
    ~HandoffEligibilityCoordinator();
    HandoffEligibilityCoordinator(const HandoffEligibilityCoordinator&) = delete;
    HandoffEligibilityCoordinator& operator=(const HandoffEligibilityCoordinator&) = delete;
    void begin(helper::Source source);
    void consumer(std::optional<core::ConsumerId> live);
    void tick(bool active = false);
    void retire();
    // Retires a still-pending bootstrap; ACTIVE and consumer grace require success.
    void confirm_completion();
    [[nodiscard]] bool bootstrap_completed() const;
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
