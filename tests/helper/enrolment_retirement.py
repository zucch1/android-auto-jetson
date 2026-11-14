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
            # The receiver's store does not approve the bond: its reconciliation hands the removal
            # to the helper's durable journal, so no receiver record is needed (or can pin capacity).
            time.sleep(1.5)
            assert not surface.state()["removed"]
            assert peer_in(directory, "pending-removals"), "the failed removal is not owned durably"
            assert command(surface.receiver, "pending-revocations") == "0"
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


def restart_receiver(stack: contextlib.ExitStack, build: Path, directory: Path, surface: Surface) -> None:
    """A fresh receiver process on the same trust store; it reconciles the helper on start."""
    surface.receiver = stack.enter_context(process([str(build / "aa_system_helper_fixture"), "receiver",
                                                    surface.address, str(directory / "store"), "bluez"], surface.env))
    assert line(surface.receiver) == "ready"
    control(surface.consumer, "RegisterConsumer", GLib.Variant("(s)", ("org.custom.BluezConsumer",)))


def start_helper(stack: contextlib.ExitStack, build: Path, directory: Path, surface: Surface) -> None:
    surface.daemon = stack.enter_context(process([str(build / "aa-system-helper"), "--private-bus",
                                                  surface.address, str(directory)], surface.env))


def stop_helper(surface: Surface) -> None:
    surface.daemon.terminate()
    assert surface.daemon.wait(timeout=5) == 0


def enrol(surface: Surface, directory: Path) -> None:
    request = surface.request()
    surface.confirm(request)
    accept(surface, request)
    command(surface.mock, "paired")
    wait(lambda: peer_in(directory, "managed-bonds") and command(surface.receiver, "pending-pairings") == "0", 8)


def receiver_exit_after_acknowledgement(build: Path, directory: Path, helper_restart: bool) -> None:
    """Finding A: the receiver exits after the helper acknowledged the bond, before its store write.

    The helper cannot know whether the write landed, so the bond is parked (neither admitted nor
    removed) until the restarted receiver's reconciliation answers: nothing is approved, so it goes."""
    with contextlib.ExitStack() as stack:
        surface = Surface(stack, build, directory)
        request = surface.request()
        surface.confirm(request)
        assert command(surface.receiver, "exit-after-ack") == "0"
        accept(surface, request)
        command(surface.mock, "paired")
        assert surface.receiver.wait(timeout=5) == 9, "the receiver did not exit at the acknowledgement"
        assert not peer_in(directory, "managed-bonds"), "a provisional bond was admitted"
        assert command(surface.mock, "AuthorizeService hfp") == "rejected"
        time.sleep(1.2)
        assert not surface.state()["removed"], "a bond was removed before the receiver answered"
        assert peer_in(directory, "pending-removals"), "the cleanup obligation was not durable"
        if helper_restart:
            stop_helper(surface)
            start_helper(stack, build, directory, surface)
            time.sleep(1.2)
            assert not surface.state()["removed"], "the restarted helper removed a bond before reconciliation"
        restart_receiver(stack, build, directory, surface)
        wait(lambda: len(surface.state()["removed"]) == 1, 8)
        wait(lambda: not peer_in(directory, "pending-removals"))
        assert not peer_in(directory, "managed-bonds")
        assert surface.phone() == 0
        print(f"PASS receiver_exit_after_acknowledgement_parks_then_reconciliation_removes_bond{'_across_helper_restart' if helper_restart else ''}", flush=True)


def receiver_exit_before_commit(build: Path, directory: Path, helper_restart: bool) -> None:
    """Commit crash window 1: the trust write is durable, the receiver exits before the commit."""
    with contextlib.ExitStack() as stack:
        surface = Surface(stack, build, directory)
        request = surface.request()
        surface.confirm(request)
        assert command(surface.receiver, "exit-before-commit") == "0"
        accept(surface, request)
        command(surface.mock, "paired")
        assert surface.receiver.wait(timeout=5) == 9, "the receiver did not exit before the commit"
        assert (directory / "store").exists(), "the trust write did not land before the commit"
        assert not peer_in(directory, "managed-bonds")
        time.sleep(1.2)
        assert not surface.state()["removed"], "the parked, approved bond was removed"
        if helper_restart:
            stop_helper(surface)
            start_helper(stack, build, directory, surface)
        restart_receiver(stack, build, directory, surface)
        wait(lambda: peer_in(directory, "managed-bonds") and not peer_in(directory, "pending-removals"), 8)
        assert not surface.state()["removed"], "reconciliation removed an approved bond"
        assert surface.phone() != 0, "the durable approval was lost"
        assert command(surface.receiver, "reconnect") == "0", "the approved bond was not admitted"
        print(f"PASS commit_crash_window_1_heals_on_reconciliation{'_across_helper_restart' if helper_restart else ''}", flush=True)


