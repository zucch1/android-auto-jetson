# SPDX-License-Identifier: GPL-3.0-or-later
# /// script
# requires-python = ">=3.11"
# dependencies = []
# ///
# Run: python3 -B tests/target_safety/sizing_rss_lifecycle.py
"""Lifecycle-aware child RSS sampling: exit-confirmed skips, live faults fail closed.

Real local children, real pipes and a local fake setting authority only; no
kernel mutation and no target contact. A child RSS reading is taken only while
the child's own Popen poll confirms it runs: a terminal child never needs one,
an exit between the confirmation and the reading never fakes a fault, and a
confirmed-running malformed or missing reading plus every host and supervisor
fault still fail closed. No RSS value is fabricated; the observed maximum is
kept or stays none, and host/supervisor monitoring continues through drains.
"""
from __future__ import annotations

import io
import json
import os
import signal
import subprocess
import sys
import tempfile
import time
from contextlib import redirect_stdout, suppress
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
sys.path.insert(0, str(Path(__file__).resolve().parent))
from target_safety import resource
from target_safety.collector import CollectionPolicy, Collector
from target_safety.resource import ResourceError, Sampler
from target_safety.supervisor import Supervisor
from ceiling_fixtures import guard_argv
from ceiling_setting_fixtures import fake_setting

GIB = 1024 ** 3
MIB = 2 ** 20
PY = (sys.executable, "-B", "-c")
FINAL_EVENT = "protected_no_detected_write_no_persistent_change"


def meminfo(bytes_available: int) -> Path:
    root = Path(tempfile.mkdtemp(prefix="lifecycle-meminfo-", dir="/tmp/opencode"))
    path = root / "meminfo"
    path.write_text(f"MemTotal: 1 kB\nMemAvailable:  {bytes_available // 1024} kB\n")
    return path


def status(kib: int) -> str:
    return f"Name:\tfixture\nVmRSS:\t{kib} kB\n"


def fixture_proc(child: tuple[int, str] | None = None) -> Path:
    """Proc root with the supervisor self reading and one optional child reading."""
    root = Path(tempfile.mkdtemp(prefix="lifecycle-proc-", dir="/tmp/opencode"))
    self_status = root / str(os.getpid()) / "status"
    self_status.parent.mkdir()
    self_status.write_text(status(50 * 1024))
    if child is not None:
        child_status = root / str(child[0]) / "status"
        child_status.parent.mkdir()
        child_status.write_text(child[1])
    return root


class ScriptedPoll:
    """Popen.poll-shaped liveness script; exhausted calls report a running child."""

    def __init__(self, *results: int | None) -> None:
        self._results = list(results)
        self.calls = 0

    def __call__(self) -> int | None:
        self.calls += 1
        return self._results.pop(0) if self._results else None


def terminal_child_skips_rss_reading_before_sample() -> None:
    # Given: a poll-confirmed exited child whose /proc reading is already gone.
    root = fixture_proc()
    poll = ScriptedPoll(0)
    sampler = Sampler(4242, meminfo=meminfo(4 * GIB), proc=root, child_poll=poll)
    # When: one reading pair is sampled.
    reason = sampler.sample()
    # Then: no child reading is needed, nothing is fabricated and host/supervisor persist.
    assert reason is None, reason
    assert poll.calls == 1, poll.calls
    extrema = sampler.extrema()
    assert extrema.max_guard_rss_bytes is None, extrema
    assert extrema.min_mem_available_bytes == 4 * GIB, extrema
    assert extrema.max_supervisor_rss_bytes == 50 * MIB, extrema
    print("PASS terminal child never needs a child RSS reading; host and supervisor readings persist")


def observed_max_survives_confirmed_exit() -> None:
    # Given: a running child with an observed reading, then that reading reaped away.
    root = fixture_proc((4242, status(256 * 1024)))
    poll = ScriptedPoll(None, None, 0)
    sampler = Sampler(4242, meminfo=meminfo(4 * GIB), proc=root, child_poll=poll)
    assert sampler.sample() is None
    assert sampler.extrema().max_guard_rss_bytes == 256 * MIB
    (root / "4242" / "status").unlink()
    # When: the next sample races the exit between confirmation and reading.
    reason = sampler.sample()
    # Then: the race never fakes a fault and the observed maximum is kept, never zeroed.
    assert reason is None, reason
    assert poll.calls == 3, poll.calls
    assert sampler.extrema().max_guard_rss_bytes == 256 * MIB, sampler.extrema()
    print("PASS confirmed exit keeps the previously observed child maximum")


