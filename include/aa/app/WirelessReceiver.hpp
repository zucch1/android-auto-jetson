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
        : helper_(helper), ap_channel_(config.ap_channel), link_(deps, std::move(config)),
          eligibility_(deps.trust, handoff_operations(helper),
              {[this](const helper::ApSnapshot& snapshot) { return link_.listen(snapshot); },
               [this] { link_.close_admission(); }}),
          confirmation_(helper, [this] { return active(); }, [this](core::ConsumerId) {
              eligibility_.tick(active());
              eligibility_.consumer(std::nullopt);
          }) {}
    ~WirelessReceiver() { stop(); }
    void configure(WirelessLink::Config config) {
        stop();
        ap_channel_ = config.ap_channel;
        link_.configure(std::move(config));
    }
    helper::Confirmation& confirmation() { return confirmation_; }
    WiredSession* session() const { return link_.session(); }
    // A phone's bootstrap is in progress (bounded): don't defer or retry the attempt (#161).
    [[nodiscard]] bool bootstrapping() const { return eligibility_.bootstrapping(); }
    // The acquiring source becomes the helper AP lease; drive this AP's handoff
    // with begin(source). reconnect() resets the lease to the empty source and
    // must precede, not follow, acquisition (docs/wireless-handoff.md).
    core::Result<helper::State> acquire_ap(ipc::ControlService& control, helper::Source source) {
        const auto state = control.state();
        if (!state.consumer.value || state.consumer.value != source.generation || !source.confirmation)
            return Error{ErrorCode::ipc_consumer_rejected};
        return helper_.acquire_ap(source, ap_channel_);
    }
    core::Result<void> begin(ipc::ControlService& control, helper::Source source) {
        const auto state = control.state();
        if (!state.consumer.value || state.consumer.value != source.generation)
            return Error{ErrorCode::ipc_consumer_rejected};
        eligibility_.consumer(state.consumer);
        eligibility_.begin(source);
        last_source_ = source;
        return {};
    }
    // Diagnostics for an expired attempt, asked of the helper now (GetBootstrapWaitV1, bounded
    // ~1 s, read only): no_bluetooth_link when the open window's phone has no Bluetooth link.
    // Any error (including a 1.4 helper without the member) is "unknown" and claims nothing.
    [[nodiscard]] helper::BootstrapWait link_wait() {
        if (!last_source_) return helper::BootstrapWait::none;
        const auto wait = helper_.bootstrap_wait(*last_source_);
        return wait ? wait.value() : helper::BootstrapWait::none;
    }
    core::Result<void> reconnect(ipc::ControlService& control, const trust::TransportIdentity& identity) {
        const auto state = control.state();
        if (!state.consumer.value) return Error{ErrorCode::ipc_consumer_rejected};
        stop(HandoffEligibilityCoordinator::Retirement::before_reconnect);
        const auto result = helper_.reconnect(identity);
        if (!result) return result.error();
        return begin(control, {state.consumer.value, 0});
    }
    core::Result<bool> poll(ipc::ControlService& control, std::function<void()> work = {}) {
        auto result = link_.poll(eligibility_, [this, &control, work = std::move(work)] {
            if (work) work();
            const auto state = control.state();
            eligibility_.tick(active());
            eligibility_.consumer(state.consumer.value ? std::optional{state.consumer} : std::nullopt);
        });
        confirmation_.flush();
        return result;
    }
    // The session and listener close first; the coordinator's retirement then settles its denial
    // with the helper (bounded), still before any fresh attempt (#169 review).
    void stop() { stop(HandoffEligibilityCoordinator::Retirement::respect_backoff); }
    void set_io_checkpoint_interval(std::chrono::milliseconds interval) noexcept {
        link_.set_io_checkpoint_interval(interval);
    }
    [[nodiscard]] std::optional<WiredSession::RejectedFrame> last_reject() const {
        return link_.last_reject();
    }
    [[nodiscard]] std::optional<diagnostics::QueueRejection> last_queue_rejection() const {
        return link_.last_queue_rejection();
    }
    [[nodiscard]] bool active() const {
        const auto* session = link_.session();
        return session && (session->state() == session::State::active || session->state() == session::State::degraded);
    }
private:
    void stop(HandoffEligibilityCoordinator::Retirement retirement) {
        link_.stop(); eligibility_.retire(retirement); confirmation_.flush(true); last_source_.reset();
    }
    helper::BusClient& helper_;
    std::uint32_t ap_channel_;
    std::optional<helper::Source> last_source_;
    WirelessLink link_;
    HandoffEligibilityCoordinator eligibility_;
    WirelessConfirmation confirmation_;
};
}
