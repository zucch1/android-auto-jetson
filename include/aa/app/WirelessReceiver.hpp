// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <aa/app/WirelessConfirmation.hpp>
#include <aa/app/WirelessLink.hpp>
#include <aa/ipc/ControlService.hpp>

namespace aa::app {
// Composition owner for the wireless path only. ControlService must use
// confirmation(); all control dispatch, poll and store access share its owner.
class WirelessReceiver final {
public:
    WirelessReceiver(WiredSessionDeps deps, helper::BusClient& helper, WirelessLink::Config config)
        : helper_(helper), link_(deps, std::move(config)),
          eligibility_(deps.trust, handoff_operations(helper),
              {[this](const helper::ApSnapshot& snapshot) { return link_.listen(snapshot); },
               [this] { link_.close_admission(); }}),
          confirmation_(helper, [this] { return active(); }, [this](core::ConsumerId) {
              eligibility_.tick(active());
              eligibility_.consumer(std::nullopt);
          }) {}
    ~WirelessReceiver() { stop(); }
    helper::Confirmation& confirmation() { return confirmation_; }
    core::Result<void> begin(ipc::ControlService& control, helper::Source source) {
        const auto state = control.state();
        if (!state.consumer.value || state.consumer.value != source.generation)
            return Error{ErrorCode::ipc_consumer_rejected};
        eligibility_.consumer(state.consumer);
        eligibility_.begin(source);
        return {};
    }
    core::Result<void> reconnect(ipc::ControlService& control, const trust::TransportIdentity& identity) {
        const auto state = control.state();
        if (!state.consumer.value) return Error{ErrorCode::ipc_consumer_rejected};
        const auto result = helper_.reconnect(identity);
        if (!result) return result.error();
        return begin(control, {state.consumer.value, 0});
    }
    core::Result<bool> poll(ipc::ControlService& control, std::function<void()> work = {}) {
        auto result = link_.poll(eligibility_, [&] {
            if (work) work();
            const auto state = control.state();
            eligibility_.tick(active());
            eligibility_.consumer(state.consumer.value ? std::optional{state.consumer} : std::nullopt);
        });
        confirmation_.flush();
        return result;
    }
    void stop() {
        eligibility_.retire(); link_.stop(); confirmation_.flush(true);
    }
    [[nodiscard]] bool active() const {
        const auto* session = link_.session();
        return session && (session->state() == session::State::active || session->state() == session::State::degraded);
    }
private:
    helper::BusClient& helper_;
    WirelessLink link_;
    HandoffEligibilityCoordinator eligibility_;
    WirelessConfirmation confirmation_;
};
}
