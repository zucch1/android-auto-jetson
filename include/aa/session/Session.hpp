// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <aa/core/Result.hpp>

#include <string_view>

namespace aa::session {

// Session lifecycle state machine (task 17 fills in timeouts and policy).
// Ownership: the machine is owned by the session and mutated only on the
// session thread (aa::core::SessionThread); there is no locking because there
// is no concurrent access.
//
// Error contract: an out-of-table move returns
// ErrorCode::session_illegal_transition and leaves the state unchanged. Phone
// admission is decided by aa::trust::allows_session (fail closed for unknown
// phones) before any transition into pairing/connecting.
enum class State {
    disconnected,
    discovering,
    pairing,
    connecting,
    negotiating,
    active,
    degraded,
    reconnecting,
    stopping,
    failed,
};

[[nodiscard]] constexpr std::string_view to_string(State state) noexcept {
    switch (state) {
    case State::disconnected: return "disconnected";
    case State::discovering: return "discovering";
    case State::pairing: return "pairing";
    case State::connecting: return "connecting";
    case State::negotiating: return "negotiating";
    case State::active: return "active";
    case State::degraded: return "degraded";
    case State::reconnecting: return "reconnecting";
    case State::stopping: return "stopping";
    case State::failed: return "failed";
    }
    return "unknown";
}

// The complete legal transition table. Everything else is rejected.
[[nodiscard]] bool is_legal_transition(State from, State to) noexcept;

class StateMachine final {
public:
    StateMachine() = default;

    [[nodiscard]] State state() const noexcept { return state_; }

    // Applies one transition under the table above.
    core::Result<State> transition(State to);

private:
    State state_{State::disconnected};
};

} // namespace aa::session
