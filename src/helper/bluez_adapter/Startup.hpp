// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <gio/gio.h>
#include <string>

namespace aa::helper::bluez_adapter {
// The caller must push context as thread-default. Returns an owned bus reference.
GDBusConnection* connect_bus(const std::string& address, GMainContext* context);
}
