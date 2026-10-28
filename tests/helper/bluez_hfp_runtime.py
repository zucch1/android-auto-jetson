# SPDX-License-Identifier: GPL-3.0-or-later
# /// script
# requires-python = ">=3.12"
# dependencies = []
# ///
# Run: python3 -B tests/helper/bluez_hfp_runtime.py build/host-dev
from __future__ import annotations

import contextlib
from pathlib import Path
import sys
import tempfile
import time
from collections.abc import Callable
from typing import Literal, assert_never

from gi.repository import GLib
from bluez_lifecycle import rpc
from bluez_profile_mock import AA, HFP
from bluez_runtime import Surface, command, rejected, wait

LossEvent = Literal["recreate", "adapter-recreate", "bond-cycle", "connected-false",
    "connected-invalidated", "Cancel", "Release", "release-name", "invalidate",
    "receiver-loss", "helper-stop"]


def admit(surface: Surface) -> None:
    command(surface.mock, "paired")
    error, handle = rpc(surface, "poll")
    assert error == 0 and handle
    assert rpc(surface, f"reconnect {handle}")[0] == 0


def exchange(surface: Surface, at: bytes, expected: bytes) -> None:
    command(surface.mock, "hfp-wire-send " + at.hex())
    reply = bytearray()

    def received() -> bool:
        chunk = command(surface.mock, "hfp-wire-read")
        assert chunk != "closed", bytes(reply)
        if chunk != "open":
            reply.extend(bytes.fromhex(chunk))
        return len(reply) >= len(expected)

    wait(received, 2)
    assert reply == expected


def negotiate(surface: Surface, *, complete: bool = True) -> None:
    exchange(surface, b"AT+BRSF=0\r", b"\r\n+BRSF:0\r\n\r\nOK\r\n")
    exchange(surface, b"AT+CIND=?\r", b'\r\n+CIND:("service",(0,1)),("call",(0,1)),'
        b'("callsetup",(0-3)),("callheld",(0-2)),("signal",(0-5)),("roam",(0,1)),'
        b'("battchg",(0-5))\r\n\r\nOK\r\n')
    exchange(surface, b"AT+CIND?\r", b"\r\n+CIND:0,0,0,0,0,0,0\r\n\r\nOK\r\n")
    if complete:
        exchange(surface, b"AT+CMER=3,0,0,1\r", b"\r\nOK\r\n")


def authorization(surface: Surface) -> None:
    # Given both profiles but no grant, unknown and unbonded peers fail closed.
    assert set(surface.state()["profile"]) == {AA, HFP}
    assert command(surface.mock, "AuthorizeService hfp") == "rejected"
    assert command(surface.mock, "NewConnection hfp") == "rejected"
    error, handle = rpc(surface, "poll")
    assert error == 0 and rpc(surface, f"bind {handle} 1")[0] == 0
    error, grant = rpc(surface, "issue 1")
    assert error == 0 and rpc(surface, f"open {grant}")[0] == 0
    for state in ("unpaired", "temporary-pair"):
        command(surface.mock, state)
        assert command(surface.mock, "AuthorizeService hfp") == "rejected"
        assert command(surface.mock, "NewConnection hfp") == "rejected"
    command(surface.mock, "paired")
    for suffix in ("wrong", "rogue", "audio"):
        assert command(surface.mock, "AuthorizeService hfp " + suffix) == "rejected"
    for suffix in ("wrong", "rogue", "regular", "bad-index", "missing-fd"):
        assert command(surface.mock, "NewConnection hfp " + suffix) == "rejected"
    # When the exact bonded peer connects, a duplicate must not replace its fd.
    assert command(surface.mock, "AuthorizeService hfp") == "allowed"
    assert command(surface.mock, "NewConnection hfp") == "allowed"
    assert command(surface.mock, "NewConnection hfp") == "rejected"
    # Then four real AT commands negotiate on the originally delivered socket.
    negotiate(surface)
    exchange(surface, b"AT+CHUP\r", b"\r\nERROR\r\n")
    assert "Trusted" not in surface.state()["properties"]
    print("PASS HFP exact bonded authorization, fd validation, duplicate rejection and four-command SLC", flush=True)


