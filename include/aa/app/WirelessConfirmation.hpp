// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <aa/helper/Confirmation.hpp>
#include <chrono>
#include <functional>

namespace aa::app {
// Receiver-owner-thread adapter passed to ControlService instead of BusClient.
// Registration is still revoked immediately; only destructive helper cleanup is
// deferred for an already-active session. flush() runs outside transport I/O.
class WirelessConfirmation final : public helper::Confirmation {
public:
    WirelessConfirmation(helper::Confirmation& target, std::function<bool()> active,
                         std::function<void(core::ConsumerId)> lost)
        : target_(target), active_(std::move(active)), lost_(std::move(lost)) {}
    ~WirelessConfirmation() override { flush(true); }
    core::Result<void> confirmed(core::ConsumerId generation, core::PairingRequestId source) override {
        flush();
        if (deferred_) return Error{ErrorCode::helper_rejected};
        return target_.confirmed(generation, source);
    }
    core::Result<std::optional<helper::PeerIdentity>> prepare(core::ConsumerId generation,
                                                            core::PairingRequestId source) override {
        flush();
        if (deferred_) return Error{ErrorCode::helper_rejected};
        return target_.prepare(generation, source);
    }
    core::Result<helper::PairingComparison> comparison(core::ConsumerId generation,
                                                        core::PairingRequestId source) override {
        flush();
        if (deferred_) return Error{ErrorCode::helper_rejected};
        return target_.comparison(generation, source);
    }
    core::Result<void> respond_comparison(core::ConsumerId generation, core::PairingRequestId source,
                                          helper::ComparisonResponse response) override {
        flush();
        if (deferred_) return Error{ErrorCode::helper_rejected};
        return target_.respond_comparison(generation, source, response);
    }

    void cancel(core::ConsumerId generation, core::PairingRequestId source) override {
        target_.cancel(generation, source);
    }
    void teardown(core::ConsumerId generation) override {
        const bool retain = active_();
        lost_(generation);
        if (retain) {
            if (!deferred_) {
                deferred_ = generation;
                until_ = std::chrono::steady_clock::now() + std::chrono::seconds{10};
            }
        } else target_.teardown(generation);
    }
    helper::RevocationAttempt forgotten_v1(const helper::RevocationV1& snapshot,
                                           const helper::RevocationProgress& progress) override {
        return target_.forgotten_v1(snapshot, progress);
    }
    void flush(bool force = false) {
        if (deferred_ && (force || !active_() || std::chrono::steady_clock::now() >= until_)) {
            const auto generation = *deferred_;
            deferred_.reset();
            target_.teardown(generation);
        }
    }
private:
    helper::Confirmation& target_;
    std::function<bool()> active_;
    std::function<void(core::ConsumerId)> lost_;
    std::optional<core::ConsumerId> deferred_;
    std::chrono::steady_clock::time_point until_{};
};
}
