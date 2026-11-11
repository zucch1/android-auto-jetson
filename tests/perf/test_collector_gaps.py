#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# /// script
# requires-python = ">=3.12"
# dependencies = []
# ///
"""Collector sampling-gap regressions on a virtual clock; no hardware or sleeps."""
from __future__ import annotations

import argparse
import json
import os
import random
import subprocess
import sys
import tempfile
from pathlib import Path
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools/perf"))
import aa_projection_collect as collector
import aa_perf_report as reporter
from aa_measurement_coverage import Sample
from test_v2_coverage import synthetic

NS = 1_000_000_000


class Process:
    pid = os.getpid()

    def __init__(self, command=(), *, stdout=None, **kwargs) -> None:
        self.command = list(command)
        self.returncode = None
        if command and command[0] == "receiver":
            stdout.write(json.dumps({"event": "ready", "state": "negotiating"}) + "\n")
            stdout.flush()

    def poll(self) -> int | None:
        return self.returncode

    def terminate(self) -> None:
        self.returncode = 0

    def kill(self) -> None:
        self.returncode = -9

    def wait(self, timeout: float) -> None:
        if self.returncode is None:
            raise subprocess.TimeoutExpired("fixture", timeout)


def progress_sample(second: int, av_pairs: int) -> Sample:
    return Sample(t_ns=second * NS, receiver_rss_mb=None, receiver_cpu_ticks=None,
                  thermal_zones=(), projection_active=True,
                  video_units=second + 1, audio_chunks=second + 1,
                  video_presentations=second + 1, audio_writes=second + 1,
                  av_pairs=av_pairs)


