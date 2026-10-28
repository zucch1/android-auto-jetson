// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <aa/helper/SystemHelper.hpp>
#include <memory>

namespace aa::helper {
class BluezBackend final : public Backend {
public:
    using Now = std::function<std::chrono::steady_clock::time_point()>;
    explicit BluezBackend(const std::string& bus_address, Now now = std::chrono::steady_clock::now);
    ~BluezBackend() override;
    bool available() const;
    core::Result<PairingComparison> comparison() override;
    core::Result<void> respond_comparison(ComparisonResponse) override;
    core::Result<State> apply(Action, std::uint32_t) override;
    core::Result<ObservedPeer> observe() override;
    core::Result<State> authorize(std::uint64_t) override;
    core::Result<State> reconnect_peer(std::uint64_t) override;
    core::Result<void> revoke_peer(const PeerIdentity&) override;
    void discard(std::uint64_t) override;
    void cleanup() noexcept override;
    void retire_lease() noexcept override;
    void tick() override;
    core::Result<ObservedPeer> handoff() override;
    SnapshotDeadline handoff_deadline() const override;
    HandoffFailure handoff_failure() const override;
    void fail_handoff(std::uint64_t, HandoffFailure) override;
    core::Result<void> authorize_handoff(std::uint64_t, bool, bool) override;
    void snapshot_provider(const SnapshotProvider&) override;
    void snapshot_fence(const SnapshotFence&) override;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
