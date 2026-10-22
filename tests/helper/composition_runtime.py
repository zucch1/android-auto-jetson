# SPDX-License-Identifier: GPL-3.0-or-later
# /// script
# requires-python = ">=3.12"
# dependencies = []
# ///
# Run: /usr/bin/python3 -B tests/helper/composition_runtime.py build/host-dev
from __future__ import annotations

import contextlib
import json
from pathlib import Path
import sys
import tempfile
import time
from typing import TypedDict

from gi.repository import Gio, GLib

from bluez_runtime import Surface, command, control, rejected, wait
from private_bus import line, process


class NmState(TypedDict):
    active: bool
    valid: bool
    starts: int
    deactivations: int
    table: bool
    quarantined: bool


class ComposedSurface(Surface):
    def platform_options(self, stack: contextlib.ExitStack, build: Path, directory: Path) -> list[str]:
        (directory / "state").mkdir(mode=0o700)
        (directory / "ap.conf").write_text("v1 wlan-test US jurisdiction-confirmed 5277\n")
        (directory / "ap.conf").chmod(0o600)
        self.credentials = directory / "state/ap.credentials"
        socket = str(directory / "cmd")
        self.nm = stack.enter_context(process([str(build / "aa_nm_ap_test"), "--wire-double", self.address, socket], self.env))
        assert line(self.nm) == "ready"
        return ["--combined", socket]

    def nm_state(self) -> NmState:
        return json.loads(command(self.nm, "state"))

    def approve_and_acquire(self) -> int:
        request = self.request()
        self.confirm(request)
        command(self.mock, "paired")
        assert command(self.receiver, "acquire") == "0"
        wait(lambda: self.nm_state()["active"])
        assert self.nm_state()["valid"] and self.nm_state()["starts"] == 1
        assert self.credentials.exists()
        return self.phone()

    def forget_error(self, phone: int, expected: str) -> None:
        started = time.monotonic()
        try:
            self.forget(phone)
        except GLib.Error as error:
            assert expected in str(error), str(error)
        else:
            raise AssertionError("unexpected completed external revocation")
        assert time.monotonic() - started < 1.15


def ap_complete_bond_pending(surface: ComposedSurface) -> None:
    phone = surface.approve_and_acquire()
    before = surface.credentials.read_bytes()
    command(surface.mock, "fail-remove")
    surface.forget_error(phone, "system helper unavailable")
    rotated = surface.credentials.read_bytes()
    assert rotated != before
    assert not surface.nm_state()["active"] and not surface.nm_state()["table"]
    for _ in range(3):
        surface.forget_error(phone, "system helper unavailable")
        assert surface.credentials.read_bytes() == rotated
        assert command(surface.receiver, "pending-revocations") == "1"
        assert command(surface.receiver, f"progress {phone}") == "1 0"
        assert surface.state()["removed"] == []
    rejected(surface.consumer, "RequestAddPhone")
    command(surface.mock, "allow-remove")
    surface.forget(phone)
    assert command(surface.receiver, "pending-revocations") == "0"
    assert command(surface.receiver, f"progress {phone}") == "none"
    assert surface.credentials.read_bytes() == rotated
    assert len(surface.state()["removed"]) == 1
    print("PASS composed_ap_ack_bond_retries_preserve_credentials_and_block_reapproval", flush=True)


def bond_complete_ap_pending(surface: ComposedSurface) -> None:
    phone = surface.approve_and_acquire()
    command(surface.nm, "fail-stop")
    surface.forget_error(phone, "system helper unavailable")
    assert len(surface.state()["removed"]) == 1
    assert surface.nm_state()["quarantined"] and surface.nm_state()["table"]
    rotated = surface.credentials.read_bytes()
    for _ in range(2):
        surface.forget_error(phone, "system helper unavailable")
        assert command(surface.receiver, f"progress {phone}") == "0 1"
        assert len(surface.state()["removed"]) == 1
        assert surface.credentials.read_bytes() == rotated
    command(surface.nm, "allow-stop")
    wait(lambda: not surface.nm_state()["table"])
    surface.forget(phone)
    assert command(surface.receiver, "pending-revocations") == "0"
    assert len(surface.state()["removed"]) == 1
    print("PASS composed_bond_ack_ap_retries_do_not_remove_bond_again", flush=True)


def durability_and_precommit(surface: ComposedSurface) -> None:
    phone = surface.approve_and_acquire()
    before = surface.credentials.read_bytes()
    command(surface.receiver, "fault-temp")
    surface.forget_error(phone, "trust store io failure")
    assert surface.phone() == phone and surface.credentials.read_bytes() == before
    assert surface.state()["removed"] == [] and surface.nm_state()["active"]
    command(surface.receiver, "fault-dir")
    command(surface.nm, "fail-stop")
    command(surface.mock, "fail-remove")
    surface.forget_error(phone, "Trust store replaced; crash durability unconfirmed.")
    command(surface.receiver, "fault-clear")
    rotated = surface.credentials.read_bytes()
    assert surface.phone() == 0 and rotated != before
    for _ in range(3):
        surface.forget_error(phone, "Trust store replaced; crash durability unconfirmed.")
        assert command(surface.receiver, f"progress {phone}") == "0 0"
        assert surface.credentials.read_bytes() == rotated
        assert command(surface.receiver, "pending-revocations") == "1"
    command(surface.nm, "allow-stop")
    command(surface.mock, "allow-remove")
    wait(lambda: not surface.nm_state()["table"])
    surface.forget_error(phone, "Trust store replaced; crash durability unconfirmed.")
    assert command(surface.receiver, "pending-revocations") == "0"
    assert len(surface.state()["removed"]) == 1
    print("PASS composed_precommit_no_effects_and_703_wins_every_failed_retry", flush=True)


