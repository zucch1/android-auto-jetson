# SPDX-License-Identifier: GPL-3.0-or-later
# /// script
# requires-python = ">=3.11"
# dependencies = []
# ///
# Run: python3 -B tests/target_safety/diag_heartbeat.py
"""Heartbeat-in-wait qualification: wake bounds, worker progress, deadline neutrality.

Deterministic clock/selector scripting over real local children and real pipes
(the shared sizing_collector fixtures). The heartbeat wakes the collection wait
at the earliest of readiness, the next heartbeat or the deadline; its records
carry the worker progress counters and the last-progress age so unchanged
values expose worker inactivity. A heartbeat never extends a deadline and never
counts as progress.
"""
from __future__ import annotations

import io
import json
import os
import sys
import tempfile
import time
from contextlib import redirect_stdout
from pathlib import Path
from typing import IO
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
sys.path.insert(0, str(Path(__file__).resolve().parent))
from target_safety import deadline, egress
from target_safety.collector import CollectionPolicy, Collector, WorkerProgress
from target_safety.supervisor import Supervisor
from ceiling_setting_fixtures import fake_setting
from sizing_collector import FakeClock, FaultLabels, ScriptedSelector, spawn_child


class PipeProcView:
    """Real pipe read end as a still-running child's stdout; data timing is scripted."""

    def __init__(self, stdout: IO[bytes]) -> None:
        self.stdout: IO[bytes] | None = stdout
        self.stderr: IO[bytes] | None = None
        self.stdin: IO[bytes] | None = None

    def poll(self) -> int | None:
        return None


def heartbeat_wakes_the_wait_and_reveals_inactivity() -> None:
    # Given: an idle worker, a 30 s heartbeat and sample/drain intervals out of the way.
    clock = FakeClock(1.000)
    script = ScriptedSelector(clock, [], FaultLabels())
    beats: list[WorkerProgress] = []
    policy = CollectionPolicy(sample_interval=100.0, heartbeat_interval=30.0)
    proc = spawn_child("import time; time.sleep(30)", pipes=False)
    try:
        with patch("time.monotonic", clock), patch("selectors.DefaultSelector", lambda: script):
            result = Collector(proc, None, clock.now + 75.0, policy=policy,
                               heartbeat=beats.append).run()
    finally:
        proc.kill()
        proc.wait(timeout=5)
    # Then: the wait is the heartbeat remainder, then the deadline remainder.
    assert script.timeouts == [30.0, 30.0, 15.0], script.timeouts
    # Then: cadence holds and unchanged progress plus a growing age expose inactivity.
    assert len(beats) == 2, beats
    assert [beat.last_progress_age_seconds for beat in beats] == [30.0, 60.0], beats
    assert all((beat.records, beat.combined_bytes, beat.stdin_sent) == (0, 0, 0) for beat in beats)
    # Then: the collection deadline is never extended by heartbeats.
    assert result.timed_out and result.first_cause == "collection_deadline", result
    print("PASS heartbeat woke the wait at 30 s cadence; frozen progress exposed the idle worker")


def heartbeat_tracks_worker_progress_without_extending_deadline() -> None:
    # Given: one scripted early chunk on a real pipe, then a silent worker.
    reader, write_fd = os.pipe()
    os.write(write_fd, b"x")
    clock = FakeClock(1.000)
    script = ScriptedSelector(clock, [(10.0, ("out",))], FaultLabels())
    beats: list[WorkerProgress] = []
    policy = CollectionPolicy(sample_interval=100.0, heartbeat_interval=30.0)
    try:
        with patch("time.monotonic", clock), patch("selectors.DefaultSelector", lambda: script):
            result = Collector(PipeProcView(os.fdopen(reader, "rb")), None, clock.now + 75.0,
                               policy=policy, heartbeat=beats.append).run()
    finally:
        os.close(write_fd)
    # Then: the post-progress age is measured from the chunk, not from collection start.
    assert result.timed_out and result.first_cause == "collection_deadline", result
    assert len(beats) == 2 and beats[0].combined_bytes == 1, beats
    assert beats[0].last_progress_age_seconds == 21.0, beats[0]
    assert beats[1].last_progress_age_seconds == 51.0, beats[1]
    print("PASS heartbeat progress age tracks the last worker chunk (21 s then 51 s)")


def heartbeat_sink_absent_leaves_waits_untouched() -> None:
    # Given: the default pump with no heartbeat sink.
    clock = FakeClock(1.000)
    script = ScriptedSelector(clock, [], FaultLabels())
    proc = spawn_child("import time; time.sleep(30)", pipes=False)
    try:
        with patch("time.monotonic", clock), patch("selectors.DefaultSelector", lambda: script):
            result = Collector(proc, None, clock.now + 0.5).run()
    finally:
        proc.kill()
        proc.wait(timeout=5)
    # Then: the wait stays the plain sample interval and no heartbeat fires.
    assert script.timeouts[0] == 0.1, script.timeouts
    assert result.timed_out and result.samples == 0, result
    print("PASS no heartbeat sink means no extra wake bound and no heartbeat records")


