// SPDX-License-Identifier: GPL-3.0-or-later
#include <aa/session/Session.hpp>

#include <array>

namespace aa::session {
namespace {

struct Edge final {
    State from;
    State to;
};

// Table-driven legality: adding a state or edge means editing this table and
// the tests that walk it. Self-transitions are not edges.
constexpr std::array kEdges{
    Edge{State::disconnected, State::discovering},
    Edge{State::discovering, State::pairing},
    Edge{State::discovering, State::connecting},
    Edge{State::discovering, State::stopping},
    Edge{State::discovering, State::failed},
    Edge{State::pairing, State::connecting},
    Edge{State::pairing, State::stopping},
    Edge{State::pairing, State::failed},
    Edge{State::connecting, State::negotiating},
    Edge{State::connecting, State::stopping},
    Edge{State::connecting, State::failed},
    Edge{State::negotiating, State::active},
    Edge{State::negotiating, State::stopping},
    Edge{State::negotiating, State::failed},
    Edge{State::active, State::degraded},
    Edge{State::active, State::reconnecting},
    Edge{State::active, State::stopping},
    Edge{State::active, State::failed},
    Edge{State::degraded, State::active},
    Edge{State::degraded, State::reconnecting},
    Edge{State::degraded, State::stopping},
    Edge{State::degraded, State::failed},
    Edge{State::reconnecting, State::connecting},
    Edge{State::reconnecting, State::stopping},
    Edge{State::reconnecting, State::failed},
    Edge{State::stopping, State::disconnected},
    Edge{State::stopping, State::failed},
    Edge{State::failed, State::disconnected},
};

} // namespace

bool is_legal_transition(State from, State to) noexcept {
    for (const Edge& edge : kEdges) {
        if (edge.from == from && edge.to == to) {
            return true;
        }
    }
    return false;
}

core::Result<State> StateMachine::transition(State to) {
    if (!is_legal_transition(state_, to)) {
        return Error{ErrorCode::session_illegal_transition};
    }
    state_ = to;
    return state_;
}

} // namespace aa::session
