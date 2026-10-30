# SPDX-License-Identifier: GPL-3.0-or-later
# /// script
# requires-python = ">=3.12"
# dependencies = []
# ///
# Run: python3 -B tests/helper/bluez_runtime.py build/host-dev
from __future__ import annotations

import contextlib
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time
from collections.abc import Callable
from typing import Final, TypedDict

from gi.repository import Gio, GLib

from bluez_mock import connect
from private_bus import line, process, send
from comparison_cases import affirmative, negative_comparisons, probe_flow

RECEIVER: Final = "org.custom.AndroidAutoReceiver"
PATH: Final = "/org/custom/AndroidAutoReceiver"
INTERFACE: Final = RECEIVER + "1"


class MockState(TypedDict):
    properties: dict[str, bool | int]
    profile: list[str]
    default_agent: bool
    removed: list[str]
    helper: str
    register_attempts: dict[str, int]


def command(child: subprocess.Popen[bytes], text: str) -> str:
    send(child, text)
    return line(child, 4)


def wait(predicate: Callable[[], bool], seconds: float = 4) -> None:
    deadline = time.monotonic() + seconds
    while not predicate():
        assert time.monotonic() < deadline, "bounded condition did not become true"
        time.sleep(0.02)


def control(bus: Gio.DBusConnection, method: str, params: GLib.Variant | None = None) -> GLib.Variant:
    return bus.call_sync(RECEIVER, PATH, INTERFACE, method, params, None,
                         Gio.DBusCallFlags.NO_AUTO_START, 2500, None)


def rejected(bus: Gio.DBusConnection, method: str, params: GLib.Variant | None = None) -> None:
    try:
        control(bus, method, params)
    except GLib.Error:
        return
    raise AssertionError(f"unexpected control success: {method}")


class Surface:
    def platform_options(self, stack: contextlib.ExitStack, build: Path, directory: Path) -> list[str]:
        return []

    def __init__(self, stack: contextlib.ExitStack, build: Path, directory: Path, probe: bool = False) -> None:
        self.env = dict(os.environ)
        buses = [stack.enter_context(process(["dbus-daemon", "--session", "--nofork", "--print-address=1"], self.env))
                 for _ in range(2)]
        self.address, session = [line(bus) for bus in buses]
        self.env["DBUS_SESSION_BUS_ADDRESS"] = session
        self.consumer = connect(session)
        self.rogue = connect(session)
        self.mock = stack.enter_context(process([sys.executable, "-B", str(Path(__file__).with_name("bluez_mock.py")), self.address], self.env))
        assert line(self.mock) == "ready"
        binary = build / ("aa_bluez_peer_probe" if probe else "aa_system_helper_fixture")
        (directory / "session.uid").write_text(f"{os.getuid()}\n")
        (directory / "receiver.sha256").write_text(hashlib.sha256(binary.read_bytes()).hexdigest() + "\n")
        for filename in ("session.uid", "receiver.sha256"):
            (directory / filename).chmod(0o600)
        options = self.platform_options(stack, build, directory)
        self.daemon = stack.enter_context(process([str(build / "aa-system-helper"), "--private-bus", self.address, str(directory), *options], self.env))
        wait(lambda: len(self.state()["profile"]) == 1)
        args = [str(binary), self.address] if probe else [str(binary), "receiver", self.address, str(directory / "store"), "bluez"]
        self.receiver = stack.enter_context(process(args, self.env))
        assert line(self.receiver) == "ready"
        if not probe:
            self.consumer.call_sync("org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", "RequestName",
                GLib.Variant("(su)", ("org.custom.BluezConsumer", 0)), None, Gio.DBusCallFlags.NONE, 1000, None)
            control(self.consumer, "RegisterConsumer", GLib.Variant("(s)", ("org.custom.BluezConsumer",)))

    def state(self) -> MockState:
        return json.loads(command(self.mock, "state"))

    def request(self) -> int:
        return control(self.consumer, "RequestAddPhone").unpack()[0]

    def confirm(self, request: int) -> None:
        control(self.consumer, "ConfirmPhonePairing", GLib.Variant("(t)", (request,)))

    def cancel(self, request: int) -> None:
        control(self.consumer, "CancelPhonePairing", GLib.Variant("(t)", (request,)))

    def phone(self) -> int:
        return int(command(self.receiver, "phone").split()[1])

    def forget(self, phone: int) -> None:
        control(self.consumer, "ForgetPhone", GLib.Variant("(t)", (phone,)))


