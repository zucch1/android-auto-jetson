// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Who may own the bus socket itself (#89 L12). The kernel reports the creator
// of a Unix socket's listening end (SO_PEERCRED, captured at listen() time);
// clients check it before sending a single byte, so a spoofed endpoint never
// sees AUTH, Hello or any RPC. The production system bus is root's: systemd
// creates the socket-activated listener, and dbus-daemon binds it before it
// drops privileges. Session buses and the explicit private host-verification
// buses are started by the caller's own UID (or root). Any other owner is
// refused, whatever it answers on the wire.

#include <cstdint>

#include <unistd.h>

namespace aa::ipc::dbus_adapter {

struct BusOwner final {
    std::uint32_t uid{0};
    bool or_root{false};

    // Production system bus: only a root-owned listener is accepted.
    [[nodiscard]] static BusOwner root() noexcept { return {0, false}; }
    // Session and private buses: the caller's real UID or root.
    [[nodiscard]] static BusOwner root_or_self() noexcept {
        return {static_cast<std::uint32_t>(::getuid()), true};
    }
    [[nodiscard]] bool admits(std::uint32_t peer) const noexcept {
        return peer == uid || (or_root && peer == 0);
    }
};

} // namespace aa::ipc::dbus_adapter
