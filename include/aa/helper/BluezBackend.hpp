// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <aa/helper/SystemHelper.hpp>
#include <memory>

namespace aa::helper {
class BluezBackend final : public Backend {
public:
    explicit BluezBackend(const std::string& bus_address);
    ~BluezBackend() override;
    bool available() const;
    core::Result<State> apply(Action, std::uint32_t) override;
    core::Result<ObservedPeer> observe() override;
    core::Result<State> authorize(std::uint64_t) override;
    core::Result<State> reconnect_peer(std::uint64_t) override;
    core::Result<void> revoke_peer(const PeerIdentity&) override;
    void discard(std::uint64_t) override;
    void cleanup() noexcept override;
    void retire_lease() noexcept override;
    void tick() override;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
