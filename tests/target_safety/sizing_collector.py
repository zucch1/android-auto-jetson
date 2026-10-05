# /// script
# requires-python = ">=3.11"
# dependencies = []
# ///
# Run: python3 -B tests/target_safety/sizing_collector.py
"""Bounded collector qualification: caps, partial input, escaped descendants.

Real local child processes and pipes only; no kernel mutation and no target
contact. Capped or unfinished data is retained for evidence but never accepted.
Also hosts the deterministic selector fixtures shared by the selector failure
regressions: scripted clocks and labeled injected faults, never organic
observations.
"""
from __future__ import annotations

import io
import json
import os
import selectors
import signal
import subprocess
import sys
import tempfile
import time
from contextlib import redirect_stdout, suppress
from dataclasses import dataclass
from pathlib import Path
from typing import IO

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
sys.path.insert(0, str(Path(__file__).resolve().parent))
from target_safety import collector
from target_safety.collector import CollectionPolicy, Collector
from target_safety.supervisor import Supervisor
from ceiling_setting_fixtures import fake_setting

PY = (sys.executable, "-B", "-c")


class FakeClock:
    """Scripted monotonic seconds; deterministic, no scheduler dependence."""

    def __init__(self, start: float) -> None:
        self.now = start

    def __call__(self) -> float:
        return self.now

    def set(self, when: float) -> None:
        self.now = when


@dataclass(frozen=True, slots=True)
class FaultLabels:
    """Labeled injected selector faults; None means the call succeeds."""

    register: str | None = None
    unregister: str | None = None
    select: str | None = None


class ScriptedSelector(selectors.BaseSelector):
    """Deterministic selector over a fake clock with labeled injected faults.

    Waits honor the requested timeout: the clock advances to the next scripted
    due event inside the timeout window, or to now + timeout when none is due.
    """

    def __init__(self, clock: FakeClock, due: list[tuple[float, tuple[str, ...]]],
                 faults: FaultLabels, sticky_out: bool = False) -> None:
        self._clock = clock
        self._due = due
        self._faults = faults
        self._sticky = sticky_out
        self._keys: dict[int, selectors.SelectorKey] = {}
        self._roles: dict[str, int] = {}
        self.timeouts: list[float] = []
        self.closed = False

    @staticmethod
    def _fd_of(fileobj: IO[bytes] | int) -> int:
        return fileobj if isinstance(fileobj, int) else fileobj.fileno()

    def register(self, fileobj: IO[bytes] | int, events: int, data: object = None) -> selectors.SelectorKey:
        if self._faults.register is not None:
            raise OSError(self._faults.register)
        key = selectors.SelectorKey(fileobj, self._fd_of(fileobj), events, data)
        self._keys[key.fd] = key
        self._roles[str(data)] = key.fd
        return key

    def unregister(self, fileobj: IO[bytes] | int) -> selectors.SelectorKey:
        if self._faults.unregister is not None:
            raise OSError(self._faults.unregister)
        key = self._keys.pop(self._fd_of(fileobj), None)
        if key is None:
            raise KeyError(f"injected unknown fileobj {fileobj!r}")
        self._roles = {role: fd for role, fd in self._roles.items() if fd != key.fd}
        return key

    def select(self, timeout: float | None = None) -> list[tuple[selectors.SelectorKey, int]]:
        if self._faults.select is not None:
            raise OSError(self._faults.select)
        self.timeouts.append(float(timeout))
        if self._sticky and "out" in self._roles:
            self._clock.set(self._clock.now + 0.05)
            return [(self._keys[self._roles["out"]], selectors.EVENT_READ)]
        limit = self._clock.now + float(timeout)
        if self._due and self._due[0][0] <= limit:
            when, roles = self._due.pop(0)
            self._clock.set(max(self._clock.now, when))
            return [(self._keys[self._roles[r]], selectors.EVENT_READ) for r in roles if r in self._roles]
        self._clock.set(limit)
        return []

    def get_map(self) -> dict[int, selectors.SelectorKey]:
        return dict(self._keys)

    def close(self) -> None:
        self.closed = True


class CloseFaultStream:
    """Real pipe stream whose close releases the fd and then faults (injected)."""

    def __init__(self, stream: IO[bytes]) -> None:
        self._stream = stream

    def fileno(self) -> int:
        return self._stream.fileno()

    def close(self) -> None:
        self._stream.close()
        raise OSError("injected close fault after release (scripted)")