def exit_between_liveness_and_reading_never_fakes_fault() -> None:
    # Given: a confirmed-running child whose required reading vanishes before the read.
    root = fixture_proc()
    poll = ScriptedPoll(None, 0)
    sampler = Sampler(4242, meminfo=meminfo(4 * GIB), proc=root, child_poll=poll)
    # When: the child RSS boundary failure is rechecked against the same poll.
    reason = sampler.sample()
    # Then: confirmed exit suppresses the false fault after exactly one recheck.
    assert reason is None, reason
    assert poll.calls == 2, poll.calls
    extrema = sampler.extrema()
    assert extrema.max_guard_rss_bytes is None, extrema
    assert extrema.min_mem_available_bytes == 4 * GIB, extrema
    assert extrema.max_supervisor_rss_bytes == 50 * MIB, extrema
    print("PASS exit between liveness confirmation and reading never fakes a child RSS fault")


def live_malformed_child_reading_fails_closed() -> None:
    # Given: a confirmed-running child carrying a malformed required reading.
    root = fixture_proc((4242, "Name:\tfixture\nVmRSS:\tnot-a-number kB\n"))
    sampler = Sampler(4242, meminfo=meminfo(4 * GIB), proc=root, child_poll=ScriptedPoll())
    try:
        sampler.sample()
    except ResourceError as error:
        assert error.operation == "rss_malformed", error
    else:
        raise AssertionError("live malformed child reading accepted")
    print("PASS confirmed-running malformed child reading fails closed")


def live_missing_child_reading_fails_closed() -> None:
    # Given: a confirmed-running child whose required reading is missing.
    root = fixture_proc()
    sampler = Sampler(4242, meminfo=meminfo(4 * GIB), proc=root, child_poll=ScriptedPoll())
    try:
        sampler.sample()
    except ResourceError as error:
        assert error.operation == "rss_unavailable", error
    else:
        raise AssertionError("live missing child reading accepted")
    print("PASS confirmed-running missing child reading fails closed")


def host_fault_fails_closed_after_child_exit() -> None:
    # Given: a poll-confirmed exited child and a malformed host reading.
    bad = meminfo(4 * GIB)
    bad.write_text("MemTotal: 1 kB\nMemAvailable:  nope kB\n")
    sampler = Sampler(4242, meminfo=bad, proc=fixture_proc(), child_poll=ScriptedPoll(0))
    try:
        sampler.sample()
    except ResourceError as error:
        assert error.operation == "meminfo_malformed", error
    else:
        raise AssertionError("malformed host reading accepted after child exit")
    print("PASS malformed host reading still fails closed after confirmed child exit")


def supervisor_fault_fails_closed_after_child_exit() -> None:
    # Given: a poll-confirmed exited child and a malformed supervisor reading.
    root = fixture_proc()
    (root / str(os.getpid()) / "status").write_text("Name:\tfixture\nVmRSS:\tnot-a-number kB\n")
    sampler = Sampler(4242, meminfo=meminfo(4 * GIB), proc=root, child_poll=ScriptedPoll(0))
    try:
        sampler.sample()
    except ResourceError as error:
        assert error.operation == "rss_malformed", error
    else:
        raise AssertionError("malformed supervisor reading accepted after child exit")
    print("PASS malformed supervisor reading still fails closed after confirmed child exit")