def authorization_flow(surface: Surface) -> None:
    state = surface.state()
    assert state["default_agent"] and len(state["profile"]) == 1
    assert not state["properties"]["Pairable"] and not state["properties"]["Discoverable"]
    command(surface.mock, "slow-inventory")
    started = time.monotonic()
    rejected(surface.consumer, "RequestAddPhone")
    assert time.monotonic() - started < 1
    assert surface.phone() == 0
    command(surface.mock, "fast-inventory")
    assert command(surface.mock, "RequestAuthorization") == "rejected"
    rejected(surface.rogue, "RequestAddPhone")
    command(surface.mock, "multiple")
    rejected(surface.consumer, "RequestAddPhone")
    command(surface.mock, "single")
    request = surface.request()
    assert surface.phone() == 0
    rejected(surface.rogue, "ConfirmPhonePairing", GLib.Variant("(t)", (request,)))
    assert command(surface.mock, "RequestAuthorization") == "rejected"
    surface.confirm(request)
    assert surface.state()["properties"]["Pairable"]
    assert command(surface.mock, "RequestAuthorization") == "allowed"
    assert command(surface.mock, "RequestAuthorization wrong") == "rejected"
    assert command(surface.mock, "RequestAuthorization rogue") == "rejected"
    affirmative(surface, request)
    assert command(surface.mock, "AuthorizeService") == "rejected"
    command(surface.mock, "paired")
    assert command(surface.mock, "AuthorizeService audio") == "rejected"
    assert command(surface.mock, "AuthorizeService ag") == "rejected"
    assert command(surface.mock, "AuthorizeService hfp") == "allowed"
    assert command(surface.mock, "AuthorizeService") == "allowed"
    assert command(surface.mock, "NewConnection") == "allowed"
    assert command(surface.mock, "fd-eof") == "open"
    assert command(surface.mock, "RequestDisconnection rogue") == "rejected"
    assert command(surface.mock, "RequestDisconnection") == "allowed"
    assert command(surface.mock, "fd-eof") == "closed"
    surface.cancel(request)
    assert not surface.state()["properties"]["Pairable"]
    assert surface.state()["removed"] == []
    assert surface.state()["cancel_unpaired"] == 0
    assert command(surface.receiver, "reconnect") == "0"
    assert command(surface.mock, "RequestAuthorization") == "rejected"
    assert command(surface.mock, "AuthorizeService") == "allowed"
    assert command(surface.mock, "RequestConfirmation") == "rejected"
    command(surface.mock, "temporary-pair")
    assert command(surface.receiver, "reconnect") == "1001"
    assert command(surface.mock, "AuthorizeService") == "rejected"
    command(surface.mock, "unpaired")
    assert command(surface.receiver, "reconnect") == "1001"
    assert command(surface.mock, "AuthorizeService") == "rejected"
    command(surface.mock, "paired")
    assert command(surface.mock, "Cancel") == "allowed"
    print("PASS exact_peer_control_authorization_fd_teardown_and_bond_checked_reconnect", flush=True)