def run_gap(*, jumps: tuple[tuple[int, int], ...] = ((5, 30),), mode: str = "healthy",
            duration: int = 10, stop_after: int | None = None) -> dict:
    """Freeze collector sleeps while the receiver logs on its own clock.

    ``catchup`` holds the frozen window's lines in a userspace buffer until 0.5 s
    after the first post-gap sample. ``silence`` logs only the first and last
    seconds of that window. ``never`` stops logging at the gap.
    """
    clock = [0]
    logs: dict[str, object] = {}
    jump_index = [0]
    last_second = [0]
    backlog: list[tuple[str, dict]] = []
    gap_start, gap_seconds = jumps[0]
    gap_end = gap_start + gap_seconds
    catchup_ns = (gap_end + 0.5) * NS
    if mode == "never":
        stop_after = gap_start

    class LoggingProcess(Process):
        def __init__(self, command=(), *, stdout=None, **kwargs) -> None:
            super().__init__(command, stdout=stdout, **kwargs)
            logs[command[0]] = stdout

    def write(name: str, event: dict) -> None:
        logs[name].write(json.dumps(event) + "\n")
        logs[name].flush()

    def emit(second: int) -> None:
        if second < 1 or (stop_after is not None and second > stop_after):
            return
        if mode == "silence" and gap_start + 1 < second < gap_end:
            return
        stamp = second * NS
        events = (
            ("receiver", {"event": "active", "startup_ms": 1000}),
            ("receiver", {"event": "video_sent", "units": stamp}),
            ("receiver", {"event": "audio_chunks", "count": stamp}),
            ("receiver", {"event": "audio_write_completed", "scheduled_ns": stamp,
                           "scheduled_end_ns": stamp + 20_000_000, "completed_ns": stamp}),
            ("display", {"event": "video_presented", "frame": stamp,
                          "scheduled_ns": stamp, "completed_ns": stamp + 1000}),
        )
        if mode == "catchup" and gap_start < second <= gap_end:
            backlog.extend(events)
        else:
            for name, event in events:
                write(name, event)

    def advance(target: int) -> None:
        clock[0] = target
        for second in range(last_second[0] + 1, target // NS + 1):
            emit(second)
        last_second[0] = max(last_second[0], target // NS)
        if backlog and target >= catchup_ns:
            for name, event in backlog:
                write(name, event)
            backlog.clear()

    def sleep(seconds: float) -> None:
        if (jump_index[0] < len(jumps)
                and clock[0] >= jumps[jump_index[0]][0] * NS):
            _, seconds_gap = jumps[jump_index[0]]
            jump_index[0] += 1
            advance(clock[0] + seconds_gap * NS)
        else:
            advance(clock[0] + round(seconds * NS))

    with tempfile.TemporaryDirectory(prefix="perf-collector-gap-") as tmp:
        args = argparse.Namespace(receiver="receiver", display="display", port="fixture",
                                  no_display=False, duration_seconds=duration, interval=1,
                                  startup_timeout_seconds=6, store=None, video_socket=None,
                                  input_socket=None, state_dir=tmp)
        with (patch.object(collector.time, "monotonic_ns", side_effect=lambda: clock[0]),
              patch.object(collector.time, "sleep", side_effect=sleep),
              patch.object(collector.subprocess, "Popen", LoggingProcess),
              patch.object(collector, "_display_connected", return_value=True),
              patch.object(collector, "_thermal_zones", return_value=()),
              patch.object(collector, "_target_hardware_state", return_value={})):
            return collector.collect(args)


def report(capture: dict) -> dict:
    return reporter.build_result(json.loads(json.dumps(capture)), duration_requested=None,
                                 now="2026-11-11T00:00:00Z", capture_path=None,
                                 capture_sha256=None)


class MainProgress:
    """origin/main's ProjectionProgress.observe, verbatim, as the no-gap oracle."""

    def __init__(self) -> None:
        self.started_ns = None
        self.last_ns = 0
        self.previous = {}
        self.advanced_ns = {}
        self.failure = None

    def observe(self, sample: Sample) -> None:
        if self.failure:
            return
        stamp = sample.t_ns
        counters = {key: value for key, value in zip(collector._PROGRESS_KEYS, (
            sample.video_units, sample.audio_chunks, sample.video_presentations,
            sample.audio_writes, sample.av_pairs)) if value is not None}
        valid = (len(counters) == len(collector._PROGRESS_KEYS) and type(stamp) is int
                 and stamp >= 0 and all(type(value) is int and value >= 0
                                        for value in counters.values()))
        if not valid:
            if self.started_ns is not None:
                self.failure = "missing_progress"
            return
        if self.started_ns is None:
            if not sample.projection_active or not all(counters.values()):
                return
            self.started_ns = stamp
            self.previous = counters
            self.advanced_ns = {key: stamp for key in counters}
        else:
            if stamp <= self.last_ns or stamp - self.last_ns > 10 * NS:
                self.failure = "sampling_gap"
            if not sample.projection_active:
                self.failure = "projection_inactive"
            for key, value in counters.items():
                if value < self.previous[key]:
                    self.failure = "counter_reset:" + key
                # Check age before accepting a resumed stream: a late recovery cannot heal a freeze.
                if stamp - self.advanced_ns[key] > 10 * NS:
                    self.failure = "media_stalled:" + key
                if value > self.previous[key]:
                    self.advanced_ns[key] = stamp
            self.previous = counters
        self.last_ns = stamp


def state(progress) -> tuple:
    return (progress.started_ns, progress.last_ns, progress.failure,
            dict(progress.previous), dict(progress.advanced_ns))


def no_gap_late_recovery() -> None:
    # av_pairs last advances at 5 s, is silent for 13 s, then resumes. Main reports
    # the stall before accepting the resume; so must this.
    progress = collector.ProjectionProgress()
    for second, pairs in ((0, 1), (5, 2), (10, 2), (14, 2), (18, 3)):
        progress.observe(progress_sample(second, pairs))
    assert progress.failure == "media_stalled:av_pairs", progress.failure
    assert progress.interferences == []


def no_gap_matches_main() -> None:
    # Differential: for any sequence with no sampling gap over 10 s, behaviour (the
    # verdict and every piece of state) is identical to origin/main, whatever
    # receiver evidence is passed alongside.
    rng = random.Random(90)
    junk = {key: {"event_count": 99, "first_event_ns": 1, "last_event_ns": 2,
                  "max_silence_ns": 0} for key in collector._PROGRESS_KEYS}
    for _ in range(3000):
        mine, oracle = collector.ProjectionProgress(), MainProgress()
        stamp = 0
        pairs = {key: 0 for key in collector._PROGRESS_KEYS}
        for _ in range(rng.randrange(2, 14)):
            stamp += rng.choice((0, 1, 2, 4900, 5000, 5100, 6000, 7500, 9000, 10000)) * 1_000_000
            for key in pairs:
                if rng.random() < 0.55:
                    pairs[key] += rng.choice((1, 1, 2))
                elif rng.random() < 0.02:
                    pairs[key] -= 1
            sample = Sample(t_ns=stamp, receiver_rss_mb=None, receiver_cpu_ticks=None,
                            thermal_zones=(), projection_active=rng.random() > 0.03, **pairs)
            oracle.observe(sample)
            mine.observe(sample, junk if rng.random() < 0.5 else None)
            assert state(mine) == state(oracle), (state(mine), state(oracle), stamp)


def evidence(first: float, last: float, silence: float, count: int = 20) -> dict:
    return {"event_count": count, "first_event_ns": int(first * NS),
            "last_event_ns": int(last * NS), "max_silence_ns": int(silence * NS)}


def gap_decision() -> None:
    def gap(counters_after: int, logged: dict | None) -> collector.ProjectionProgress:
        progress = collector.ProjectionProgress()
        progress.observe(progress_sample(0, 1))
        progress.observe(progress_sample(5, 2))
        progress.observe(progress_sample(39, counters_after), logged)
        return progress

    every = lambda item: {key: item for key in collector._PROGRESS_KEYS}
    # Without receiver evidence a long gap keeps main's verdict, even though every
    # counter advanced by one: one increment cannot place progress inside the gap.
    oracle = MainProgress()
    for second, pairs in ((0, 1), (5, 2), (39, 3)):
        oracle.observe(progress_sample(second, pairs))
    assert oracle.failure is not None and gap(3, None).failure == oracle.failure
    assert gap(3, every(evidence(0, 0, 34, count=0))).failure.startswith("media_stalled:")
    # Receiver-clock events with no silence over 10 s through the window: continue.
    good = gap(900, every(evidence(6, 39, 3)))
    assert good.failure is None and len(good.interferences) == 1
    assert good.interferences[0]["gap_seconds"] == 34
    assert good.interferences[0]["projection_failure"] is None
    # The same counters with a 20 s logged silence inside the window: a stall.
    silent = gap(900, every(evidence(6, 39, 20, count=4)))
    assert silent.failure.startswith("media_stalled:"), silent.failure
    assert silent.interferences[0]["projection_failure"] == silent.failure
    # A trailing silence (last event long before the window ends) is also a stall.
    assert gap(900, every(evidence(6, 20, 19))).failure.startswith("media_stalled:")
    # No events at all in the window, even with the counter advanced by one and
    # evidence that claims no silence.
    assert gap(3, every(evidence(0, 0, 34, count=0))).failure.startswith("media_stalled:")
    assert gap(3, every(evidence(0, 0, 0, count=0))).failure.startswith("media_stalled:")
    # Logged events without the counters having moved are not progress either.
    assert gap(2, every(evidence(6, 39, 3))).failure.startswith("media_stalled:")
    # One stream silent is enough, and it names that stream.
    uneven = every(evidence(6, 39, 3))
    uneven["audio_writes"] = evidence(6, 12, 27)
    assert gap(900, uneven).failure == "media_stalled:audio_writes"


def gap_progress() -> None:
    capture = run_gap()
    assert capture["projection_failure"] is None, capture
    assert capture["observed_duration_seconds"] == 35, capture["observed_duration_seconds"]
    assert len(capture["interference"]) == 1
    gap = capture["interference"][0]
    assert gap["classification"] == "host_interference" and gap["gap_seconds"] == 30
    assert gap["projection_failure"] is None
    for stream in collector._PROGRESS_KEYS:
        logged = gap["logged_progress"][stream]
        assert logged["event_count"] >= 29, (stream, logged)
        assert logged["max_silence_ns"] <= collector._MEDIA_STALL_SECONDS * NS
    doc = report(capture)
    assert doc["run_status"]["projection_failure"] is None, doc["run_status"]
    assert doc["run_status"]["interference"] == capture["interference"]
    assert "coverage:sampling" in doc["named_gaps"], doc["named_gaps"]
    assert doc["verdict"] != "PASS"
    assert any("host interference" in note for note in doc["notes"])


def gap_logged_silence() -> None:
    capture = run_gap(mode="silence")
    assert capture["projection_failure"].startswith("media_stalled:"), capture
    gap = capture["interference"][0]
    assert gap["projection_failure"] == capture["projection_failure"]
    assert max(item["max_silence_ns"] for item in gap["logged_progress"].values()) > 10 * NS
    doc = report(capture)
    assert doc["run_status"]["projection_failure"] == capture["projection_failure"]
    assert doc["verdict"] == "FAIL"


def gap_log_catches_up() -> None:
    capture = run_gap(mode="catchup")
    assert capture["projection_failure"] is None, capture
    assert len(capture["interference"]) == 1
    # The grace ended as soon as the delayed lines arrived, well before its bound.
    assert capture["interference"][0]["gap_seconds"] < 30 + collector._COLLECTOR_LOG_GRACE_SECONDS
    assert report(capture)["run_status"]["projection_failure"] is None


def gap_log_never_catches_up() -> None:
    capture = run_gap(mode="never")
    assert capture["projection_failure"].startswith("media_stalled:"), capture
    gap = capture["interference"][0]
    # Reported after the bounded grace: not at the first post-gap read, not later.
    assert 0 < collector._COLLECTOR_LOG_GRACE_SECONDS <= collector._MEDIA_STALL_SECONDS / 2
    assert (collector._COLLECTOR_LOG_GRACE_SECONDS
            <= gap["gap_seconds"] - 30 <= collector._COLLECTOR_LOG_GRACE_SECONDS + 1), gap
    assert report(capture)["run_status"]["projection_failure"] == capture["projection_failure"]


def stall_after_gap() -> None:
    # The gap is healthy; the receiver then freezes for real and is still reported.
    capture = run_gap(duration=60, stop_after=45)
    assert capture["projection_failure"].startswith("media_stalled:"), capture
    assert len(capture["interference"]) == 1
    assert capture["interference"][0]["projection_failure"] is None
    assert 45 + 10 < capture["observed_duration_seconds"] <= 45 + 16


def repeated_short_gaps() -> None:
    # Gaps over the interference threshold but under the stall threshold are all
    # recorded and need no log evidence; the ordinary age check still runs.
    capture = run_gap(jumps=((5, 8), (17, 8), (29, 9)), duration=40)
    assert capture["projection_failure"] is None, capture
    assert [gap["gap_seconds"] for gap in capture["interference"]] == [8, 8, 9]
    assert all("logged_progress" not in gap for gap in capture["interference"])
    doc = report(capture)
    assert len(doc["run_status"]["interference"]) == 3
    # Gaps at or under the threshold are ordinary sampling, not interference.
    quiet = run_gap(jumps=((5, 6),), duration=20)
    assert quiet["interference"] == [] and quiet["projection_failure"] is None
    assert "interference" not in report(quiet)["run_status"]


def repeated_long_gaps() -> None:
    capture = run_gap(jumps=((5, 20), (40, 25)), duration=70)
    assert capture["projection_failure"] is None, capture
    assert len(capture["interference"]) == 2
    assert all("logged_progress" in gap for gap in capture["interference"])


def old_capture_replay() -> None:
    # A capture written before this change has no interference or evidence: its long
    # gap replays with main's verdict, and the result stays schema-valid.
    capture = run_gap()
    capture.pop("interference")
    doc = report(capture)
    assert doc["run_status"]["projection_failure"] is not None, doc["run_status"]
    assert doc["verdict"] == "FAIL"
    assert doc["run_status"]["interference"][0]["gap_seconds"] == 30
    clean = run_gap(jumps=((5, 3),), duration=10)
    clean.pop("interference")
    assert report(clean)["run_status"]["projection_failure"] is None


CASES = {name: globals()[name] for name in (
    "no_gap_late_recovery", "no_gap_matches_main", "gap_decision", "gap_progress",
    "gap_logged_silence", "gap_log_catches_up", "gap_log_never_catches_up",
    "stall_after_gap", "repeated_short_gaps", "repeated_long_gaps", "old_capture_replay")}


if __name__ == "__main__":
    for name in sys.argv[1:] or list(CASES):
        CASES[name]()
        print("PASS collector-gap-" + name)
