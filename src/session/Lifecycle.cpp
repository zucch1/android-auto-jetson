// SPDX-License-Identifier: GPL-3.0-or-later
#include <aa/session/Lifecycle.hpp>

namespace aa::session {
namespace {

// Mandated contract budgets. Never caller-overridable; the plan fixes these.
constexpr core::Milliseconds kPairingBudget{60'000};
constexpr core::Milliseconds kWiredReconnectBudget{5'000};
constexpr core::Milliseconds kWirelessReconnectBudget{15'000};
constexpr core::Milliseconds kConsumerGrace{10'000};

[[nodiscard]] core::Milliseconds phase_budget_for(const PhaseBudget& phases,
                                                  State state) noexcept {
    switch (state) {
    case State::discovering: return phases.discovery;
    case State::connecting: return phases.connect;
    case State::negotiating: return phases.negotiate;
    case State::stopping: return phases.stop;
    default: return core::Milliseconds{0};
    }
}

} // namespace

Lifecycle::Lifecycle(LifecycleConfig config, MonotonicClock& clock,
                     const trust::TrustStore& trust, transport::Transport* transport)
    : config_(config), clock_(clock), trust_(trust), transport_(transport) {}

core::Nanoseconds Lifecycle::now() const noexcept { return clock_.now(); }

core::Result<void> Lifecycle::guard() const {
    if (cancel_.get_token().stop_requested()) {
        return Error{ErrorCode::cancelled};
    }
    return {};
}

bool Lifecycle::in_state(std::initializer_list<State> allowed) const noexcept {
    const State s = machine_.state();
    for (State candidate : allowed) {
        if (candidate == s) {
            return true;
        }
    }
    return false;
}

core::Result<void> Lifecycle::transition_if(std::initializer_list<State> allowed,
                                            State to) {
    if (!in_state(allowed)) {
        return Error{ErrorCode::session_illegal_transition};
    }
    auto moved = enter(to);
    if (!moved) {
        return moved.error();
    }
    return {};
}

core::Result<void> Lifecycle::begin_transport() {
    if (transport_ == nullptr || transport_opened_) {
        return {};
    }
    auto opened = transport_->open(cancel_.get_token());
    if (!opened) {
        return opened.error();
    }
    transport_opened_ = true;
    return {};
}

void Lifecycle::arm_deadline(std::optional<core::Nanoseconds>& slot,
                             core::Milliseconds budget, core::Nanoseconds at) {
    if (budget.count > 0) {
        slot = core::Nanoseconds{at.count + core::to_nanoseconds(budget).count};
    } else {
        slot.reset();
    }
}

void Lifecycle::clear_provisional_pairing() noexcept {
    pairing_identity_.reset();
    pairing_authorized_ = false;
    pairing_deadline_.reset();
}

void Lifecycle::run_cleanup() noexcept {
    // Unblock any in-flight transport I/O before closing it, and close only here
    // on the owner thread so pending I/O is never torn down concurrently. A handle
    // is closed even when its open failed, since it may still own resources.
    cancel_.request_stop();
    if (transport_ != nullptr) {
        transport_->close();
        transport_opened_ = false;
        transport_ = nullptr;
    }
    transport_retired_ = false;
}

void Lifecycle::request_stop() noexcept { cancel_.request_stop(); }

void Lifecycle::attach_transport(transport::Transport* transport) noexcept {
    transport_ = transport;
    transport_opened_ = false;
    if (transport != nullptr) {
        transport_retired_ = false;
    }
}

void Lifecycle::retire_transport() noexcept {
    // Link loss: the old handle is terminal. Close it once on the owner thread
    // (pending I/O has returned by the time the drop is reported) and require a
    // fresh handle before the session may connect again.
    if (transport_ != nullptr) {
        if (transport_opened_) {
            transport_->close();
        }
        transport_opened_ = false;
        transport_ = nullptr;
        transport_retired_ = true;
    }
}

core::Result<State> Lifecycle::enter(State to) {
    auto moved = machine_.transition(to);
    if (!moved) {
        return moved;
    }
    const auto at = now();

    // Reconnect budget spans reconnecting -> ... -> active: armed once on loss,
    // cleared only on active, never reset by connecting/negotiating.
    if (to == State::reconnecting) {
        arm_deadline(reconnect_deadline_,
                     config_.link == LinkKind::wired ? kWiredReconnectBudget
                                                     : kWirelessReconnectBudget,
                     at);
    } else if (to == State::active) {
        reconnect_deadline_.reset();
    }

    // Pairing budget is released whenever pairing ends.
    if (to == State::pairing) {
        arm_deadline(pairing_deadline_, kPairingBudget, at);
    } else {
        pairing_deadline_.reset();
    }

    // Consumer grace applies only while degraded.
    if (to == State::degraded) {
        arm_deadline(grace_deadline_, kConsumerGrace, at);
    } else {
        grace_deadline_.reset();
    }

    // Caller-supplied phase budget applies only inside a bounded phase.
    arm_deadline(phase_deadline_, phase_budget_for(config_.phases, to), at);

    if (to == State::stopping) {
        clear_provisional_pairing();
    }
    if (to == State::disconnected || to == State::failed) {
        clear_provisional_pairing();
        phone_identity_.reset();
        reconnect_deadline_.reset();
        run_cleanup();
    }
    if (to == State::disconnected) {
        last_failure_.reset();
        cancel_ = core::CancellationSource{}; // fresh token for the next cycle
    }
    return moved;
}

void Lifecycle::tick() {
    if (cancel_.get_token().stop_requested()) {
        const State s = machine_.state();
        if (s == State::discovering || s == State::pairing || s == State::connecting ||
            s == State::negotiating || s == State::active || s == State::degraded ||
            s == State::reconnecting) {
            (void)enter(State::stopping);
            return;
        }
    }
    (void)observe_deadlines();
}

} // namespace aa::session
