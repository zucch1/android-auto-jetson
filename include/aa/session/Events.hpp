// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <string_view>

namespace aa::session {

// Link medium selects the contract reconnect budget (wired 5 s / wireless 15 s).
enum class LinkKind { wired, wireless };

// Typed, payload-free lifecycle occurrences. Payload-carrying inputs (a phone
// identity, a typed failure reason) and cross-thread cancellation use dedicated
// Lifecycle methods; this vocabulary covers the rest and is what apply(Event)
// dispatches. Keeping it typed lets diagnostics and tests enumerate events
// without stringly-typed plumbing.
enum class Event {
    start_discovery,
    transport_ready,
    negotiation_succeeded,
    degrade,
    recover,
    link_lost,
    reconnect,
    cleanup_finished,
    reset,
};

[[nodiscard]] constexpr std::string_view to_string(Event event) noexcept {
    switch (event) {
    case Event::start_discovery: return "start_discovery";
    case Event::transport_ready: return "transport_ready";
    case Event::negotiation_succeeded: return "negotiation_succeeded";
    case Event::degrade: return "degrade";
    case Event::recover: return "recover";
    case Event::link_lost: return "link_lost";
    case Event::reconnect: return "reconnect";
    case Event::cleanup_finished: return "cleanup_finished";
    case Event::reset: return "reset";
    }
    return "unknown";
}

[[nodiscard]] constexpr std::string_view to_string(LinkKind kind) noexcept {
    switch (kind) {
    case LinkKind::wired: return "wired";
    case LinkKind::wireless: return "wireless";
    }
    return "unknown";
}

} // namespace aa::session
