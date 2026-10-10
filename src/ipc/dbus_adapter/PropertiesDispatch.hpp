// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Public org.freedesktop.DBus.Properties surface of the dispatch boundary
// (task 21 owner review round 3): the read-only ContractSemVer metadata as
// Get/GetAll typed replies. Private to src/ipc/dbus_adapter.

#include <aa/ipc/ControlDispatcher.hpp>

#include <optional>

namespace aa::ipc {

[[nodiscard]] std::optional<OutboundReply> dispatch_properties(const IncomingCall& call);

} // namespace aa::ipc
