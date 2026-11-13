# SPDX-License-Identifier: GPL-3.0-or-later
"""Private buses and mock BlueZ only; never uses the installed helper or controller."""
from __future__ import annotations
import contextlib
import sys
import tempfile
from pathlib import Path
from gi.repository import GLib
from bluez_runtime import Surface, command, control, wait
from comparison_cases import present


def run(build: Path, directory: Path, failure: str) -> None:
    with contextlib.ExitStack() as stack:
        surface = Surface(stack, build, directory)
        request = surface.request()
        surface.confirm(request)
        command(surface.mock, "comparison-start")
        challenge = present(surface, request)
        assert surface.phone() == 0, "unaccepted comparison must never persist trust"
        # Simulate a bond appearing before consent. Retirement must remove it.
        command(surface.mock, "paired")
        match failure:
            case "reject":
                control(surface.consumer, "RespondPairingComparison",
                        GLib.Variant("(ttb)", (request, int(challenge["challenge_id"]), False)))
            case "cancel-retry":
                command(surface.mock, "fail-remove")
                surface.cancel(request)
                assert not surface.state()["removed"]
                command(surface.mock, "allow-remove")
            case "reload":
                assert command(surface.receiver, "reload-store") == "0"
            case "disconnect":
                surface.consumer.call_sync("org.freedesktop.DBus", "/org/freedesktop/DBus",
                    "org.freedesktop.DBus", "ReleaseName", GLib.Variant("(s)",
                    ("org.custom.BluezConsumer",)), None, 0, 1000, None)
            case "restart":
                surface.receiver.kill()
                surface.receiver.wait(timeout=3)
            case "helper-failure":
                command(surface.mock, "fail-inventory")
                # Receiver sees a failed exact-snapshot acknowledgement and cancels.
                wait(lambda: command(surface.receiver, "pending-pairings") == "0")
                command(surface.mock, "allow-inventory")
        wait(lambda: len(surface.state()["removed"]) == 1, 5)
        assert not surface.state()["properties"]["Pairable"]
        assert not (directory / "store").exists(), "failed pending enrolment wrote durable trust"
        if failure != "restart":
            assert surface.phone() == 0
            assert command(surface.receiver, "pending-pairings") == "0"
        print(f"PASS pending_enrolment_{failure}_retires_unaccepted_bond", flush=True)


if __name__ == "__main__":
    build = Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory(prefix="aa-enrolment-", dir=build) as temporary:
        for failure in ("reject", "cancel-retry", "reload", "disconnect", "restart", "helper-failure"):
            directory = Path(temporary) / failure
            directory.mkdir(mode=0o700)
            run(build, directory, failure)
