#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# /// script
# requires-python = ">=3.12"
# dependencies = []
# ///
"""Collector startup-repair regressions: bounded receiver-ready observation,
managed-display launch, endpoint overrides and the frozen shutdown cutoff.
Deterministic virtual clock and fake processes; no hardware, no wall-clock sleeps."""
from __future__ import annotations

import argparse
import builtins
import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools/perf"))
import aa_perf_report as reporter
import aa_projection_collect as collector

SECOND = 1_000_000_000
FAKE_RECEIVER = ROOT / "tests/perf/fixtures/wired_perf/fake_receiver.py"
READY = {"event": "ready", "state": "negotiating"}

clock = [0]
spawn: list["Process"] = []


class Process:
    """Virtual-clock Popen stand-in recording command and launch instant."""

    pid = os.getpid()

    def __init__(self, command=(), *, stdout=None, **kwargs) -> None:
        self.command = list(command)
        self.stdout = stdout
        self.returncode = None
        self.launched_ns = clock[0]
        spawn.append(self)

    def poll(self) -> int | None:
        return self.returncode

    def terminate(self) -> None:
        self.returncode = 0

    def kill(self) -> None:
        self.returncode = -9

    def wait(self, timeout: float) -> None:
        if self.returncode is None:
            raise subprocess.TimeoutExpired("fixture", timeout)


def reset() -> None:
    clock[0] = 0
    spawn.clear()


def result(capture: dict, requested: str) -> dict:
    return reporter.build_result(json.loads(json.dumps(capture)), duration_requested=requested,
                                 now="2026-10-18T00:00:00Z", capture_path=None,
                                 capture_sha256=None)


def args_ns(**overrides) -> argparse.Namespace:
    base = dict(receiver="receiver", display="display", port="fixture", no_display=False,
                duration_seconds=10, interval=1, startup_timeout_seconds=6, store=None,
                video_socket=None, input_socket=None, state_dir=".")
    base.update(overrides)
    return argparse.Namespace(**base)


def run_collect(args: argparse.Namespace,
                events: dict[int, list[tuple[str, str]]],
                process: type = Process) -> dict:
    """Drive collect() on the virtual clock; `events` maps clock nanoseconds to log lines."""
    written = {stamp: False for stamp in events}

    def sleep(seconds: float) -> None:
        clock[0] += int(seconds * SECOND)
        for stamp, lines in sorted(events.items()):
            if clock[0] >= stamp and not written[stamp]:
                written[stamp] = True
                for name, line in lines:
                    handle = spawn[0].stdout if name == "receiver" else spawn[1].stdout
                    handle.write(line + "\n")
                    handle.flush()

    with tempfile.TemporaryDirectory(prefix="perf-startup-") as tmp:
        args.state_dir = str(Path(tmp) / "state")
        with (patch.object(collector.time, "monotonic_ns", side_effect=lambda: clock[0]),
              patch.object(collector.time, "sleep", side_effect=sleep),
              patch.object(collector.subprocess, "Popen", process),
              patch.object(collector, "_display_connected", return_value=True),
              patch.object(collector, "_thermal_zones", return_value=()),
              patch.object(collector, "_target_hardware_state", return_value={})):
            return collector.collect(args)


def ready_launch() -> None:
    # Given a managed display and a receiver whose ready event lands at t=5.75s.
    reset()
    events = {5_750_000_000: [("receiver", json.dumps(READY))]}
    # When the collector observes readiness instead of a fixed startup sleep.
    capture = run_collect(args_ns(interval=0.25, startup_timeout_seconds=6), events)
    # Then the display launches on the ready event, and the deadline stays the
    # original absolute one measured from the pre-launch origin.
    assert len(spawn) == 2, [p.command for p in spawn]
    assert spawn[1].launched_ns == 5_750_000_000, spawn[1].launched_ns
    assert capture["observed_duration_seconds"] == 6.0, capture["observed_duration_seconds"]
    assert capture["display_survived"] is True
    assert capture["early_exit"] is False


def receiver_dies() -> None:
    # Given a managed display and a receiver that dies at t=2s before any ready event.
    reset()

    def sleep(seconds: float) -> None:
        clock[0] += int(seconds * SECOND)
        if clock[0] >= 2 * SECOND:
            spawn[0].returncode = 3

    with tempfile.TemporaryDirectory(prefix="perf-dies-") as tmp:
        args = args_ns(interval=0.5, startup_timeout_seconds=6, state_dir=str(Path(tmp) / "state"))
        with (patch.object(collector.time, "monotonic_ns", side_effect=lambda: clock[0]),
              patch.object(collector.time, "sleep", side_effect=sleep),
              patch.object(collector.subprocess, "Popen", Process),
              patch.object(collector, "_display_connected", return_value=True),
              patch.object(collector, "_thermal_zones", return_value=()),
              patch.object(collector, "_target_hardware_state", return_value={})):
            # When the receiver dies while the collector is waiting for readiness.
            capture = collector.collect(args)
    # Then the collector exits early without a hang and never launches the display.
    assert capture["early_exit"] is True, capture
    assert capture["crashes"] == 1
    assert capture["observed_duration_seconds"] == 2.0, capture["observed_duration_seconds"]
    assert len(spawn) == 1, [p.command for p in spawn]
    assert capture["display_survived"] is None