def wired_ap_only(surface: ComposedSurface) -> None:
    assert command(surface.receiver, "seed-wired") == "0"
    surface.forget(surface.phone())
    assert surface.credentials.exists()
    assert command(surface.receiver, "pending-revocations") == "0"
    assert surface.state()["removed"] == []
    print("PASS composed_wired_deletion_requires_ap_and_no_bond", flush=True)


def cleanup_and_unregister(surface: ComposedSurface) -> None:
    surface.approve_and_acquire()
    assert command(surface.receiver, "unregister-profile") == "0"
    wait(lambda: not surface.state()["profile"])
    command(surface.mock, "release-name")
    command(surface.mock, "acquire-name")
    for _ in range(5):
        assert surface.state()["profile"] == []
        assert surface.nm_state()["active"]
        time.sleep(0.1)
    surface.receiver.kill()
    surface.receiver.wait(timeout=3)
    wait(lambda: not surface.nm_state()["table"] and not surface.state()["properties"]["Pairable"], 5)
    assert surface.state()["removed"] == []
    print("PASS composed_persistent_unregister_and_receiver_loss_cleanup_both", flush=True)


def live_peer_after_wired_revoke(surface: ComposedSurface) -> int:
    assert command(surface.receiver, "seed-wired") == "0"
    wired = surface.phone()
    assert wired != 0
    request = surface.request()
    surface.confirm(request)
    command(surface.mock, "paired")
    assert command(surface.receiver, "acquire") == "0"
    wait(lambda: surface.nm_state()["active"])
    assert command(surface.mock, "NewConnection") == "allowed"
    assert command(surface.mock, "fd-eof") == "open"
    surface.forget(wired)
    assert surface.phone() not in (0, wired)
    assert command(surface.receiver, "pending-revocations") == "0"
    assert not surface.nm_state()["active"] and not surface.nm_state()["table"]
    assert surface.state()["removed"] == []
    assert surface.state()["properties"]["Pairable"] and surface.state()["properties"]["Discoverable"]
    assert command(surface.mock, "fd-eof") == "open"
    assert command(surface.mock, "RequestAuthorization") == "allowed"
    return request


def assert_peer_cleaned(surface: ComposedSurface) -> None:
    state = surface.state()
    observed = (state["properties"]["Pairable"], state["properties"]["Discoverable"],
                command(surface.mock, "fd-eof"), command(surface.mock, "RequestAuthorization"))
    print(f"cleanup observation (Pairable, Discoverable, FD, authorization): {observed}", flush=True)
    assert observed == (False, False, "closed", "rejected"), observed
    assert command(surface.mock, "AuthorizeService") == "rejected"
    assert command(surface.mock, "NewConnection") == "rejected"
    assert state["removed"] == [] and surface.phone() != 0
    assert surface.receiver.poll() is None


def unregister_after_wired_revoke(surface: ComposedSurface) -> None:
    # Given: AP-only revocation left an unrelated approved wireless source live.
    live_peer_after_wired_revoke(surface)
    # When: the consumer unregisters without stopping the receiver.
    control(surface.consumer, "UnregisterConsumer")
    # Then: matching source cleanup closes Bluetooth, without deleting its bond.
    assert_peer_cleaned(surface)
    print("PASS composed_unregister_after_wired_ap_revoke_cleans_live_peer", flush=True)


def cancel_after_wired_revoke(surface: ComposedSurface) -> None:
    # Given
    request = live_peer_after_wired_revoke(surface)
    # When
    surface.cancel(request)
    # Then
    assert_peer_cleaned(surface)
    print("PASS composed_matching_cancel_after_wired_ap_revoke_cleans_live_peer", flush=True)


def consumer_loss_after_wired_revoke(surface: ComposedSurface) -> None:
    # Given
    live_peer_after_wired_revoke(surface)
    # When: only the registered consumer name loses its owner.
    released = surface.consumer.call_sync("org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus",
        "ReleaseName", GLib.Variant("(s)", ("org.custom.BluezConsumer",)), None, Gio.DBusCallFlags.NONE, 1000, None)
    assert released.unpack() == (1,)
    assert command(surface.receiver, "refresh") == "0"
    # Then
    assert_peer_cleaned(surface)
    print("PASS composed_consumer_owner_loss_after_wired_ap_revoke_cleans_live_peer", flush=True)


def main() -> None:
    build = Path(sys.argv[1]).resolve()
    for scenario in (ap_complete_bond_pending, bond_complete_ap_pending, durability_and_precommit, wired_ap_only,
                     cleanup_and_unregister, unregister_after_wired_revoke, cancel_after_wired_revoke,
                     consumer_loss_after_wired_revoke):
        with tempfile.TemporaryDirectory(prefix="cmp-", dir=build) as temporary:
            with contextlib.ExitStack() as stack:
                scenario(ComposedSurface(stack, build, Path(temporary)))


if __name__ == "__main__":
    main()
