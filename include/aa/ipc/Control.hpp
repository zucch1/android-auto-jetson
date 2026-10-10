// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <aa/core/Ids.hpp>
#include <aa/core/Result.hpp>

#include <cstdint>
#include <string_view>

namespace aa::ipc {

// Session D-Bus control surface (org.custom.AndroidAutoReceiver1, full XML in
// task 21). Low-rate control and status only: audio/video payloads never cross
// D-Bus and the surface grants no UI authority.
//
// Ownership/threading: the D-Bus adapter owns the bus connection on its own
// thread and parses wire arguments into these types at the boundary ("parse,
// don't validate"); every request is then posted to the session thread.
//
// Exactly one active session consumer may hold projection control. The adapter
// rejects callers whose Unix UID differs from the active session user, callers
// that do not own the registered consumer name, and anyone other than the
// active consumer for pairing requests. Unknown callers fail closed.
enum class ControlMethod {
    register_consumer,
    unregister_consumer,
    start_projection,
    stop_projection,
    request_add_phone,
    confirm_phone_pairing,
    cancel_phone_pairing,
    forget_phone,
    get_state,
    get_capabilities,
    set_display_viewport,
    ping,
};

// Stable wire names (the D-Bus introspection contract).
[[nodiscard]] constexpr std::string_view wire_name(ControlMethod method) noexcept {
    switch (method) {
    case ControlMethod::register_consumer: return "RegisterConsumer";
    case ControlMethod::unregister_consumer: return "UnregisterConsumer";
    case ControlMethod::start_projection: return "StartProjection";
    case ControlMethod::stop_projection: return "StopProjection";
    case ControlMethod::request_add_phone: return "RequestAddPhone";
    case ControlMethod::confirm_phone_pairing: return "ConfirmPhonePairing";
    case ControlMethod::cancel_phone_pairing: return "CancelPhonePairing";
    case ControlMethod::forget_phone: return "ForgetPhone";
    case ControlMethod::get_state: return "GetState";
    case ControlMethod::get_capabilities: return "GetCapabilities";
    case ControlMethod::set_display_viewport: return "SetDisplayViewport";
    case ControlMethod::ping: return "Ping";
    }
    return {};
}

struct ConsumerIdentity final {
    core::ConsumerId consumer{};
    std::uint32_t unix_uid{};

    [[nodiscard]] friend constexpr bool operator==(const ConsumerIdentity&,
                                                   const ConsumerIdentity&) noexcept = default;
};

struct Viewport final {
    std::uint16_t width{};
    std::uint16_t height{};
    std::uint32_t dpi{};
};

struct AccessCheck final {
    ConsumerIdentity caller{};
    ConsumerIdentity active{};
    bool owns_registered_name{false};
};

enum class Authorization { allowed, wrong_uid, unowned_name, not_active_consumer };

// Single-consumer admission rule. Deny by default.
[[nodiscard]] constexpr Authorization authorize(const AccessCheck& check) noexcept {
    if (check.caller.unix_uid != check.active.unix_uid) {
        return Authorization::wrong_uid;
    }
    if (!check.owns_registered_name) {
        return Authorization::unowned_name;
    }
    if (!(check.caller.consumer == check.active.consumer)) {
        return Authorization::not_active_consumer;
    }
    return Authorization::allowed;
}

} // namespace aa::ipc
