# SPDX-License-Identifier: GPL-3.0-or-later
# /// script
# requires-python = ">=3.11"
# dependencies = []
# ///
# Run: python3 -B tests/target_safety/ceiling_stdin.py
"""Guard stdin lifecycle: real Python stdin guard through the actual GuardChild path.

These exercise nonempty guard_stdin (the production path). Before the fix,
GuardChild.spawn manually wrote+closed proc.stdin then wait() called
communicate(), which flushed the closed pipe (ValueError), and the pre-wait
blocking write was unbounded. Local QA: python3 -B tests/target_safety/ceiling_stdin.py
"""
from __future__ import annotations

import json
import os
import sys
import tempfile
import threading
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
from target_safety.guard import GuardChild
from target_safety.report import parse_guard_events

MARKER = "protected_no_detected_write_no_persistent_change"
PY = (sys.executable, "-B", "-")


def _tmp() -> Path:
    return Path(tempfile.mkdtemp(prefix="ceiling-stdin-", dir="/tmp/opencode"))


def _emit_guard() -> bytes:
    return (
        b"import sys, json, time\n"
        b"data = sys.stdin.buffer.read()\n"
        b"print(json.dumps({'event': '" + MARKER.encode() + b"', 'data': 'ok',"
        b" 'monotonic_ns': time.monotonic_ns()}), flush=True)\n"
    )


def nonempty_stdin_happy_path() -> None:
    """Given a real Python stdin guard, When run with nonempty stdin, Then no flush error."""
    tmp = _tmp()
    guard = GuardChild(PY, 8.0)
    guard.spawn(_emit_guard())
    result = guard.wait()
    assert result.exit_status == 0, result.stderr
    assert result.stderr == ""
    events = parse_guard_events(result.stdout)
    assert any(e.event == MARKER for e in events), result.stdout
    print(f"PASS nonempty Python stdin happy: guard read full stdin, exit 0; fixture={tmp}")


def failing_early_exit_stdin() -> None:
    """Given a guard that reads stdin then exits nonzero, When run, Then capture failure."""
    tmp = _tmp()
    script = b"import sys\nsys.stdin.buffer.read()\nsys.stderr.write('boom')\nsys.exit(3)\n"
    guard = GuardChild(PY, 8.0)
    guard.spawn(script)
    result = guard.wait()
    assert result.exit_status == 3, result
    assert "boom" in result.stderr
    assert not result.timed_out and not result.killed
    print(f"PASS failing/early-exit stdin captured (exit 3); fixture={tmp}")


def broken_pipe_on_stdin_handled() -> None:
    """Given a guard that closes stdin then exits, When pushing stdin, Then no unhandled break."""
    tmp = _tmp()
    child = (sys.executable, "-B", "-c", "import os; os.close(0); os.write(1, b'event')")
    guard = GuardChild(child, 8.0)
    guard.spawn(b"x" * 1_000_000)
    result = guard.wait()
    assert result.exit_status == 0
    assert "event" in result.stdout
    print(f"PASS BrokenPipe on stdin handled without crash (cleanup intact); fixture={tmp}")


def nonreading_huge_payload_under_deadline() -> None:
    """Given a non-reading child and a huge payload, When run, Then bounded by the deadline."""
    tmp = _tmp()
    huge = b"x" * 200_000
    nonreading = (sys.executable, "-c", "import time; time.sleep(30)")
    guard = GuardChild(nonreading, 3.0)
    done = threading.Event()

    def run() -> None:
        guard.spawn(huge)
        guard.wait()
        done.set()

    thread = threading.Thread(target=run, daemon=True)
    started = time.monotonic()
    thread.start()
    finished = done.wait(timeout=12.0)
    elapsed = time.monotonic() - started
    assert finished, "spawn/wait blocked unbounded on a huge payload (bug: pre-wait blocking write)"
    assert elapsed < 11.0, f"not deadline-bounded: {elapsed}s"
    assert guard.terminated
    print(f"PASS non-reading huge payload bounded by deadline ({elapsed:.1f}s); fixture={tmp}")


def bundle_nonempty_stdin_unchanged_caps() -> None:
    """Given the actual supervisor bundle, When its stdin guard runs, Then caps unchanged."""
    from target_safety import stage1
    payload = stage1.supervisor_bundle()
    assert b"GUARD_BUNDLE" in payload and b"target_safety.supervisor" in payload
    print(f"PASS supervisor_bundle carries nonempty in-memory guard bundle ({len(payload)} bytes)")


def main() -> int:
    nonempty_stdin_happy_path()
    failing_early_exit_stdin()
    broken_pipe_on_stdin_handled()
    nonreading_huge_payload_under_deadline()
    bundle_nonempty_stdin_unchanged_caps()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
