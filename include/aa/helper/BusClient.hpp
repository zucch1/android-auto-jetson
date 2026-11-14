// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <aa/helper/Confirmation.hpp>
#include <aa/helper/HelperClient.hpp>
#include <aa/helper/SystemHelper.hpp>
#include <aa/trust/Store.hpp>
#include <memory>

namespace aa::helper {
// Confirmation and HelperClient operations may share an instance across threads;
// complete transactions (including local grants and cancellation) are serialized.
// The store remains session-thread-owned: reconnect must run on its owner thread,
// as must all external store access/mutation. This mutex does not protect the store.
// call/confirmed/status/reconnect budgets include waiting for transaction ownership.
// Void invalidations wait for ownership, then allow 500ms for remote cleanup.
// Destruction requires all callers to have finished.
class BusClient final : public HelperClient, public Confirmation {
public:
    static core::Result<std::unique_ptr<BusClient>> system(trust::ApprovedPhoneStore& store);
    static core::Result<std::unique_ptr<BusClient>> private_bus(const std::string& address,
                                                               trust::ApprovedPhoneStore& store);
    // Host-only private-bus fixtures can explicitly trust this process's UID.
    // system() and private_bus() always require a root-owned helper.
    static core::Result<std::unique_ptr<BusClient>> private_bus_for_test(const std::string& address,
                                                                       trust::ApprovedPhoneStore& store);
    ~BusClient() override;
    core::Result<Response> call(const Request&, core::Milliseconds, core::CancellationToken) override;
    core::Result<State> status(Action action);
    core::Result<State> reconnect(const trust::TransportIdentity& identity);
    core::Result<State> acquire_ap(Source source, std::uint32_t channel = 36);
    core::Result<ObservedPeer> poll_bootstrap(Source, HandoffFailure&);
    core::Result<void> authorize_bootstrap(Source, std::uint64_t handle, bool allow, bool listener_ready);
    core::Result<ApSnapshot> get_ap_credentials(Source, std::uint64_t handle);
    core::Result<void> confirmed(core::ConsumerId, core::PairingRequestId) override;
    bool requires_comparison() const override { return true; }
    core::Result<bool> bond_confirmed(core::ConsumerId, core::PairingRequestId) override;
    core::Result<void> bond_acknowledged(core::ConsumerId, core::PairingRequestId) override;
    core::Result<void> bond_committed(core::ConsumerId, core::PairingRequestId) override;
    bool reconciles() const override { return true; }
    // Must run on the store owner thread (it may be called with the store's own approvals).
    core::Result<std::uint64_t> reconcile_bonds(const std::vector<PeerIdentity>&, bool force) override;
    // The store's approved wireless identities, the set every reconciliation sends.
    static std::vector<PeerIdentity> approved_bonds(const trust::ApprovedPhoneStore&);
    core::Result<PairingComparison> comparison(core::ConsumerId, core::PairingRequestId) override;
    core::Result<void> respond_comparison(core::ConsumerId, core::PairingRequestId, ComparisonResponse) override;
    core::Result<void> cancel(core::ConsumerId, core::PairingRequestId) override;
    void teardown(core::ConsumerId) override;
    RevocationAttempt forgotten_v1(const RevocationV1&, const RevocationProgress&) override;
    core::Result<std::optional<PeerIdentity>> prepare(core::ConsumerId, core::PairingRequestId) override;
private:
    struct Impl;
    explicit BusClient(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};
}
