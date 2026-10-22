# SPDX-License-Identifier: GPL-3.0-or-later
# /// script
# requires-python = ">=3.12"
# dependencies = []
# ///
# Run: /usr/bin/python3 -B tests/helper/retirement_budget.py build/host-dev [durable|703|mutex]
from __future__ import annotations

import contextlib
from concurrent.futures import ThreadPoolExecutor, wait as wait_futures
import hashlib
import os
from pathlib import Path
import signal
import sys
import tempfile
import time

from gi.repository import Gio, GLib

from bluez_mock import connect
from bluez_runtime import command, control, rejected, wait
from composition_runtime import ComposedSurface
from private_bus import line, process, send


def stalled_forget(build: Path, not_durable: bool) -> None:
    with tempfile.TemporaryDirectory(prefix="budget-", dir=build) as temporary, contextlib.ExitStack() as stack:
        # Given: a committed wireless approval with a live source on the actual composed daemon.
        surface = ComposedSurface(stack, build, Path(temporary))
        phone = surface.approve_and_acquire()
        assert command(surface.mock, "NewConnection") == "allowed"
        if not_durable:
            assert command(surface.receiver, "fault-dir") == "0"
        expected = "Trust store replaced; crash durability unconfirmed." if not_durable else "system helper unavailable"
        for attempt in range(2):
            # When: only this fixture's helper child is paused through the entire attempt.
            surface.daemon.send_signal(signal.SIGSTOP)
            _, status = os.waitpid(surface.daemon.pid, os.WUNTRACED)
            assert os.WIFSTOPPED(status)
            started = time.monotonic()
            try:
                try:
                    surface.forget(phone)
                except GLib.Error as error:
                    if not_durable:
                        assert expected in str(error), str(error)
                    else:
                        assert any(message in str(error) for message in
                                   ("system helper unavailable", "operation timed out")), str(error)
                else:
                    raise AssertionError("stalled helper falsely completed revocation")
                elapsed = time.monotonic() - started
            finally:
                surface.daemon.send_signal(signal.SIGCONT)
            # Then: initial and retry share the bound, preserve703, and keep unknown effects pending.
            print(f"stalled_forget not_durable={not_durable} attempt={attempt} seconds={elapsed:.6f}", flush=True)
            assert elapsed < 1.15, elapsed
            assert surface.phone() == 0
            assert command(surface.receiver, "pending-revocations") == "1"
            assert command(surface.receiver, f"progress {phone}") == "0 0"
            assert command(surface.receiver, f"retirements {phone}") == "1"
            wait(lambda: not surface.nm_state()["table"] and len(surface.state()["removed"]) == 1)
        assert command(surface.receiver, "fault-clear") == "0"

        def finish() -> bool:
            if not_durable:
                surface.forget_error(phone, expected)
            else:
                try:
                    surface.forget(phone)
                except GLib.Error as error:
                    assert "system helper unavailable" in str(error), str(error)
            return command(surface.receiver, "pending-revocations") == "0"

        wait(finish)
        assert command(surface.mock, "fd-eof") == "closed"
        assert command(surface.mock, "RequestAuthorization") == "rejected"
        assert command(surface.receiver, f"retirements {phone}") == "none"
        command(surface.mock, "restore")
        surface.confirm(surface.request())
        print(f"PASS composed_stalled_retirement_budget_and_retry not_durable={not_durable}", flush=True)


def contended_forget(build: Path) -> None:
    with tempfile.TemporaryDirectory(prefix="mutex-", dir=build) as temporary, contextlib.ExitStack() as stack:
        # Given: existing recording helper's entered handshake proves a worker owns the shared client mutex.
        directory = Path(temporary)
        fixture = build / "aa_system_helper_fixture"
        env = dict(os.environ, AA_HELPER_HOLD_RELEASE="1")
        (directory / "session.uid").write_text(f"{os.getuid()}\n")
        (directory / "receiver.sha256").write_text(hashlib.sha256(fixture.read_bytes()).hexdigest() + "\n")
        for name in ("session.uid", "receiver.sha256"):
            (directory / name).chmod(0o600)
        buses = [stack.enter_context(process(["dbus-daemon", "--session", "--nofork", "--print-address=1"], env))
                 for _ in range(2)]
        address, env["DBUS_SESSION_BUS_ADDRESS"] = [line(bus) for bus in buses]
        consumer = connect(env["DBUS_SESSION_BUS_ADDRESS"])
        helper = stack.enter_context(process([str(fixture), "helper", address, str(directory), str(directory / "effects")], env))
        assert line(helper) == "ready"
        receiver = stack.enter_context(process([str(fixture), "receiver", address, str(directory / "store"), "unused"], env))
        assert line(receiver) == "ready"
        consumer.call_sync("org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", "RequestName",
            GLib.Variant("(su)", ("org.custom.BudgetConsumer", 0)), None, Gio.DBusCallFlags.NONE, 1000, None)
        control(consumer, "RegisterConsumer", GLib.Variant("(s)", ("org.custom.BudgetConsumer",)))
        request = control(consumer, "RequestAddPhone").unpack()[0]
        control(consumer, "ConfirmPhonePairing", GLib.Variant("(t)", (request,)))
        phone = int(command(receiver, "phone").split()[1])
        command(receiver, "worker-release")
        assert line(helper, 2) == "entered"

        def forget() -> tuple[float, str]:
            started = time.monotonic()
            try:
                control(consumer, "ForgetPhone", GLib.Variant("(t)", (phone,)))
            except GLib.Error as error:
                return time.monotonic() - started, str(error)
            return time.monotonic() - started, "success"

        # When: committed forget must return before releasing the blocked helper RPC.
        with ThreadPoolExecutor(max_workers=1) as executor:
            future = executor.submit(forget)
            try:
                completed, _ = wait_futures([future], timeout=1.15)
                bounded = bool(completed)
            finally:
                send(helper, "release")
            elapsed, error = future.result(timeout=4)
        print(f"contended_forget completed_before_release={bounded} seconds={elapsed:.6f} result={error}", flush=True)
        command(receiver, "join-first")
        # Then: the exact closed source survives the timed lock failure and is retired on retry.
        assert bounded and elapsed < 1.15
        assert "success" != error
        assert command(receiver, "pending-revocations") == "1"
        assert command(receiver, f"progress {phone}") == "0 0"
        assert command(receiver, f"retirements {phone}") == "1"
        rejected(consumer, "CancelPhonePairing", GLib.Variant("(t)", (request,)))
        control(consumer, "ForgetPhone", GLib.Variant("(t)", (phone,)))
        assert command(receiver, "pending-revocations") == "0"
        request = control(consumer, "RequestAddPhone").unpack()[0]
        control(consumer, "ConfirmPhonePairing", GLib.Variant("(t)", (request,)))
        print("PASS shared_client_mutex_budget_retains_exact_source_for_retry", flush=True)


def main() -> None:
    build = Path(sys.argv[1]).resolve()
    cases = {"durable": lambda: stalled_forget(build, False), "703": lambda: stalled_forget(build, True),
             "mutex": lambda: contended_forget(build)}
    for name in sys.argv[2:] or cases:
        cases[name]()


if __name__ == "__main__":
    main()