def readiness_timeout() -> None:
    # Given a managed display and a receiver that never emits ready.
    reset()
    capture = run_collect(args_ns(interval=1, startup_timeout_seconds=6), {})
    # Then the absolute startup deadline ends the run blocked without the display.
    assert_startup_blocked(capture, 6.0)
    assert capture["early_exit"] is False
    doc = result(capture, "10s")
    assert doc["run_status"]["display_survived"] is None
    assert "run_evidence:display_survived" in doc["blocked_on"], doc["blocked_on"]


def assert_startup_blocked(capture: dict, observed: float) -> None:
    assert [p.launched_ns for p in spawn] == [0], [p.launched_ns for p in spawn]
    assert capture["display_survived"] is None and capture["display_connected"] is None
    assert capture["active_projection_seconds"] == 0.0 and capture["startup_wait_seconds"] is None
    assert capture["observed_duration_seconds"] == observed
    assert capture["receiver_stopped_by_collector"] is True and spawn[0].returncode == 0
    assert clock[0] == int(observed * SECOND)
    assert result(capture, "10s")["verdict"] == "BLOCKED"


def ready_at_deadline() -> None:
    # Given readiness exactly at the original 6s deadline.
    reset()
    # When collecting, then only the receiver was launched and cleanup is bounded.
    capture = run_collect(args_ns(), {6 * SECOND: [("receiver", json.dumps(READY))]})
    assert_startup_blocked(capture, 6.0)


def ready_after_deadline() -> None:
    # Given Popen returns at 7s with readiness already in the receiver log.
    reset()

    class DelayedProcess(Process):
        def __init__(self, *args, **kwargs) -> None:
            super().__init__(*args, **kwargs)
            if len(spawn) == 1:
                clock[0] = 7 * SECOND
                self.stdout.write(json.dumps(READY) + "\n")
                self.stdout.flush()

    # When collecting, then the original 6s deadline still forbids display launch.
    capture = run_collect(args_ns(), {}, DelayedProcess)
    assert_startup_blocked(capture, 7.0)


def deadline_before_launch() -> None:
    # Given ready at 5.75s, but opening the display log reaches the 6s deadline.
    reset()
    real_open = builtins.open

    def slow_display_open(path, *args, **kwargs):
        if Path(path).name == "display.log" and args and args[0] == "w":
            clock[0] = 6 * SECOND
        return real_open(path, *args, **kwargs)

    events = {5_750_000_000: [("receiver", json.dumps(READY))]}
    # When preparing launch, then a fresh time check must still block Popen.
    with patch.object(builtins, "open", side_effect=slow_display_open):
        capture = run_collect(args_ns(interval=0.25), events)
    assert_startup_blocked(capture, 6.0)


def no_display_default() -> None:
    # Given no managed display: --no-display, and no --display at all.
    for display, no_display in (("display", True), (None, False)):
        reset()
        capture = run_collect(args_ns(display=display, no_display=no_display,
                                      interval=1, startup_timeout_seconds=4), {})
        # Then the receiver runs unchanged without the consumer gate and without
        # any readiness wait or display launch.
        assert len(spawn) == 1, [p.command for p in spawn]
        assert "--require-video-consumer" not in spawn[0].command, spawn[0].command
        assert capture["observed_duration_seconds"] == 4.0, capture["observed_duration_seconds"]
        assert capture["display_survived"] is None
        assert capture["receiver_stopped_by_collector"] is True


def endpoint_overrides() -> None:
    # Given a managed display with socket overrides on the collector CLI.
    reset()
    events = {250_000_000: [("receiver", json.dumps(READY))]}
    capture = run_collect(args_ns(video_socket="v.sock", input_socket="i.sock",
                                  interval=0.25, startup_timeout_seconds=4), events)
    # Then both the receiver and the display get both socket overrides, and only
    # the managed case requests the consumer gate.
    receiver_cmd, display_cmd = spawn[0].command, spawn[1].command
    assert "--require-video-consumer" in receiver_cmd, receiver_cmd
    assert "--require-video-consumer" not in display_cmd, display_cmd
    for cmd in (receiver_cmd, display_cmd):
        assert cmd[cmd.index("--video-socket") + 1] == "v.sock", cmd
        assert cmd[cmd.index("--input-socket") + 1] == "i.sock", cmd
    assert display_cmd[1] == "--test-only-autostart", display_cmd
    assert capture["display_survived"] is True
    # Given no overrides, neither process receives socket arguments.
    reset()
    plain = run_collect(args_ns(interval=0.25, startup_timeout_seconds=4), events)
    for proc in spawn:
        assert "--video-socket" not in proc.command and "--input-socket" not in proc.command, proc.command
    assert plain["display_survived"] is True
    # The fake receiver fixture accepts the managed flag without extra behaviour.
    proc = subprocess.run([sys.executable, "-B", str(FAKE_RECEIVER), "--port", "fixture",
                           "--duration-ms", "100", "--require-video-consumer",
                           "--video-socket", "v.sock", "--input-socket", "i.sock"],
                          capture_output=True, text=True, timeout=15)
    assert proc.returncode == 0, proc.stderr
    assert '"event": "ready"' in proc.stdout, proc.stdout