def independent_disconnect(surface: Surface) -> None:
    # Given coexisting AA bootstrap and fully negotiated HFP sockets.
    admit(surface)
    assert command(surface.mock, "NewConnection") == "allowed"
    assert command(surface.mock, "NewConnection hfp") == "allowed"
    negotiate(surface)
    # When only HFP disconnects, then AA bootstrap is still open.
    assert command(surface.mock, "RequestDisconnection hfp wrong") == "rejected"
    assert command(surface.mock, "RequestDisconnection hfp rogue") == "rejected"
    assert command(surface.mock, "RequestDisconnection hfp") == "allowed"
    assert command(surface.mock, "hfp-wire-read") == "closed"
    assert command(surface.mock, "fd-eof") == "open"
    assert command(surface.mock, "NewConnection hfp") == "allowed"
    negotiate(surface)
    # When only AA disconnects, then HFP still answers on its own socket.
    assert command(surface.mock, "RequestDisconnection") == "allowed"
    assert command(surface.mock, "fd-eof") == "closed"
    exchange(surface, b"AT+CHUP\r", b"\r\nERROR\r\n")
    print("PASS independent AA/HFP RequestDisconnection", flush=True)


def incomplete_timeout(surface: Surface) -> None:
    # Given HFP accepted alongside AA, with CMER deliberately omitted.
    admit(surface)
    assert command(surface.mock, "NewConnection") == "allowed"
    started = time.monotonic()
    assert command(surface.mock, "NewConnection hfp") == "allowed"
    assert rpc(surface, "poll")[0] == 0
    assert time.monotonic() - started < 1
    negotiate(surface, complete=False)
    # When the deadline elapses, then only HFP is closed below the 5s watchdog.
    wait(lambda: command(surface.mock, "hfp-wire-read") == "closed", 4)
    elapsed = time.monotonic() - started
    assert 2.8 <= elapsed < 4.5, elapsed
    assert command(surface.mock, "fd-eof") == "open"
    assert command(surface.mock, "AuthorizeService hfp") == "allowed"
    assert command(surface.mock, "NewConnection hfp") == "allowed"
    negotiate(surface)
    completed = time.monotonic()

    def survives_deadline() -> bool:
        assert command(surface.mock, "hfp-wire-read") == "open"
        return time.monotonic() - completed >= 3.2

    wait(survives_deadline, 4)
    print(f"PASS incomplete SLC timeout isolates HFP elapsed={elapsed:.3f}s", flush=True)


def profile_release(surface: Surface) -> None:
    # Given both sockets, profile Release must be scoped to its actual object path.
    admit(surface)
    assert command(surface.mock, "NewConnection") == "allowed"
    assert command(surface.mock, "NewConnection hfp") == "allowed"
    negotiate(surface)
    # When HFP is released, then AA stays live while HFP re-registers.
    assert command(surface.mock, "Release hfp") == "allowed"
    assert command(surface.mock, "hfp-wire-read") == "closed"
    assert command(surface.mock, "fd-eof") == "open"
    wait(lambda: len(surface.state()["profile"]) == 2)
    assert command(surface.mock, "NewConnection hfp") == "allowed"
    negotiate(surface)
    # When AA is released, then HFP stays live while AA re-registers.
    assert command(surface.mock, "Release aa-profile") == "allowed"
    assert command(surface.mock, "fd-eof") == "closed"
    exchange(surface, b"AT+CHUP\r", b"\r\nERROR\r\n")
    wait(lambda: len(surface.state()["profile"]) == 2)
    # Given HFP alone failing to re-register, then succeeding after a late reply.
    attempts = surface.state()["register_attempts"][HFP]
    assert command(surface.mock, "Release hfp") == "allowed"
    command(surface.mock, "fail-register-hfp")
    wait(lambda: surface.state()["register_attempts"][HFP] > attempts)
    # Then AA remains available, and best-effort cleanup permits later recovery.
    assert command(surface.mock, "NewConnection") == "allowed"
    assert command(surface.mock, "fd-eof") == "open"
    command(surface.mock, "allow-register-hfp")
    command(surface.mock, "late-register-hfp")
    wait(lambda: len(surface.state()["profile"]) == 2)
    assert command(surface.mock, "NewConnection hfp") == "allowed"
    print("PASS independent Profile1 Release, failure isolation and reacquisition", flush=True)


