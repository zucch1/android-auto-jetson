# SPDX-License-Identifier: GPL-3.0-or-later
"""Private buses and mock BlueZ only; never uses the installed helper or controller."""
from __future__ import annotations
import contextlib
import sys
import tempfile
import time
from pathlib import Path
from gi.repository import GLib
from bluez_runtime import Surface, command, control, wait
from private_bus import process
from comparison_cases import present


def run(build: Path, directory: Path, failure: str) -> None:
    with contextlib.ExitStack() as stack:
        surface = Surface(stack, build, directory)
        request = surface.request()
        surface.confirm(request)
        command(surface.mock, "comparison-start")
        challenge = present(surface, request)
        assert not (directory / "managed-bonds").exists(), "pending enrolment wrote managed authority"
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
            case "helper-restart":
                # The removal is pending retry (BlueZ refuses it) when the helper itself restarts.
                command(surface.mock, "fail-remove")
                surface.cancel(request)
                assert not surface.state()["removed"]
                assert (directory / "pending-removals").exists() and "02:00:00:00:00:01" in (directory / "pending-removals").read_text(), \
                    "pending removal not persisted"
                surface.daemon.terminate()
                assert surface.daemon.wait(timeout=5) == 0
                command(surface.mock, "allow-remove")
                stack.enter_context(process([str(build / "aa-system-helper"), "--private-bus",
                                             surface.address, str(directory)], surface.env))
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
        assert not (directory / "managed-bonds").exists(), "failed pending enrolment wrote managed authority"
        assert not (directory / "store").exists(), "failed pending enrolment wrote durable trust"
        if failure == "helper-restart":
            wait(lambda: "02:00:00:00:00:01" not in (directory / "pending-removals").read_text())
        if failure != "restart":
            assert surface.phone() == 0
            assert command(surface.receiver, "pending-pairings") == "0"
        print(f"PASS pending_enrolment_{failure}_retires_unaccepted_bond", flush=True)


def just_works_after_failed_pairing(build: Path, directory: Path) -> None:
    """The comparison was accepted, the genuine pairing failed, a spoofing device then pairs without a number."""
    with contextlib.ExitStack() as stack:
        surface = Surface(stack, build, directory)
        request = surface.request()
        surface.confirm(request)
        command(surface.mock, "comparison-start")
        challenge = present(surface, request)
        control(surface.consumer, "RespondPairingComparison",
                GLib.Variant("(ttb)", (request, int(challenge["challenge_id"]), True)))
        wait(lambda: command(surface.mock, "comparison-result") == "allowed")
        # The genuine pairing never completes. Pairing is closed straight after the reply, and a
        # pairing that shows no number is refused whether or not the window is still open.
        assert command(surface.mock, "just-works") == "rejected", "a pairing without a number was accepted"
        assert command(surface.mock, "RequestAuthorization") == "rejected"
        assert command(surface.mock, "RequestConfirmation") == "rejected"
        wait(lambda: not surface.state()["properties"]["Pairable"])
        time.sleep(0.6)
        assert surface.phone() == 0, "a bond without a consumed comparison was recorded"
        assert not (directory / "managed-bonds").exists(), "a bond without a consumed comparison became managed"
        assert not (directory / "store").exists()
        print("PASS just_works_after_failed_pairing_is_refused_and_records_nothing", flush=True)


if __name__ == "__main__":
    build = Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory(prefix="aa-enrolment-", dir=build) as temporary:
        for failure in ("reject", "cancel-retry", "helper-restart", "reload", "disconnect", "restart", "helper-failure"):
            directory = Path(temporary) / failure
            directory.mkdir(mode=0o700)
            run(build, directory, failure)
        directory = Path(temporary) / "just-works"
        directory.mkdir(mode=0o700)
        just_works_after_failed_pairing(build, directory)
