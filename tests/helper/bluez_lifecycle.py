# SPDX-License-Identifier: GPL-3.0-or-later
# /// script
# requires-python = ">=3.12"
# dependencies = []
# ///
# Run: python3 -B tests/helper/bluez_lifecycle.py build/host-dev unregister
"""Real helper regressions using only temporary private buses and Unix sockets."""
from __future__ import annotations

import contextlib
import hashlib
import os
from pathlib import Path
import select
import socket
import subprocess
import sys
import tempfile
import time
from typing import Literal, assert_never

from bluez_runtime import Surface, command, wait
from private_bus import process


def rpc(surface: Surface, text: str) -> tuple[int, int]:
    error, result = command(surface.receiver, text).split()
    return int(error), int(result)


def stays_unregistered(surface: Surface) -> None:
    # This is the reported idle duration, not a synchronization delay.
    select.select([], [], [], 0.4)
    state = surface.state()
    assert state["profile"] == [], "unregister was undone without explicit register"
    assert state["removed"] == [], "ordinary unregister must preserve bonds"
    assert not state["properties"]["Pairable"] and not state["properties"]["Discoverable"]


def unregister_stays_off(build: Path) -> None:
    with tempfile.TemporaryDirectory(prefix="bluez-unregister-", dir=build) as temporary:
        with contextlib.ExitStack() as stack:
            # Given a registered real daemon and a pending exact-peer grant.
            surface = Surface(stack, build, Path(temporary), probe=True)
            assert rpc(surface, "execute 7") == (0, 1)
            command(surface.mock, "paired")
            error, handle = rpc(surface, "poll")
            assert error == 0 and handle
            assert rpc(surface, f"bind {handle} 1")[0] == 0
            error, grant = rpc(surface, "issue 1")
            assert error == 0 and grant
            # When the authenticated receiver explicitly unregisters.
            error, fresh = rpc(surface, "poll")
            assert error == 0 and fresh
            assert rpc(surface, f"reconnect {fresh}")[0] == 0
            assert rpc(surface, "execute 8") == (0, 0)
            assert command(surface.mock, "AuthorizeService hfp") == "rejected"
            # Then idle, observations, consumed grants and owner changes stay off.
            stays_unregistered(surface)
            assert rpc(surface, "poll")[0] == 1000
            assert rpc(surface, f"open {grant}")[0] == 1001
            assert rpc(surface, f"reconnect {handle}")[0] == 1001
            assert rpc(surface, "close")[0] == 0
            assert command(surface.mock, "RequestAuthorization") == "rejected"
            assert command(surface.mock, "Cancel") == "allowed"
            assert command(surface.mock, "Release") == "allowed"
            stays_unregistered(surface)
            command(surface.mock, "release-name")
            stays_unregistered(surface)
            command(surface.mock, "acquire-name")
            stays_unregistered(surface)
            assert rpc(surface, "poll")[0] == 1000
            stays_unregistered(surface)
            # Explicit register restores registration and ordinary self-healing.
            assert rpc(surface, "execute 7") == (0, 1)
            assert len(surface.state()["profile"]) == 1
            command(surface.mock, "release-name")
            command(surface.mock, "acquire-name")
            wait(lambda: len(surface.state()["profile"]) == 1)
            assert rpc(surface, "poll")[0] == 0
            assert surface.state()["removed"] == []
            print("PASS unregister persists across idle, peer operations and owner changes; explicit register restores", flush=True)


def startup_exits_when_bus_stalls(build: Path, hello: bool) -> None:
    with tempfile.TemporaryDirectory(prefix="bluez-startup-", dir=build) as temporary:
        directory = Path(temporary)
        binary = build / "aa-system-helper"
        (directory / "session.uid").write_text(f"{os.getuid()}\n")
        (directory / "receiver.sha256").write_text(hashlib.sha256(binary.read_bytes()).hexdigest() + "\n")
        for filename in ("session.uid", "receiver.sha256"):
            (directory / filename).chmod(0o600)
        # Given a private endpoint that accepts AUTH but never finishes startup.
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as listener:
            endpoint = "bluez-startup-" + hashlib.sha256(str(directory).encode()).hexdigest()[:24]
            listener.bind("\0" + endpoint)
            listener.listen()
            listener.settimeout(2)
            started = time.monotonic()
            with process([str(binary), "--private-bus", f"unix:abstract={endpoint}", str(directory)], dict(os.environ)) as child:
                connection, _ = listener.accept()
                with connection:
                    connection.settimeout(1)
                    with connection.makefile("rb", buffering=0) as stream:
                        auth = stream.readline()
                        assert auth.startswith(b"\0AUTH"), auth
                        if hello:
                            connection.sendall(b"REJECTED EXTERNAL\r\n")
                            auth = stream.readline()
                            assert auth.startswith(b"AUTH EXTERNAL "), auth
                            connection.sendall(b"OK 0123456789abcdef0123456789abcdef\r\n")
                            negotiation = stream.readline()
                            assert negotiation == b"NEGOTIATE_UNIX_FD\r\n", negotiation
                            connection.sendall(b"AGREE_UNIX_FD\r\n")
                            assert stream.readline() == b"BEGIN\r\n"
                            assert connection.recv(4096), "expected binary Hello request"
                        # When AUTH or Hello stalls, the actual helper must fail closed.
                        try:
                            output, errors = child.communicate(timeout=max(0.01, 2 - (time.monotonic() - started)))
                        except subprocess.TimeoutExpired:
                            raise AssertionError("helper startup exceeded 2 seconds on stalled private bus") from None
                        elapsed = time.monotonic() - started
                        # Then no raw BusService startup follows failed GIO startup.
                        assert child.returncode == 1, (child.returncode, output, errors)
                        assert elapsed < 2, elapsed
                        print(f"PASS stalled {'Hello' if hello else 'AUTH'} exit=1 elapsed={elapsed:.3f}s", flush=True)


def main() -> None:
    build = Path(sys.argv[1]).resolve()
    case: Literal["unregister", "auth", "hello"] = sys.argv[2]
    match case:
        case "unregister":
            unregister_stays_off(build)
        case "auth":
            startup_exits_when_bus_stalls(build, False)
        case "hello":
            startup_exits_when_bus_stalls(build, True)
        case unreachable:
            assert_never(unreachable)


if __name__ == "__main__":
    main()