def terminal_loss(surface: Surface, event: LossEvent) -> None:
    # Given independent live sockets for an exact bonded peer.
    admit(surface)
    assert command(surface.mock, "NewConnection") == "allowed"
    assert command(surface.mock, "NewConnection hfp") == "allowed"
    negotiate(surface)
    # When peer continuity, authority, or helper lifetime is lost.
    match event:
        case "invalidate":
            assert rpc(surface, "close")[0] == 0
        case "receiver-loss":
            surface.receiver.kill()
            surface.receiver.wait(timeout=3)
        case "helper-stop":
            surface.daemon.terminate()
            surface.daemon.wait(timeout=3)
        case "recreate" | "adapter-recreate" | "bond-cycle" | "connected-false" | \
             "connected-invalidated" | "Cancel" | "Release" | "release-name":
            assert command(surface.mock, event) in ("ok", "allowed")
        case unreachable:
            assert_never(unreachable)
    # Then neither socket survives; stale grants cannot authorize another HFP fd.
    wait(lambda: command(surface.mock, "hfp-wire-read") == "closed")
    assert command(surface.mock, "fd-eof") == "closed"
    if event == "helper-stop":
        assert surface.state()["profile"] == []
    else:
        if event == "release-name":
            command(surface.mock, "acquire-name")
            wait(lambda: len(surface.state()["profile"]) == 2)
        assert command(surface.mock, "AuthorizeService hfp") == "rejected"
        assert command(surface.mock, "NewConnection hfp") == "rejected"
    print("PASS HFP/AA fail-closed lifecycle " + event, flush=True)


def stale_during_inventory(surface: Surface) -> None:
    # Given a grant whose owner disappears during exact() revalidation.
    admit(surface)
    command(surface.mock, "owner-during-inventory")
    # When HFP requests admission, then the stale owner cannot deliver an fd.
    assert command(surface.mock, "NewConnection hfp") == "rejected"
    print("PASS HFP stale owner during exact revalidation", flush=True)


def revoke(surface: Surface) -> None:
    # Given approved enrollment and two accepted sockets.
    request = surface.request()
    surface.confirm(request)
    command(surface.mock, "paired")
    assert command(surface.mock, "NewConnection") == "allowed"
    assert command(surface.mock, "NewConnection hfp") == "allowed"
    # When ForgetPhone retires approval, then both sockets close despite absent AP.
    rejected(surface.consumer, "ForgetPhone", GLib.Variant("(t)", (surface.phone(),)))
    assert command(surface.mock, "hfp-wire-read") == "closed"
    assert command(surface.mock, "fd-eof") == "closed"
    assert command(surface.mock, "AuthorizeService hfp") == "rejected"
    print("PASS HFP/AA revoke cleanup", flush=True)


def main() -> None:
    build = Path(sys.argv[1]).resolve()
    scenarios: tuple[Callable[[Surface], None], ...] = (
        authorization, independent_disconnect, incomplete_timeout, profile_release, stale_during_inventory)
    with tempfile.TemporaryDirectory(prefix="bluez-hfp-", dir=build) as temporary:
        for scenario in scenarios:
            directory = Path(temporary) / scenario.__name__
            directory.mkdir(mode=0o700)
            with contextlib.ExitStack() as stack:
                scenario(Surface(stack, build, directory, probe=True))
        events: tuple[LossEvent, ...] = ("recreate", "adapter-recreate", "bond-cycle", "connected-false",
                      "connected-invalidated", "Cancel", "Release", "release-name",
                      "invalidate", "receiver-loss", "helper-stop")
        for event in events:
            directory = Path(temporary) / event
            directory.mkdir(mode=0o700)
            with contextlib.ExitStack() as stack:
                terminal_loss(Surface(stack, build, directory, probe=True), event)
        directory = Path(temporary) / "revoke"
        directory.mkdir(mode=0o700)
        with contextlib.ExitStack() as stack:
            revoke(Surface(stack, build, directory))


if __name__ == "__main__":
    main()