def drain_incomplete_still_rejected_under_lifecycle_sampling() -> None:
    # Given: a real child whose escaped descendant holds the pipes after its exit.
    with tempfile.TemporaryDirectory(dir="/tmp/opencode") as directory:
        pidfile = Path(directory) / "leak.pid"
        source = ("import subprocess, sys\n"
                  "g = subprocess.Popen([sys.executable, '-c', 'import os,time; os.setsid(); time.sleep(30)'])\n"
                  f"open({str(pidfile)!r}, 'w').write(str(g.pid))\n"
                  "print('partial', flush=True)\n")
        proc = subprocess.Popen(PY + (source,), stdin=subprocess.DEVNULL,
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        sampler = Sampler(proc.pid, meminfo=resource.MEMINFO, proc=resource.PROC_STATUS,
                          child_poll=proc.poll)
        try:
            result = Collector(proc, None, time.monotonic() + 10.0,
                               policy=CollectionPolicy(drain_grace=0.2), sample=sampler.sample).run()
        finally:
            if pidfile.exists():
                with suppress(ProcessLookupError):
                    os.killpg(int(pidfile.read_text()), signal.SIGKILL)
            if proc.poll() is None:
                proc.kill()
                proc.wait(timeout=5)
    # Then: the unfinished drain still rejects and the exited child never fakes a fault.
    assert result.invalidated and result.first_cause == "drain_incomplete", result
    assert result.samples >= 2, result
    extrema = sampler.extrema()
    assert extrema.min_mem_available_bytes is not None, extrema
    assert extrema.max_supervisor_rss_bytes is not None, extrema
    print("PASS escaped-descendant drain still rejects; exited child never fakes a resource fault")


def completed_child_accepted_with_fake_authority_and_real_pipes() -> None:
    # Given: a real completing guard whose inherited pipes outlive it briefly, fake authority.
    holder = ("import json, subprocess, sys, time\n"
              f"print(json.dumps({{'event': '{FINAL_EVENT}', 'data': 'ok', 'monotonic_ns': time.monotonic_ns()}}), flush=True)\n"
              "subprocess.Popen([sys.executable, '-c', 'import time; time.sleep(0.5)'])\n")
    with tempfile.TemporaryDirectory(dir="/tmp/opencode") as directory:
        root = Path(directory)
        setting, state = fake_setting(root)
        supervisor = Supervisor(setting, (sys.executable, "-B", "-c", holder))
        out = io.StringIO()
        with redirect_stdout(out):
            outcome = supervisor.run()
        # Then: the completed child is accepted; the exit race never invalidates it.
        assert outcome.accepted and outcome.guard_valid and outcome.restored_original, outcome
        assert state.read_text() == "65536"
        assert outcome.guard_exit == 0, outcome
        assert outcome.collection is not None and not outcome.collection["invalidated"], outcome.collection
        assert outcome.collection["first_cause"] is None, outcome.collection
        resources = outcome.resources
        assert resources is not None and resources["min_mem_available_bytes"] is not None, resources
        assert resources["max_supervisor_rss_bytes"] is not None, resources
        wires = [json.loads(line) for line in out.getvalue().splitlines()]
        emitted = [wire for wire in wires if wire["event"] == "ceiling_outcome"]
        assert len(emitted) == 1 and emitted[0]["data"]["accepted"] is True, out.getvalue()
    print("PASS completed child accepted under fake authority and real pipes despite the exit race")


def active_child_fault_invalidates_and_window_restores() -> None:
    # Given: a real hanging guard child whose live RSS readings fail typed.
    real_rss = resource.read_rss_bytes
    def missing_child_proc(pid: int, proc: Path = resource.PROC_STATUS) -> int:
        if pid != os.getpid():
            raise ResourceError("rss_unavailable", f"/proc/{pid}/status: simulated missing child proc")
        return real_rss(pid, proc)
    with tempfile.TemporaryDirectory(dir="/tmp/opencode") as directory:
        root = Path(directory)
        setting, state = fake_setting(root)
        supervisor = Supervisor(setting, guard_argv(root, "hang"))
        out = io.StringIO()
        with patch.object(resource, "read_rss_bytes", missing_child_proc), redirect_stdout(out):
            outcome = supervisor.run()
        # Then: the live fault still invalidates and the window restores the exact pair.
        assert outcome.accepted is False and outcome.guard_valid is False, outcome
        assert outcome.restored_original and outcome.restore_class == "restore_original", outcome
        assert outcome.collection is not None and "rss_unavailable" in (outcome.collection["first_cause"] or "")
        assert state.read_text() == "65536"
        guard = supervisor.guard
        assert guard is not None and guard.terminated, guard
        wires = [json.loads(line) for line in out.getvalue().splitlines()]
        emitted = [wire for wire in wires if wire["event"] == "ceiling_outcome"]
        assert len(emitted) == 1 and emitted[0]["data"]["accepted"] is False, out.getvalue()
    print("PASS active child RSS fault still invalidates and the window restores the exact pair")


def main() -> int:
    terminal_child_skips_rss_reading_before_sample()
    observed_max_survives_confirmed_exit()
    exit_between_liveness_and_reading_never_fakes_fault()
    live_malformed_child_reading_fails_closed()
    live_missing_child_reading_fails_closed()
    host_fault_fails_closed_after_child_exit()
    supervisor_fault_fails_closed_after_child_exit()
    drain_incomplete_still_rejected_under_lifecycle_sampling()
    completed_child_accepted_with_fake_authority_and_real_pipes()
    active_child_fault_invalidates_and_window_restores()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
