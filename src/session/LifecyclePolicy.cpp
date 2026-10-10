// SPDX-License-Identifier: GPL-3.0-or-later
#include <aa/session/Lifecycle.hpp>

namespace aa::session {

core::Result<void> Lifecycle::apply(Event event) {
    const bool teardown = event == Event::cleanup_finished || event == Event::reset;
    if (auto g = begin_step(!teardown); !g) {
        return g.error();
    }
    switch (event) {
    case Event::start_discovery:
        return transition_if({State::disconnected}, State::discovering);
    case Event::transport_ready: {
        if (!in_state({State::connecting})) {
            return Error{ErrorCode::session_illegal_transition};
        }
        if (auto ok = readmit(); !ok) {
            (void)enter_failed(ok.error());
            return ok.error();
        }
        return transition_if({State::connecting}, State::negotiating);
    }
    case Event::negotiation_succeeded: {
        if (!in_state({State::negotiating})) {
            return Error{ErrorCode::session_illegal_transition};
        }
        if (auto ok = readmit(); !ok) {
            (void)enter_failed(ok.error());
            return ok.error();
        }
        return transition_if({State::negotiating}, State::active);
    }
    case Event::degrade:
        return transition_if({State::active}, State::degraded);
    case Event::recover: {
        if (!in_state({State::degraded})) {
            return Error{ErrorCode::session_illegal_transition};
        }
        if (auto ok = readmit(); !ok) {
            (void)enter_failed(ok.error());
            return ok.error();
        }
        return transition_if({State::degraded}, State::active);
    }
    case Event::link_lost: {
        auto moved = transition_if({State::active, State::degraded}, State::reconnecting);
        if (moved) {
            retire_transport();
        }
        return moved;
    }
    case Event::reconnect: {
        if (!in_state({State::reconnecting})) {
            return Error{ErrorCode::session_illegal_transition};
        }
        if (auto ok = readmit(); !ok) {
            (void)enter_failed(ok.error());
            return ok.error();
        }
        if (transport_retired_ && transport_ == nullptr) {
            return Error{ErrorCode::transport_unavailable};
        }
        if (auto moved = transition_if({State::reconnecting}, State::connecting); !moved) {
            return moved.error();
        }
        return complete_connect();
    }
    case Event::cleanup_finished:
        return transition_if({State::stopping}, State::disconnected);
    case Event::reset:
        return transition_if({State::failed}, State::disconnected);
    }
    return Error{ErrorCode::invalid_argument};
}

core::Result<void> Lifecycle::phone_discovered(const trust::PhoneIdentity& identity) {
    if (auto g = begin_step(true); !g) {
        return g.error();
    }
    if (!in_state({State::discovering})) {
        return Error{ErrorCode::session_illegal_transition};
    }
    // Discovery is never approval: only an already-approved phone connects here.
    if (auto ok = admit(identity); !ok) {
        return ok.error();
    }
    phone_identity_ = identity;
    if (auto moved = transition_if({State::discovering}, State::connecting); !moved) {
        return moved.error();
    }
    return complete_connect();
}

core::Result<void> Lifecycle::authorize_pairing(const trust::PhoneIdentity& identity) {
    if (auto g = begin_step(true); !g) {
        return g.error();
    }
    if (!in_state({State::discovering})) {
        return Error{ErrorCode::session_illegal_transition};
    }
    pairing_identity_ = identity;
    pairing_authorized_ = true;
    return transition_if({State::discovering}, State::pairing);
}

core::Result<void> Lifecycle::confirm_pairing(const trust::PhoneIdentity& identity) {
    if (auto g = begin_step(true); !g) {
        return g.error();
    }
    if (!in_state({State::pairing})) {
        return Error{ErrorCode::session_illegal_transition};
    }
    // Confirmation is bound to the exact pending authorized identity; a different
    // approved phone cannot be confirmed into this pairing window.
    if (!pairing_authorized_ || !pairing_identity_.has_value() ||
        pairing_identity_->key != identity.key) {
        return Error{ErrorCode::session_unknown_phone};
    }
    if (auto ok = admit(identity); !ok) {
        clear_provisional_pairing();
        (void)enter_failed(ok.error());
        return ok.error();
    }
    phone_identity_ = identity;
    clear_provisional_pairing();
    if (auto moved = transition_if({State::pairing}, State::connecting); !moved) {
        return moved.error();
    }
    return complete_connect();
}

core::Result<void> Lifecycle::fail(Error reason) {
    // A teardown failure from stopping is legitimate even after cancellation has
    // already routed here; elsewhere observe cancellation before the terminal
    // transition so cancellation wins over a normal progress move.
    if (state() != State::stopping) {
        if (auto g = guard(); !g) {
            return g.error();
        }
    }
    if (!in_state({State::discovering, State::pairing, State::connecting, State::negotiating,
                   State::active, State::degraded, State::reconnecting, State::stopping})) {
        return Error{ErrorCode::session_illegal_transition};
    }
    return enter_failed(reason);
}

core::Result<void> Lifecycle::readmit() {
    if (!phone_identity_.has_value()) {
        return Error{ErrorCode::session_unknown_phone};
    }
    return admit(*phone_identity_);
}

core::Result<void> Lifecycle::admit(const trust::PhoneIdentity& identity) {
    // Fail-closed admission straight from the TrustStore; nothing is fabricated.
    const trust::Decision decision = trust_.lookup(identity);
    switch (decision) {
    case trust::Decision::approved:
        return {};
    case trust::Decision::unknown:
        return Error{ErrorCode::session_unknown_phone};
    case trust::Decision::rejected:
        return Error{ErrorCode::trust_rejected};
    }
    return Error{ErrorCode::session_unknown_phone};
}

} // namespace aa::session