def supervisor_writes_stay_inside_the_diagnostic_allowance() -> None:
    # Given: one real supervisor window (fake authority, real child) whose
    # heartbeats fire at a 0.2 s interval, with every write deadline captured
    # at the bounded deliver() seam.
    root = Path(tempfile.mkdtemp(prefix="diag-allowance-", dir="/tmp/opencode"))
    setting, _state = fake_setting(root)
    captured: list[tuple[float, float, str]] = []
    real_deliver = egress.deliver

    def instrumented(lines: list[str], write_deadline: float) -> bool:
        captured.append((time.monotonic(), write_deadline, json.loads(lines[0])["event"]))
        return real_deliver(lines, write_deadline)

    supervisor = Supervisor(setting, (sys.executable, "-B", "-c", "import time; time.sleep(0.6)"),
                            guard_stdin=b"", budget_seconds=60.0, cleanup_headroom_seconds=40.0)
    supervisor.guard_policy = CollectionPolicy(sample_interval=0.1, heartbeat_interval=0.2)
    with patch.object(egress, "deliver", instrumented), redirect_stdout(io.StringIO()):
        supervisor.run()
    # When: the window streams. Then: heartbeats fire and every synchronous
    # diagnostic write (stage brackets, transitions, results) is bounded by the
    # aggregate diagnostic allowance - never the 5 s per-write bound.
    events = [event for _, _, event in captured]
    assert events.count("heartbeat") >= 2, events
    allowance = deadline.DIAGNOSTIC_ALLOWANCE_SECONDS
    for started, write_deadline, event in captured:
        if event in ("guard_event", "guard_stderr", "ceiling_outcome", "sizing_outcome"):
            continue
        assert write_deadline <= started + allowance + 0.05, (event, write_deadline - started)
    # Then: the terminal keeps its 5 s egress bound instead of the allowance.
    terminals = [(started, write_deadline) for started, write_deadline, event in captured
                 if event == "ceiling_outcome"]
    assert terminals and terminals[0][1] <= terminals[0][0] + egress.WRITE_ATTEMPT_BOUND_SECONDS + 0.05
    assert terminals[0][1] > terminals[0][0] + allowance
    print("PASS supervisor sync writes and heartbeats bounded by DIAGNOSTIC_ALLOWANCE; terminal keeps 5 s")


def heartbeat_writes_stay_inside_the_collection_phase() -> None:
    # Given: one real supervisor window (tight collection deadline) whose
    # heartbeat sink writes are captured at the bounded deliver() seam, and the
    # sink itself invoked once more after the window (Oracle repro: a heartbeat
    # write deadline past the collection end).
    root = Path(tempfile.mkdtemp(prefix="diag-heartbeat-", dir="/tmp/opencode"))
    setting, _state = fake_setting(root)
    captured: list[tuple[float, float, dict]] = []
    real_deliver = egress.deliver

    def instrumented(lines: list[str], write_deadline: float) -> bool:
        captured.append((time.monotonic(), write_deadline, json.loads(lines[0])))
        return real_deliver(lines, write_deadline)

    supervisor = Supervisor(setting, (sys.executable, "-B", "-c", "import time; time.sleep(1.0)"),
                            guard_stdin=b"", budget_seconds=8.0, cleanup_headroom_seconds=6.0)
    supervisor.guard_policy = CollectionPolicy(sample_interval=0.1, heartbeat_interval=0.7)
    with patch.object(egress, "deliver", instrumented), redirect_stdout(io.StringIO()):
        supervisor.run()
        collection_deadline = supervisor.guard.deadline
        beats = [(at, write_deadline) for at, write_deadline, record in captured
                 if record["event"] == "heartbeat"]
        # When: the window streams. Then: every in-window heartbeat write is
        # capped at one sample interval (the 0.1 s cadence slips at most that
        # interval) and never runs past the collection phase it reports on.
        assert beats, [record["event"] for _, _, record in captured]
        for at, write_deadline in beats:
            assert write_deadline - at <= 0.1 + 0.05, write_deadline - at
            assert write_deadline <= collection_deadline + 0.05, write_deadline - collection_deadline
        # Then: the sink's write deadline is capped at the collection deadline
        # even when invoked after it - the until=guard.deadline wiring, never a
        # fresh per-write bound past the phase it reports on. The refused write
        # latches the channel, so it runs on a throwaway writer (test isolation).
        sink = supervisor.guard.heartbeat
        time.sleep(max(0.0, collection_deadline + 0.2 - time.monotonic()))
        before = time.monotonic()
        with patch.object(egress, "_WRITER", egress.Writer(time.monotonic() + 60.0)):
            sink(WorkerProgress(records=0, combined_bytes=0, stdin_sent=0,
                                last_progress_age_seconds=0.0))
        at, write_deadline, record = captured[-1]
        assert record["event"] == "heartbeat" and at >= before
        assert write_deadline <= collection_deadline + 1e-9, write_deadline - collection_deadline
    print("PASS heartbeat writes capped at one sample interval and at the collection deadline")


