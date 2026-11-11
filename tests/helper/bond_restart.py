# SPDX-License-Identifier: GPL-3.0-or-later
"""Real private-bus helper restart, with mock BlueZ retaining its bond."""
import contextlib
from pathlib import Path
import sys
import tempfile
from gi.repository import GLib, Gio
from bluez_runtime import Surface, command, wait
from private_bus import process

build = Path(sys.argv[1]).resolve()
with tempfile.TemporaryDirectory(prefix="bond-restart-") as directory:
    with contextlib.ExitStack() as stack:
        surface = Surface(stack, build, Path(directory), probe=True)
        def rpc(text):
            return tuple(map(int, command(surface.receiver, text).split()))
        peer = "02:00:00:00:00:01 02:00:00:00:00:AA"
        assert command(surface.mock, "paired") == "ok"
        assert rpc("revoke " + peer)[0] == 1001, "unmanaged bond accepted"
        assert surface.state()["removed"] == []
        error, handle = rpc("poll")
        assert error == 0 and rpc(f"bind {handle} 1")[0] == 0
        error, grant = rpc("issue 1")
        assert error == 0 and rpc(f"open {grant}")[0] == 0
        assert command(surface.mock, "paired") == "ok"
        surface.daemon.terminate()
        assert surface.daemon.wait(timeout=5) == 0
        stack.enter_context(process([str(build / "aa-system-helper"), "--private-bus",
            surface.address, directory], surface.env))
        def owned():
            try:
                surface.consumer.call_sync("org.freedesktop.DBus", "/org/freedesktop/DBus",
                    "org.freedesktop.DBus", "GetNameOwner",
                    GLib.Variant("(s)", ("org.custom.AndroidAutoSystemHelper",)), None,
                    Gio.DBusCallFlags.NONE, 100, None)
                return True
            except GLib.Error:
                return False
        # consumer is on the receiver bus; use a connection on the helper bus.
        from bluez_mock import connect
        surface.consumer = connect(surface.address)
        wait(owned)
        assert rpc("revoke " + peer)[0] == 0, "managed revocation lost across restart"
        assert len(surface.state()["removed"]) == 1
        assert rpc("revoke 02:00:00:00:00:02 02:00:00:00:00:AA")[0] == 1001
        assert len(surface.state()["removed"]) == 1
        print("PASS restart revokes managed bond and refuses unmanaged bond")
