// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <aa/core/Result.hpp>

#include <string>

namespace aa::ipc {

// A D-Bus bus name: either a unique connection name (":1.7") as assigned by
// the bus or a well-known consumer name ("org.custom...."). The wire adapter
// parses names into this value at the boundary; everything interior compares
// whole values and never re-validates.
struct BusName final {
    std::string value{};

    [[nodiscard]] friend bool operator==(const BusName&, const BusName&) noexcept = default;
};

// Authorization seam (task 21): peer-credential lookup as the D-Bus daemon
// reports it — the connection's Unix UID and the owner of a bus name. The
// production implementation (src/ipc/dbus_adapter/BusPeerLookup.*) queries a
// real bus connection; tests inject a deterministic double so wrong-UID and
// name-ownership shapes are reproducible.
//
// Error contract: an unknown connection or unowned name is a typed error
// (ipc_peer_unauthorized), never a default value — admission fails closed.
class PeerCredentialLookup {
public:
    virtual ~PeerCredentialLookup() = default;
    PeerCredentialLookup() = default;
    PeerCredentialLookup(const PeerCredentialLookup&) = delete;
    PeerCredentialLookup& operator=(const PeerCredentialLookup&) = delete;
    PeerCredentialLookup(PeerCredentialLookup&&) = delete;
    PeerCredentialLookup& operator=(PeerCredentialLookup&&) = delete;

    // Unix UID of the connection behind `name` (GetConnectionUnixUser).
    [[nodiscard]] virtual core::Result<std::uint32_t> unix_uid(const BusName& name) const = 0;

    // Current owner of `name` (GetNameOwner). Errors when nobody owns it.
    [[nodiscard]] virtual core::Result<BusName> name_owner(const BusName& name) const = 0;
};

} // namespace aa::ipc
