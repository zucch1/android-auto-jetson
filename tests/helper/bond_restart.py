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
with tempfile.TemporaryDirectory(prefix="bond-restart-", dir=build) as directory:
    with contextlib.ExitStack() as stack:
        surface = Surface(stack, build, Path(directory), probe=True)
        def rpc(text):
            return tuple(map(int, command(surface.receiver, text).split()))
        def consent(source, wrong=False, expected=0):
            assert command(surface.mock, "comparison-start wrong" if wrong else "comparison-start") == "ok"
            challenge = [0]
            def presented():
                error, challenge[0] = rpc(f"comparison {source}")
                return error == 0 and challenge[0] != 0
            wait(presented)
            assert rpc(f"respond {source} {challenge[0]}")[0] == 0
            assert command(surface.mock, "paired") == "ok"
            managed = (Path(directory) / "managed-bonds").exists()
            error, bonded = rpc(f"bond {source}")
            assert (error, bonded) == (0, 1), "verification of an accepted, bonded peer"
            assert (Path(directory) / "managed-bonds").exists() == managed, "verification recorded a managed bond"
            assert rpc(f"ack {source}")[0] == expected
        peer = "02:00:00:00:00:01 02:00:00:00:00:AA"
        assert command(surface.mock, "paired") == "ok"
        assert rpc("revoke " + peer)[0] == 1001, "unmanaged bond accepted"
        assert surface.state()["removed"] == []
        error, handle = rpc("poll")
        assert error == 0 and rpc(f"bind {handle} 1")[0] == 0
        error, grant = rpc("issue 1")
        assert error == 0 and rpc(f"open {grant}")[0] == 0
        assert not (Path(directory) / "managed-bonds").exists()
        consent(1)
        assert (Path(directory) / "managed-bonds").exists()
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
        # The mock holds a bonded device that is NOT managed (02:..:02) beside the managed
        # one (02:..:01): revoking either lookalike must be refused and remove nothing.
        assert rpc("revoke 02:00:00:00:00:01 02:00:00:00:00:BB")[0] == 1001, "address-only match accepted"
        command(surface.mock, "observe-other")
        assert rpc("revoke 02:00:00:00:00:02 02:00:00:00:00:AA")[0] == 1001, "unmanaged bond accepted"
        assert surface.state()["removed"] == []
        # A persistence failure refuses only new admissions. Making the state
        # directory non-private breaks every write, as a failing disk would.
        directory_path = Path(directory)
        directory_path.chmod(0o500)
        try:
            error, handle = rpc("poll")
            assert error == 0 and rpc(f"bind {handle} 2")[0] == 0
            error, grant = rpc("issue 2")
            assert error == 0 and rpc(f"open {grant}")[0] != 0, "an admission whose pending removal cannot be recorded was opened"
        finally:
            directory_path.chmod(0o700)
        assert rpc("close")[0] == 0
        error, handle = rpc("poll")
        assert error == 0 and rpc(f"bind {handle} 1")[0] == 0
        error, grant = rpc("issue 1")
        assert error == 0 and rpc(f"open {grant}")[0] == 0
        assert "02:00:00:00:00:02" in (directory_path / "pending-removals").read_text(), "pending removal not persisted"
        directory_path.chmod(0o500)
        try:
            error, handle = rpc("poll")
            assert error == 0 and rpc(f"reconnect {handle}")[0] == 1001, "reconnect recorded a bond the helper did not create"
            consent(1, wrong=True, expected=1001)
            assert rpc("close")[0] == 0
            command(surface.mock, "restore")
            command(surface.mock, "paired")
            command(surface.mock, "observe-first")
            error, handle = rpc("poll")
            assert error == 0 and rpc(f"reconnect {handle}")[0] == 0, "trusted reconnect of a managed bond lost"
            assert rpc("revoke " + peer)[0] == 0, "managed revocation lost after a write failure"
            assert len(surface.state()["removed"]) == 2
        finally:
            directory_path.chmod(0o700)
        # The failed disk is back: the stale pending-removal record is cleaned on a helper tick.
        wait(lambda: "02:00:00:00:00:02" not in (directory_path / "pending-removals").read_text())
        assert rpc("revoke 02:00:00:00:00:02 02:00:00:00:00:AA")[0] == 1001
        assert len(surface.state()["removed"]) == 2
        print("PASS restart revokes managed bond, refuses unmanaged bond, and write failures refuse new bond acknowledgements")
