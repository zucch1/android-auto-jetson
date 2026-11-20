// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "../ipc/dbus_adapter/BusOwner.hpp"
#include <gio/gio.h>

namespace aa::helper {
// #89 L12 for the helper's GIO (NetworkManager/BlueZ) system-bus sockets: the
// kernel-reported owner of the listening end (SO_PEERCRED) must be admitted
// before GDBus sends AUTH or Hello. Unknown credentials are refused.
inline bool gio_bus_owner_admitted(GSocket* socket, ipc::dbus_adapter::BusOwner owner) {
    GError* error{};
    GCredentials* credentials = g_socket_get_credentials(socket, &error);
    if (!credentials) { g_clear_error(&error); return false; }
    const uid_t uid = g_credentials_get_unix_user(credentials, &error);
    const bool admitted = !error && owner.admits(static_cast<std::uint32_t>(uid));
    g_clear_error(&error);
    g_object_unref(credentials);
    return admitted;
}
}