def heartbeat_overrun_rechecks_the_deadline_before_events() -> None:
    # Given: a real pipe with one pending readiness event and a scripted
    # heartbeat sink whose synchronous write overruns the collection deadline.
    reader, write_fd = os.pipe()
    os.write(write_fd, b"x")
    clock = FakeClock(1.000)
    script = ScriptedSelector(clock, [(100.0, ("out",))], FaultLabels())
    beats: list[WorkerProgress] = []
    policy = CollectionPolicy(sample_interval=100.0, heartbeat_interval=30.0)

    def overrunning(progress: WorkerProgress) -> None:
        beats.append(progress)
        clock.set(clock.now + 75.0)

    try:
        with patch("time.monotonic", clock), patch("selectors.DefaultSelector", lambda: script):
            result = Collector(PipeProcView(os.fdopen(reader, "rb")), None, clock.now + 75.0,
                               policy=policy, heartbeat=overrunning).run()
    finally:
        os.close(write_fd)
    # When: the callback returns past the deadline. Then: the collector
    # rechecks immediately, stops before the readiness event is processed and
    # never consults the wait again.
    assert result.timed_out and result.first_cause == "collection_deadline", result
    assert len(beats) == 1 and result.combined_bytes == 0, (beats, result.combined_bytes)
    assert script.timeouts == [30.0], script.timeouts
    print("PASS heartbeat overrun rechecked the deadline before any readiness event")


def phase_diagnostic_allowance_is_shared_and_cumulative() -> None:
    # Given: one real supervisor window whose three pre-restoration diagnostic
    # writes (collection-completed, guard_result, cleanup_started) each take
    # 0.3 s of bounded write time at the deliver() seam.
    root = Path(tempfile.mkdtemp(prefix="diag-shared-", dir="/tmp/opencode"))
    setting, _state = fake_setting(root)
    captured: list[tuple[float, float, dict]] = []
    real_deliver = egress.deliver
    trio_events = ("guard_result", "cleanup_started")

    def slow_trio(lines: list[str], write_deadline: float) -> bool:
        record = json.loads(lines[0])
        is_trio = (record["event"] in trio_events
                   or (record["event"] == "stage_completed"
                       and record.get("data") == {"phase": "collection"}))
        captured.append((time.monotonic(), write_deadline, record))
        if is_trio:
            time.sleep(0.3)
        return real_deliver(lines, write_deadline)

    supervisor = Supervisor(setting, (sys.executable, "-B", "-c", "import time; time.sleep(0.4)"),
                            guard_stdin=b"", budget_seconds=60.0, cleanup_headroom_seconds=40.0)
    with patch.object(egress, "deliver", slow_trio), redirect_stdout(io.StringIO()):
        supervisor.run()
    trio = [(at, write_deadline) for at, write_deadline, record in captured
            if record["event"] in trio_events
            or (record["event"] == "stage_completed"
                and record.get("data") == {"phase": "collection"})]
    # When: the writes stream. Then: the three draws share ONE phase allowance -
    # every write deadline lands inside DIAGNOSTIC_ALLOWANCE_SECONDS counted from
    # the first draw, instead of a fresh second per write.
    assert len(trio) == 3, [(record["event"], at) for at, _, record in captured]
    allowance = deadline.DIAGNOSTIC_ALLOWANCE_SECONDS
    first = trio[0][0]
    for at, write_deadline in trio:
        assert write_deadline <= first + allowance + 0.05, (at, write_deadline - first)
    print("PASS collection-completed + guard_result + cleanup_started share one "
          "cumulative DIAGNOSTIC_ALLOWANCE budget")


def main() -> int:
    heartbeat_wakes_the_wait_and_reveals_inactivity()
    heartbeat_tracks_worker_progress_without_extending_deadline()
    heartbeat_sink_absent_leaves_waits_untouched()
    supervisor_writes_stay_inside_the_diagnostic_allowance()
    heartbeat_writes_stay_inside_the_collection_phase()
    heartbeat_overrun_rechecks_the_deadline_before_events()
    phase_diagnostic_allowance_is_shared_and_cumulative()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