class FaultCloseView:
    """Real Popen view with stdout and stdin replaced by close-faulting streams."""

    def __init__(self, proc: subprocess.Popen[bytes]) -> None:
        self._proc = proc
        assert proc.stdout is not None and proc.stdin is not None
        self.stdout: IO[bytes] | None = CloseFaultStream(proc.stdout)
        self.stderr = proc.stderr
        self.stdin: IO[bytes] | None = CloseFaultStream(proc.stdin)

    def poll(self) -> int | None:
        return self._proc.poll()


class ExitedProcView:
    """Real Popen view reporting scripted exit; poll is injected, not scheduled."""

    def __init__(self, proc: subprocess.Popen[bytes]) -> None:
        self.stdin = proc.stdin
        self.stdout = proc.stdout
        self.stderr = proc.stderr

    def poll(self) -> int:
        return 0


def open_fds() -> dict[int, str]:
    """Snapshot open fd targets via /proc; the listing's own transient fd is skipped."""
    targets: dict[int, str] = {}
    for entry in os.listdir("/proc/self/fd"):
        try:
            targets[int(entry)] = os.readlink(f"/proc/self/fd/{entry}")
        except FileNotFoundError:
            continue
    return targets


def spawn_child(source: str, *, pipes: bool = True) -> subprocess.Popen[bytes]:
    return subprocess.Popen(PY + (source,),
                            stdin=subprocess.PIPE if pipes else subprocess.DEVNULL,
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE)


