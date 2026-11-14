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
from private_bus import line, process
from comparison_cases import present
from composition_runtime import ComposedSurface


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


def store_write_failure_after_acknowledgement(build: Path, directory: Path, removal_fails: bool) -> None:
    """The bond is acknowledged (helper-managed), then the trust-store write fails."""
    with contextlib.ExitStack() as stack:
        surface = Surface(stack, build, directory)
        request = surface.request()
        surface.confirm(request)
        command(surface.mock, "comparison-start")
        challenge = present(surface, request)
        control(surface.consumer, "RespondPairingComparison",
                GLib.Variant("(ttb)", (request, int(challenge["challenge_id"]), True)))
        wait(lambda: command(surface.mock, "comparison-result") == "allowed")
        assert command(surface.receiver, "fault-temp") == "0"
        if removal_fails:
            command(surface.mock, "fail-remove")
        command(surface.mock, "paired")
        # The failed write must disconnect the device, even when the removal itself keeps failing.
        wait(lambda: bool(surface.state()["disconnected"]), 8)
        assert surface.state()["disconnected"][0].endswith("dev_02_00_00_00_00_01")
        assert command(surface.mock, "AuthorizeService hfp") == "rejected", "HFP stayed authorized after the failed write"
        if removal_fails:
            # The failed removal is tracked in the receiver's revocation record, not forgotten.
            wait(lambda: command(surface.receiver, "pending-revocations") == "1")
            assert not surface.state()["removed"]
            command(surface.mock, "allow-remove")
        wait(lambda: len(surface.state()["removed"]) == 1, 8)
        wait(lambda: command(surface.receiver, "pending-revocations") == "0", 8)
        assert surface.phone() == 0 and not (directory / "store").exists(), "a failed write stored trust"
        assert not (directory / "managed-bonds").exists(), "a bond without a durable trust write was admitted"
        assert command(surface.receiver, "fault-clear") == "0"
        print(f"PASS store_write_failure_after_acknowledgement_retires_bond_{'with_retry' if removal_fails else 'at_once'}", flush=True)


def accept(surface: Surface, request: int) -> None:
    command(surface.mock, "comparison-start")
    challenge = present(surface, request)
    control(surface.consumer, "RespondPairingComparison",
            GLib.Variant("(ttb)", (request, int(challenge["challenge_id"]), True)))
    wait(lambda: command(surface.mock, "comparison-result") == "allowed")


def peer_in(directory: Path, name: str) -> bool:
    path = directory / name
    return path.exists() and "02:00:00:00:00:01" in path.read_text()


def receiver_exit_after_acknowledgement(build: Path, directory: Path, helper_restart: bool) -> None:
    """Finding A: the receiver process exits after the helper acknowledged the bond, before its store commit."""
    with contextlib.ExitStack() as stack:
        surface = Surface(stack, build, directory)
        request = surface.request()
        surface.confirm(request)
        assert command(surface.receiver, "exit-after-ack") == "0"
        if helper_restart:
            command(surface.mock, "fail-remove")  # the helper's cleanup cannot finish before it restarts
        accept(surface, request)
        command(surface.mock, "paired")
        assert surface.receiver.wait(timeout=5) == 9, "the receiver did not exit at the acknowledgement"
        assert not peer_in(directory, "managed-bonds"), "a provisional bond was admitted"
        assert command(surface.mock, "AuthorizeService hfp") == "rejected"
        if helper_restart:
            time.sleep(0.5)
            assert not surface.state()["removed"]
            assert peer_in(directory, "pending-removals"), "the cleanup obligation was not durable"
            surface.daemon.terminate()
            assert surface.daemon.wait(timeout=5) == 0
            command(surface.mock, "allow-remove")
            stack.enter_context(process([str(build / "aa-system-helper"), "--private-bus",
                                         surface.address, str(directory)], surface.env))
        wait(lambda: len(surface.state()["removed"]) == 1, 8)
        wait(lambda: not peer_in(directory, "pending-removals"))
        assert not peer_in(directory, "managed-bonds")
        # The restarted receiver finds nothing approved.
        receiver = stack.enter_context(process([str(build / "aa_system_helper_fixture"), "receiver",
                                                surface.address, str(directory / "store"), "bluez"], surface.env))
        assert line(receiver) == "ready"
        assert command(receiver, "phone") == "phone 0"
        print(f"PASS receiver_exit_after_acknowledgement_removes_bond{'_across_helper_restart' if helper_restart else ''}", flush=True)


