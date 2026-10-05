# /// script
# requires-python = ">=3.11"
# dependencies = []
# ///
# Run: python3 -B tests/target_safety/ceiling_guard.py
"""Guard-child and transport lifecycle qualification with real local subprocesses.

Actual process-lifecycle cases: real subprocesses, real process groups, real
signals and reap. The setting boundary is not involved here. No kernel mutation.
"""
from __future__ import annotations

import os
import signal
import sys
import tempfile
import time
from contextlib import ExitStack, suppress
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
from target_safety.guard import GuardChild
from target_safety.runner import run_owned
from target_safety.local_capture import LocalCapture
from ceiling_fixtures import MARKER, guard_argv


def _tmp() -> Path:
    return Path(tempfile.mkdtemp(prefix="ceiling-guard-", dir="/tmp/opencode"))


def guard_success_failure_crash() -> None:
    """Given success/fail/crash guards, When run, Then reflect exit status faithfully."""
    tmp = _tmp()
    ok = GuardChild(guard_argv(tmp, "success"), 5.0)
    ok.spawn()
    result = ok.wait()
    assert result.exit_status == 0 and MARKER in result.stdout and not result.timed_out
    bad = GuardChild(guard_argv(tmp, "fail"), 5.0)
    bad.spawn()
    assert bad.wait().exit_status == 3
    crash = GuardChild(guard_argv(tmp, "crash"), 5.0)
    crash.spawn()
    crashed = crash.wait()
    assert crashed.exit_status != 0 and crashed.exit_status < 0
    print(f"PASS guard success/failure/crash exit statuses; fixture={tmp}")


def guard_hang_deadline_terminates() -> None:
    """Given a hanging guard, When the monotonic deadline fires, Then terminate and reap."""
    tmp = _tmp()
    guard = GuardChild(guard_argv(tmp, "hang"), 1.0)
    guard.spawn()
    result = guard.wait()
    assert result.timed_out and result.killed
    assert result.exit_status != 0
    assert guard.terminated
    print(f"PASS guard hang killed at monotonic deadline independent of Watch.check; fixture={tmp}")


def guard_term_ignoring_children_reaped() -> None:
    """Given a guard whose child ignores TERM, When terminated, Then KILL reaps the child."""
    tmp = _tmp()
    pidfile = tmp / "grandchild.pid"
    guard = GuardChild(guard_argv(tmp, "ignore-term", str(pidfile)), 10.0)
    guard.spawn()
    deadline = time.monotonic() + 3.0
    while not pidfile.exists() and time.monotonic() < deadline:
        time.sleep(0.05)
    assert pidfile.exists(), "grandchild did not start"
    grandchild = int(pidfile.read_text())
    os.kill(grandchild, 0)
    guard.terminate()
    gone = False
    recheck = time.monotonic() + 3.0
    while time.monotonic() < recheck:
        try:
            os.kill(grandchild, 0)
        except ProcessLookupError:
            gone = True
            break
        time.sleep(0.05)
    assert gone, "TERM-ignoring grandchild survived TERM then KILL"
    assert guard.terminated
    print(f"PASS guard TERM-ignoring child reaped by unconditional group KILL; fixture={tmp}")


def transport_timeout_preserves_partial_output() -> None:
    """Given a transport that emits partial output then hangs, When it times out, Then keep it."""
    tmp = _tmp()
    with ExitStack() as stack:
        capture = LocalCapture.reserve(stack, tmp / 'transport.json')
        result = run_owned(guard_argv(tmp, "partial-hang"), capture=capture, timeout_seconds=1.0, cwd=tmp)
    assert result.timed_out
    assert "partial-output" in result.stdout
    assert result.exit_status != 0
    print(f"PASS transport timeout preserves partial output and owns process group; fixture={tmp}")


def transport_term_ignoring_descendant_bounded() -> None:
    """Given a TERM-ignoring descendant holding stdout outside the group, Then collection is bounded."""
    tmp = _tmp()
    pidfile = tmp / "leak.pid"
    started = time.monotonic()
    try:
        with ExitStack() as stack:
            capture = LocalCapture.reserve(stack, tmp / 'transport.json')
            result = run_owned(guard_argv(tmp, "leak-hold", str(pidfile)), capture=capture, timeout_seconds=1.0, cwd=tmp)
    finally:
        if pidfile.exists():
            with suppress(ProcessLookupError):
                os.killpg(int(pidfile.read_text()), signal.SIGKILL)
    elapsed = time.monotonic() - started
    assert result.timed_out
    assert pidfile.exists(), "leak descendant did not start"
    assert elapsed < 20.0, f"collection not bounded: {elapsed}s"
    print(f"PASS transport TERM-ignoring out-of-group descendant, bounded collection ({elapsed:.1f}s); fixture={tmp}")


def main() -> int:
    guard_success_failure_crash()
    guard_hang_deadline_terminates()
    guard_term_ignoring_children_reaped()
    transport_timeout_preserves_partial_output()
    transport_term_ignoring_descendant_bounded()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
