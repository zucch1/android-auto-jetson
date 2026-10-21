// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <aa/helper/Confirmation.hpp>
#include <aa/helper/HelperClient.hpp>
#include <aa/helper/SystemHelper.hpp>
#include <aa/trust/Store.hpp>
#include <memory>

namespace aa::helper {
class BusClient final : public HelperClient, public Confirmation {
public:
    static core::Result<std::unique_ptr<BusClient>> system(trust::ApprovedPhoneStore& store);
    static core::Result<std::unique_ptr<BusClient>> private_bus(const std::string& address,
                                                               trust::ApprovedPhoneStore& store);
    ~BusClient() override;
    core::Result<Response> call(const Request&, core::Milliseconds, core::CancellationToken) override;
    core::Result<State> status(Action action);
    core::Result<State> reconnect(const trust::TransportIdentity& identity);
    core::Result<void> confirmed(core::ConsumerId, core::PairingRequestId) override;
    void cancel(core::ConsumerId, core::PairingRequestId) override;
    void teardown(core::ConsumerId) override;
    void forgotten() override;
private:
    struct Impl;
    explicit BusClient(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};
}
