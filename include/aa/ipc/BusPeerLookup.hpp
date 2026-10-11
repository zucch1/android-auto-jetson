// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <aa/core/Result.hpp>
#include <aa/ipc/PeerCredentials.hpp>

#include <memory>

namespace aa::ipc {

// Production peer-credential lookup (task 21): real D-Bus-backed answers from
// the session bus — the connection Unix UID (GetConnectionUnixUser) and name
// ownership (GetNameOwner) exactly as the bus daemon reports them. Construct
// once per receiver against the session bus; the bus connection lives behind
// the pimpl (src/ipc/dbus_adapter) so no wire or socket type ever appears in
// a public header.
//
// Error contract: unknown connections, unowned names and every bus failure
// surface as typed errors (never default values) so admission fails closed.
// Tests exercise this implementation on a private bus (dbus-run-session) and
// use a deterministic PeerCredentialLookup double for shapes the private bus
// cannot produce (a foreign Unix UID).
class BusPeerLookup final : public PeerCredentialLookup {
public:
    BusPeerLookup();
    ~BusPeerLookup() override;

    BusPeerLookup(const BusPeerLookup&) = delete;
    BusPeerLookup& operator=(const BusPeerLookup&) = delete;
    BusPeerLookup(BusPeerLookup&&) = delete;
    BusPeerLookup& operator=(BusPeerLookup&&) = delete;

    // False when no session bus is reachable; lookups then fail closed.
    [[nodiscard]] bool connected() const noexcept;

    // The lookup connection's daemon-stamped unique name (diagnostics and
    // forged-reply regressions pin this identity).
    [[nodiscard]] const BusName& unique_name() const;

    [[nodiscard]] core::Result<std::uint32_t> unix_uid(const BusName& name) const override;
    [[nodiscard]] core::Result<BusName> name_owner(const BusName& name) const override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace aa::ipc