def commit_failure_retracts_approval(build: Path, directory: Path) -> None:
    """The durable trust write succeeded but the helper commit fails: the approval is retracted."""
    with contextlib.ExitStack() as stack:
        # Composed with the AP double, so the retraction's AP leg completes too.
        surface = ComposedSurface(stack, build, directory)
        request = surface.request()
        Surface.confirm(surface, request)
        assert command(surface.receiver, "hold-commit") == "0"
        accept(surface, request)
        command(surface.mock, "paired")
        wait(lambda: len(surface.state()["removed"]) == 1, 8)
        assert command(surface.mock, "AuthorizeService hfp") == "rejected"
        assert command(surface.mock, "AuthorizeService") == "rejected"
        wait(lambda: command(surface.receiver, "pending-revocations") == "0", 8)
        assert surface.phone() == 0, "an approval whose commit failed stayed"
        assert not peer_in(directory, "managed-bonds") and not peer_in(directory, "pending-removals")
        assert command(surface.receiver, "reconnect") != "0"
        print("PASS commit_failure_retracts_approval_and_removes_bond", flush=True)


def forget_then_reenrol(build: Path, directory: Path, failure: str) -> None:
    """Finding B: acknowledge, commit, revoke, re-enrol the same identity, then reject or cancel."""
    with contextlib.ExitStack() as stack:
        # Composed with the AP double, so ForgetPhone's AP leg completes and re-enrolment is allowed.
        surface = ComposedSurface(stack, build, directory)
        request = surface.request()
        surface.confirm(request)  # accepts, bonds, acknowledges, stores and commits
        wait(lambda: surface.phone() != 0)
        assert peer_in(directory, "managed-bonds") and not peer_in(directory, "pending-removals")
        phone = surface.phone()
        surface.forget(phone)
        assert len(surface.state()["removed"]) == 1
        assert not peer_in(directory, "managed-bonds") and peer_in(directory, "revoked-bonds")
        wait(lambda: command(surface.receiver, "pending-revocations") == "0")
        command(surface.mock, "restore")
        command(surface.mock, "unpaired")
        request = surface.request()
        Surface.confirm(surface, request)  # opens the window only; no acceptance
        assert peer_in(directory, "pending-removals"), "the new enrolment has no cleanup obligation"
        command(surface.mock, "comparison-start")
        challenge = present(surface, request)
        command(surface.mock, "paired")  # a new bond for the same adapter/address, before consent
        if failure == "reject":
            control(surface.consumer, "RespondPairingComparison",
                    GLib.Variant("(ttb)", (request, int(challenge["challenge_id"]), False)))
        else:
            surface.cancel(request)
        wait(lambda: len(surface.state()["removed"]) == 2, 5)
        wait(lambda: not peer_in(directory, "pending-removals"))
        assert not peer_in(directory, "managed-bonds") and peer_in(directory, "revoked-bonds")
        assert surface.phone() == 0
        assert command(surface.mock, "AuthorizeService hfp") == "rejected"
        print(f"PASS forget_then_reenrol_{failure}_removes_the_new_bond", flush=True)


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
        for helper_restart in (False, True):
            directory = Path(temporary) / f"exit-after-ack-{helper_restart}"
            directory.mkdir(mode=0o700)
            receiver_exit_after_acknowledgement(build, directory, helper_restart)
        for failure in ("reject", "cancel"):
            directory = Path(temporary) / f"reenrol-{failure}"
            directory.mkdir(mode=0o700)
            forget_then_reenrol(build, directory, failure)
        directory = Path(temporary) / "commit-failure"
        directory.mkdir(mode=0o700)
        commit_failure_retracts_approval(build, directory)
        for removal_fails in (False, True):
            directory = Path(temporary) / f"store-write-{removal_fails}"
            directory.mkdir(mode=0o700)
            store_write_failure_after_acknowledgement(build, directory, removal_fails)
