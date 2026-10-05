# /// script
# requires-python = ">=3.11"
# dependencies = []
# ///
# Run: python3 -B tests/target_safety/window.py
"""Monotonic window and descriptor lifecycle qualification with tiny fixtures."""
from __future__ import annotations

import ctypes
import errno
import sys
import tempfile
import time
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
from target_safety.kernel import WINDOW_TIMEOUT_MESSAGE, Blocked, Watch


def deadline_persists_across_phases() -> None:
    """Given one monotonic window, When checks span phases, Then the deadline never resets."""
    with Watch.session() as watch:
        original = watch.deadline
        for _ in range(3):
            watch.check()
        assert watch.deadline == original
        assert original > time.monotonic()
    assert watch.closed
    print("PASS window single monotonic deadline persists across phases before expiry")


def check_blocks_at_deadline() -> None:
    """Given a live window, When the deadline is reached, Then block with the named message."""
    try:
        with Watch.session() as watch:
            watch.deadline = time.monotonic()
            watch.check()
    except Blocked as error:
        assert error.reason == "window_timeout", str(error)
        assert error.detail == WINDOW_TIMEOUT_MESSAGE
        assert watch.closed
        print(f"PASS window at-deadline blocks: {error}")
        return
    raise AssertionError("deadline boundary accepted")


def final_drain_expiry_closes_descriptor() -> None:
    """Given an expired window, When the session exits, Then fail the drain yet release the fd."""
    opened: list[Watch] = []
    try:
        with Watch.session() as watch:
            opened.append(watch)
            watch.deadline = time.monotonic() - 1
    except Blocked as error:
        assert error.reason == "window_timeout", str(error)
        assert error.detail == WINDOW_TIMEOUT_MESSAGE
        assert opened[0].closed
        print(f"PASS window final-drain expiry closes descriptor: {error}")
        return
    raise AssertionError("expired final drain accepted")


def enospc_later_watch_cleanup() -> None:
    """Given ENOSPC on a later registration, When it fails, Then release earlier watches."""
    fixture = Path(tempfile.mkdtemp(prefix="capacity-enospc-", dir="/tmp/opencode"))
    targets = []
    for index in range(3):
        path = fixture / f"o{index}"
        path.write_bytes(b"x")
        targets.append(path)
    registrations = {"count": 0}
    try:
        with Watch.session() as watch:
            real_add = watch.libc.inotify_add_watch

            def add_until_enospc(fd: int, path: bytes, mask: int) -> int:
                registrations["count"] += 1
                if registrations["count"] > 2:
                    ctypes.set_errno(errno.ENOSPC)
                    return -1
                return real_add(fd, path, mask)

            with patch.object(watch.libc, "inotify_add_watch", add_until_enospc):
                for target in targets:
                    watch.add(target)
    except OSError as error:
        assert error.errno == errno.ENOSPC, error
        assert registrations["count"] == 3
        assert watch.closed
        print(f"PASS window ENOSPC on later watch cleans up earlier watches; fixture={fixture}")
        return
    raise AssertionError("ENOSPC registration accepted")


def main() -> int:
    deadline_persists_across_phases()
    check_blocks_at_deadline()
    final_drain_expiry_closes_descriptor()
    enospc_later_watch_cleanup()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