def revocation_flow(surface: Surface) -> None:
    phone = surface.phone()
    assert command(surface.receiver, "fault-temp") == "0"
    rejected(surface.consumer, "ForgetPhone", GLib.Variant("(t)", (phone,)))
    assert surface.phone() == phone and surface.state()["removed"] == []
    assert command(surface.receiver, "fault-dir") == "0"
    rejected(surface.consumer, "ForgetPhone", GLib.Variant("(t)", (phone,)))
    assert surface.phone() == 0 and len(surface.state()["removed"]) == 1
    assert command(surface.receiver, "fault-clear") == "0"
    assert command(surface.receiver, "pending-revocations") == "1"
    command(surface.mock, "restore")
    rejected(surface.consumer, "RequestAddPhone")
    for _ in range(3):
        rejected(surface.consumer, "ForgetPhone", GLib.Variant("(t)", (phone,)))
        assert len(surface.state()["removed"]) == 1
    assert command(surface.receiver, "pending-revocations") == "1"
    print("PASS revoke703_precommit_and_missing_ap_remains_pending_without_reapproval", flush=True)


def bond_retry_flow(surface: Surface) -> None:
    request = surface.request()
    surface.confirm(request)
    command(surface.mock, "paired")
    command(surface.mock, "fail-remove")
    phone = surface.phone()
    rejected(surface.consumer, "ForgetPhone", GLib.Variant("(t)", (phone,)))
    assert surface.phone() == 0
    assert command(surface.receiver, "pending-revocations") == "1"
    command(surface.mock, "allow-remove")
    rejected(surface.consumer, "ForgetPhone", GLib.Variant("(t)", (phone,)))
    assert command(surface.receiver, "pending-revocations") == "1"
    assert len(surface.state()["removed"]) == 1
    print("PASS bond_retry_progress_with_explicitly_unavailable_ap", flush=True)


def loss_flow(surface: Surface) -> None:
    command(surface.mock, "restore")
    request = surface.request()
    surface.confirm(request)
    command(surface.mock, "paired")
    assert command(surface.mock, "NewConnection") == "allowed"
    surface.receiver.kill()
    surface.receiver.wait(timeout=3)
    wait(lambda: not surface.state()["properties"]["Pairable"], 5)
    assert command(surface.mock, "fd-eof") == "closed"
    assert command(surface.mock, "RequestAuthorization") == "rejected"
    assert len(surface.state()["removed"]) == 0
    print("PASS quiet_receiver_loss_preserves_approved_bond", flush=True)


def rollback_flow(surface: Surface) -> None:
    assert surface.phone() == 0
    request = surface.request()
    command(surface.mock, "fail-set")
    rejected(surface.consumer, "ConfirmPhonePairing", GLib.Variant("(t)", (request,)))
    assert surface.phone() != 0
    assert command(surface.mock, "RequestAuthorization") == "rejected"
    command(surface.mock, "allow-set")
    wait(lambda: not surface.state()["properties"]["Pairable"])
    rejected(surface.consumer, "ForgetPhone", GLib.Variant("(t)", (surface.phone(),)))
    assert surface.phone() == 0
    assert command(surface.receiver, "pending-revocations") == "1"
    assert len(surface.state()["removed"]) == 1
    print("PASS helper_failure_keeps_committed_approval_and_closes_window", flush=True)


def main() -> None:
    build = Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory(prefix="bluez36-", dir=build) as directory:
        with contextlib.ExitStack() as stack:
            surface = Surface(stack, build, Path(directory))
            authorization_flow(surface)
            revocation_flow(surface)
        for scenario in (bond_retry_flow, loss_flow, negative_comparisons):
            with contextlib.ExitStack() as stack:
                scenario_directory = Path(directory) / scenario.__name__
                scenario_directory.mkdir(mode=0o700)
                scenario(Surface(stack, build, scenario_directory))
        with contextlib.ExitStack() as stack:
            probe_flow(Surface(stack, build, Path(directory), probe=True))
        with contextlib.ExitStack() as stack:
            rollback_directory = Path(directory) / "rollback"
            rollback_directory.mkdir(mode=0o700)
            rollback_flow(Surface(stack, build, rollback_directory))


if __name__ == "__main__":
    main()