def _run(source: str, stdin: bytes | None, *, policy: CollectionPolicy = CollectionPolicy(),
         sample=None, deadline_seconds: float = 10.0):
    proc = subprocess.Popen(PY + (source,), stdin=subprocess.PIPE if stdin is not None else subprocess.DEVNULL,
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    try:
        return Collector(proc, stdin, time.monotonic() + deadline_seconds,
                         policy=policy, sample=sample).run()
    finally:
        if proc.poll() is None:
            proc.kill()
            proc.wait(timeout=5)


def combined_flood_stops_and_never_accepts() -> None:
    # Given: a child flooding both streams past the combined cap.
    policy = CollectionPolicy(combined_limit=64 * 1024, record_limit=1 << 20)
    result = _run("import sys\nwhile True: sys.stdout.write('x' * 4095 + chr(10)); "
                  "sys.stderr.write('y' * 4095 + chr(10))",
                  None, policy=policy, deadline_seconds=8.0)
    assert result.invalidated and result.truncated
    assert result.combined_bytes <= 64 * 1024
    assert result.first_cause and "combined_output_limit" in result.first_cause
    assert len(result.violation_details) <= collector.MAX_VIOLATION_DETAILS
    assert result.combined_bytes == len(result.stdout) + len(result.stderr)
    print(f"PASS combined flood capped at {result.combined_bytes} bytes and invalidated")


def oversized_record_stops() -> None:
    policy = CollectionPolicy(combined_limit=8 * 1024 * 1024, record_limit=1024)
    result = _run("import sys\nsys.stdout.write('x' * 100000); sys.stdout.flush()", None, policy=policy)
    assert result.invalidated and not result.truncated
    assert result.first_cause and "oversized_record" in result.first_cause
    print("PASS oversized record stops the guard with the first cause retained")


def partial_stdin_and_nonreading_child_stay_bounded() -> None:
    closed = _run("import os, sys\nos.close(0); sys.stdout.write('done')", b"x" * 500_000)
    assert closed.stdin_complete is False and closed.stdout == b"done"
    started = time.monotonic()
    nonreading = _run("import time\ntime.sleep(30)", b"y" * 200_000,
                      policy=CollectionPolicy(drain_grace=0.2), deadline_seconds=2.0)
    assert time.monotonic() - started < 8.0
    assert nonreading.timed_out and nonreading.invalidated
    print(f"PASS partial stdin and non-reading child bounded ({nonreading.first_cause})")


def escaped_descendant_drain_is_bounded() -> None:
    # Given: a child whose escaped descendant keeps the pipe open after exit.
    with tempfile.TemporaryDirectory(dir="/tmp/opencode") as directory:
        pidfile = Path(directory) / "leak.pid"
        source = ("import subprocess, sys\n"
                  "g = subprocess.Popen([sys.executable, '-c', 'import os,time; os.setsid(); time.sleep(30)'])\n"
                  f"open({str(pidfile)!r}, 'w').write(str(g.pid))\n"
                  "print('partial', flush=True)\n")
        started = time.monotonic()
        try:
            result = _run(source, None, policy=CollectionPolicy(drain_grace=0.2), deadline_seconds=3.0)
        finally:
            if pidfile.exists():
                with suppress(ProcessLookupError):
                    os.killpg(int(pidfile.read_text()), signal.SIGKILL)
        assert time.monotonic() - started < 10.0
        assert result.stdout.startswith(b"partial")
        assert result.invalidated and result.first_cause == "drain_incomplete"
    print("PASS escaped descendant holding the pipe cannot extend collection")


def sampling_is_capped_and_resource_aborts_stop() -> None:
    calls: list[float] = []
    def sample():
        calls.append(time.monotonic())
        return None
    result = _run("import sys, time\n"
                  "for _ in range(30):\n"
                  "    sys.stdout.write('x' * 10); sys.stdout.flush(); time.sleep(0.02)\n",
                  None, sample=sample, deadline_seconds=8.0)
    assert result.samples == len(calls) >= 2, (result.samples, calls)
    gaps = [b - a for a, b in zip(calls, calls[1:])]
    assert all(gap <= 0.25 for gap in gaps), gaps
    aborting = _run("import time\ntime.sleep(30)", None,
                    sample=lambda: "mem_available fixture", deadline_seconds=5.0)
    assert aborting.invalidated and "resource_abort" in (aborting.first_cause or "")
    print(f"PASS sampling bounded at <=100ms cadence ({result.samples} samples); resource abort stops")


def bounded_growth_and_compact_receipt() -> None:
    # Given: a finite serialization stress well past the cap.
    policy = CollectionPolicy(combined_limit=128 * 1024, record_limit=64 * 1024)
    result = _run("import sys\n[sys.stdout.write('z' * 30000 + chr(10)) for _ in range(200)]",
                  None, policy=policy, deadline_seconds=8.0)
    assert result.combined_bytes <= 128 * 1024 and result.invalidated
    assert len(result.stdout_sha256) == 64 and len(result.stderr_sha256) == 64
    wire = json.dumps({"combined_bytes": result.combined_bytes, "records": result.records,
                       "stdout_sha256": result.stdout_sha256, "stderr_sha256": result.stderr_sha256,
                       "truncated": result.truncated, "invalidated": result.invalidated,
                       "first_cause": result.first_cause})
    assert len(wire) < 2048 < result.combined_bytes
    print(f"PASS bounded growth ({result.combined_bytes} B retained) and compact wire ({len(wire)} B)")


def supervisor_rejects_capped_collection_and_restores() -> None:
    # Given: a flooding guard inside a real window with local fake authority.
    with tempfile.TemporaryDirectory(dir="/tmp/opencode") as directory:
        root = Path(directory)
        setting, state = fake_setting(root)
        flood = (sys.executable, "-B", "-c",
                 "import sys\nwhile True: sys.stdout.write('x' * 8192); sys.stdout.flush()")
        supervisor = Supervisor(setting, flood)
        supervisor.guard_policy = CollectionPolicy(combined_limit=32 * 1024, record_limit=1 << 20)
        out = io.StringIO()
        with redirect_stdout(out):
            outcome = supervisor.run()
        assert not outcome.accepted and outcome.guard_valid is False
        assert outcome.restored_original and state.read_text() == "65536"
        assert outcome.collection is not None and outcome.collection["truncated"]
        assert outcome.sizing is None and outcome.mode == "ceiling"
    print("PASS supervisor stops a flooding guard, restores, and never accepts capped data")


def main() -> int:
    combined_flood_stops_and_never_accepts()
    oversized_record_stops()
    partial_stdin_and_nonreading_child_stay_bounded()
    escaped_descendant_drain_is_bounded()
    sampling_is_capped_and_resource_aborts_stop()
    bounded_growth_and_compact_receipt()
    supervisor_rejects_capped_collection_and_restores()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
