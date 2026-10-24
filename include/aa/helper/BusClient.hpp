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
    ~BusClient() override;
    core::Result<Response> call(const Request&, core::Milliseconds, core::CancellationToken) override;
    core::Result<State> status(Action action);
    core::Result<State> reconnect(const trust::TransportIdentity& identity);
    core::Result<ObservedPeer> poll_bootstrap(Source, HandoffFailure&);
    core::Result<void> authorize_bootstrap(Source, std::uint64_t handle, bool allow, bool listener_ready);
    core::Result<ApSnapshot> get_ap_credentials(Source, std::uint64_t handle);
    core::Result<void> confirmed(core::ConsumerId, core::PairingRequestId) override;
    void cancel(core::ConsumerId, core::PairingRequestId) override;
    void teardown(core::ConsumerId) override;
    RevocationAttempt forgotten_v1(const RevocationV1&, const RevocationProgress&) override;
    core::Result<std::optional<PeerIdentity>> prepare(core::ConsumerId, core::PairingRequestId) override;
private:
    struct Impl;
    explicit BusClient(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};
}