def cutoff_no_heal() -> None:
    # Given an active window whose absolute cutoff is t=60s and whose first IPC
    # pair appears only at t=63s, in the post-termination drain.
    reset()
    ipc_lines = (
        json.dumps({"event": "video_ipc_send", "producer_pid": 7, "consumer_pid": 8,
                    "frame": 1, "sequence": 1, "timestamp_ns": 63_000_000_000,
                    "connection_ns": 62_000_000_000, "observed_ns": 63_000_000_000,
                    "payload_bytes": 1024}),
        json.dumps({"event": "video_ipc_receive", "producer_pid": 7, "consumer_pid": 8,
                    "frame": 1, "sequence": 1, "timestamp_ns": 63_000_000_000,
                    "connection_ns": 62_000_000_000, "observed_ns": 63_200_000_000,
                    "payload_bytes": 1024}),
    )

    class DrainingProcess(Process):
        def terminate(self) -> None:
            for line in ipc_lines:
                self.stdout.write(line + "\n")
            self.stdout.flush()
            super().terminate()

    def progress(second: int) -> list[tuple[str, str]]:
        stamp = second * SECOND
        return [("receiver", json.dumps({"event": "active", "startup_ms": 5000})),
                ("receiver", json.dumps({"event": "video_sent", "units": stamp})),
                ("receiver", json.dumps({"event": "audio_chunks", "count": stamp})),
                ("receiver", json.dumps({"event": "audio_write_completed", "scheduled_ns": stamp,
                                         "scheduled_end_ns": stamp + 20_000_000,
                                         "completed_ns": stamp})),
                ("receiver", json.dumps({"event": "video_presented", "frame": stamp,
                                         "scheduled_ns": stamp, "completed_ns": stamp + 1000}))]

    events = {second * SECOND: progress(second) for second in range(5, 61)}
    args = args_ns(display=None, no_display=True, duration_seconds=55, interval=1,
                   startup_timeout_seconds=60)
    capture = run_collect(args, events, DrainingProcess)
    # Then the sealed window keeps its progress coverage and the late first IPC
    # pair cannot heal any of it.
    assert capture["active_projection_seconds"] == 55, capture["active_projection_seconds"]
    assert capture["startup_wait_seconds"] == 5
    assert capture["observed_duration_seconds"] == 60.0
    assert capture["projection_failure"] is None
    blocks = capture["video_evidence"]["blocks"]
    assert blocks and all(block["ipc_consumption"]["count"] == 0
                          and block["ipc_matches"]["count"] == 0 for block in blocks), blocks
    telemetry = capture["video_telemetry"]
    assert telemetry["ipc_matched"] == 0 and telemetry["ipc_pending_send"] == 0
    assert telemetry["ipc_pending_receive"] == 0 and telemetry["ipc_unmatched_or_expired"] == 0
    assert "ipc_throughput_mbps" not in capture["events"] and "ipc_p95_added_latency_ms" not in capture["events"]
    doc = result(capture, "55s")
    assert doc["verdict"] != "PASS"
    for metric in ("ipc_throughput_mbps", "ipc_p95_added_latency_ms"):
        assert doc["metrics"][metric]["value"] is None, (metric, doc["metrics"][metric])

    # Given no active progress at all (the observed run shape) the same late IPC
    # pair must not fabricate or heal a window before the t=60s deadline either.
    reset()
    bare = run_collect(args_ns(display=None, no_display=True, duration_seconds=1, interval=5,
                               startup_timeout_seconds=60), {}, DrainingProcess)
    assert bare["active_projection_seconds"] == 0.0
    assert bare["startup_wait_seconds"] is None
    assert not bare["video_evidence"]["blocks"]
    assert bare["video_telemetry"]["ipc_matched"] == 0
    assert "ipc_throughput_mbps" not in bare["events"]
    doc = result(bare, "60s")
    assert doc["verdict"] == "BLOCKED", doc["verdict"]
    assert doc["metrics"]["ipc_throughput_mbps"]["value"] is None
    assert "run_evidence:active_projection_progress" in doc["blocked_on"], doc["blocked_on"]


CASES = {"ready_launch": ready_launch, "receiver_dies": receiver_dies,
         "ready_at_deadline": ready_at_deadline, "ready_after_deadline": ready_after_deadline,
         "deadline_before_launch": deadline_before_launch,
         "readiness_timeout": readiness_timeout, "no_display_default": no_display_default,
         "endpoint_overrides": endpoint_overrides, "cutoff_no_heal": cutoff_no_heal}

if __name__ == "__main__":
    names = [sys.argv[1]] if len(sys.argv) > 1 else list(CASES)
    for name in names:
        CASES[name]()
    print(f"PASS {len(names)} startup repair tests")
