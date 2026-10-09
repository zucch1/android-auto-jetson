// SPDX-License-Identifier: GPL-3.0-or-later
#include <aa/session/Lifecycle.hpp>

namespace aa::session {
namespace {

[[nodiscard]] bool expired(const std::optional<core::Nanoseconds>& deadline,
                           core::Nanoseconds now) noexcept {
    return deadline.has_value() && now.count >= deadline->count;
}

} // namespace

std::optional<ErrorCode> Lifecycle::observe_deadlines() {
    const auto at = now();
    const State s = machine_.state();
    if (s == State::pairing && expired(pairing_deadline_, at)) {
        clear_provisional_pairing();
        (void)enter_failed(Error{ErrorCode::session_pairing_timeout});
        return ErrorCode::session_pairing_timeout;
    }
    const bool reconnect_cycle =
        s == State::reconnecting || s == State::connecting || s == State::negotiating;
    if (reconnect_cycle && expired(reconnect_deadline_, at)) {
        (void)enter_failed(Error{ErrorCode::timeout});
        return ErrorCode::timeout;
    }
    const bool phase_state =
        s == State::discovering || s == State::connecting || s == State::negotiating ||
        s == State::stopping;
    if (phase_state && expired(phase_deadline_, at)) {
        (void)enter_failed(Error{ErrorCode::timeout});
        return ErrorCode::timeout;
    }
    if (s == State::degraded && expired(grace_deadline_, at)) {
        (void)enter(State::reconnecting);
        return ErrorCode::timeout;
    }
    return std::nullopt;
}

core::Result<void> Lifecycle::begin_step(bool progress) {
    if (std::optional<ErrorCode> fired = observe_deadlines(); fired) {
        return Error{*fired};
    }
    if (progress && cancel_.get_token().stop_requested()) {
        return Error{ErrorCode::cancelled};
    }
    return {};
}

core::Result<void> Lifecycle::post_open_check() {
    if (cancel_.get_token().stop_requested()) {
        (void)enter_failed(Error{ErrorCode::cancelled});
        return Error{ErrorCode::cancelled};
    }
    if (std::optional<ErrorCode> fired = observe_deadlines(); fired) {
        return Error{*fired};
    }
    return {};
}

core::Result<void> Lifecycle::complete_connect() {
    if (auto opened = begin_transport(); !opened) {
        (void)enter_failed(opened.error());
        return opened.error();
    }
    return post_open_check();
}

core::Result<void> Lifecycle::enter_failed(Error reason) {
    auto moved = enter(State::failed);
    if (!moved) {
        return moved.error();
    }
    last_failure_ = reason;
    return {};
}

} // namespace aa::session