def helper_exit_between_commit_writes(build: Path, directory: Path) -> None:
    """Commit crash window 2: managed-bonds written, the journal entry not yet durably removed."""
    with contextlib.ExitStack() as stack:
        surface = Surface(stack, build, directory)
        enrol(surface, directory)
        stop_helper(surface)
        # The durable outcome of a helper crash between the two writes of the commit.
        (directory / "pending-removals").write_text("v1\n02:00:00:00:00:01 02:00:00:00:00:AA\n")
        start_helper(stack, build, directory, surface)
        # The live receiver sees the new helper incarnation and reconciles it.
        wait(lambda: not peer_in(directory, "pending-removals"), 8)
        assert peer_in(directory, "managed-bonds")
        assert not surface.state()["removed"], "restart cleanup removed an approved bond"
        wait(lambda: command(surface.receiver, "reconnect") == "0", 4)
        assert surface.phone() != 0
        print("PASS commit_crash_window_2_heals_on_reconciliation", flush=True)


def commit_failure_keeps_approval(build: Path, directory: Path) -> None:
    """The durable trust write succeeded but the commit fails: the approval stays (it is the
    authority) and reconciliation admits the parked bond."""
    with contextlib.ExitStack() as stack:
        surface = Surface(stack, build, directory)
        request = surface.request()
        surface.confirm(request)
        assert command(surface.receiver, "hold-commit") == "0"
        accept(surface, request)
        command(surface.mock, "paired")
        wait(lambda: peer_in(directory, "managed-bonds") and not peer_in(directory, "pending-removals"), 8)
        assert not surface.state()["removed"]
        assert surface.phone() != 0, "a durable approval was withdrawn"
        assert command(surface.receiver, "pending-revocations") == "0"
        assert command(surface.receiver, "reconnect") == "0"
        print("PASS commit_failure_keeps_approval_and_reconciliation_admits_bond", flush=True)


def forget_with_helper_down_then_receiver_restart(build: Path, directory: Path) -> None:
    """Finding 2: ForgetPhone removes trust durably while the helper is down; the receiver restarts.
    The helper's managed authority must not survive: reconciliation removes the bond, and the phone
    can enrol again."""
    with contextlib.ExitStack() as stack:
        surface = Surface(stack, build, directory)
        enrol(surface, directory)
        phone = surface.phone()
        stop_helper(surface)
        try:
            surface.forget(phone)
        except GLib.Error:
            pass  # the helper is down: its legs stay pending in a record the restart loses
        assert surface.phone() == 0, "the forget did not remove trust"
        surface.receiver.kill()
        surface.receiver.wait(timeout=3)
        start_helper(stack, build, directory, surface)
        time.sleep(1.2)
        assert peer_in(directory, "managed-bonds") and not surface.state()["removed"], \
            "the helper acted before the receiver answered"
        restart_receiver(stack, build, directory, surface)
        wait(lambda: len(surface.state()["removed"]) == 1, 8)
        assert not peer_in(directory, "managed-bonds") and peer_in(directory, "revoked-bonds")
        wait(lambda: not peer_in(directory, "pending-removals"))
        command(surface.mock, "restore")
        command(surface.mock, "unpaired")
        enrol(surface, directory)
        assert surface.phone() != 0
        assert command(surface.receiver, "reconnect") == "0"
        print("PASS forget_with_helper_down_then_receiver_restart_removes_bond_and_allows_reenrolment", flush=True)


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
        # The new enrolment cleared the history (its journal entry held the authority meanwhile), so
        # an approval of this identity found on revoked-bonds is never one committed after it.
        assert not peer_in(directory, "managed-bonds") and not peer_in(directory, "revoked-bonds")
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
        commit_failure_keeps_approval(build, directory)
        for helper_restart in (False, True):
            directory = Path(temporary) / f"exit-before-commit-{helper_restart}"
            directory.mkdir(mode=0o700)
            receiver_exit_before_commit(build, directory, helper_restart)
        directory = Path(temporary) / "helper-exit-between-writes"
        directory.mkdir(mode=0o700)
        helper_exit_between_commit_writes(build, directory)
        directory = Path(temporary) / "forget-helper-down"
        directory.mkdir(mode=0o700)
        forget_with_helper_down_then_receiver_restart(build, directory)
        for removal_fails in (False, True):
            directory = Path(temporary) / f"store-write-{removal_fails}"
            directory.mkdir(mode=0o700)
            store_write_failure_after_acknowledgement(build, directory, removal_fails)
