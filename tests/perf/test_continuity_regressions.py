#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# /// script
# requires-python = ">=3.12"
# dependencies = []
# ///
"""Run collector continuity scenarios with a virtual clock, no hardware or sleeps."""
from __future__ import annotations

import argparse
from dataclasses import asdict
import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools/perf"))
import aa_projection_collect as collector
import aa_perf_report as reporter
from aa_measurement_coverage import CoverageLedger, Sample


class Process:
    pid = os.getpid()

    def __init__(self, *args, **kwargs) -> None:
        self.returncode = None

    def poll(self) -> int | None:
        return self.returncode

    def terminate(self) -> None:
        self.returncode = 0

    def kill(self) -> None:
        self.returncode = -9

    def wait(self, timeout: float) -> None:
        if self.returncode is None:
            raise subprocess.TimeoutExpired("fixture", timeout)


def disconnected() -> None:
    # Given living receiver/display processes, sysfs disconnected, session initially connected.
    clock = [0.0]
    def sleep(seconds: float) -> None:
        clock[0] += seconds
    def query(*args, **kwargs) -> subprocess.CompletedProcess:
        state = "connected 1920x1080+0+0" if clock[0] < 2 else "disconnected"
        return subprocess.CompletedProcess(args[0], 0, "DP-0 " + state + "\n", "")
    with tempfile.TemporaryDirectory(prefix="perf-continuity-") as tmp:
        status = Path(tmp) / "status"
        status.write_text("disconnected\n")
        args = argparse.Namespace(receiver="fixture", display="fixture", port="fixture",
                                  no_display=False, duration_seconds=10, interval=1,
                                  store=None, video_socket=None, input_socket=None,
                                  state_dir=str(Path(tmp) / "state"))
        with (patch.object(collector.time, "monotonic_ns", side_effect=lambda: int(clock[0] * 1e9)),
              patch.object(collector.time, "sleep", side_effect=sleep),
              patch.object(collector, "_STARTUP_GRACE_SECONDS", 0),
              patch.object(collector.subprocess, "Popen", Process),
              patch.object(collector.subprocess, "run", side_effect=query),
              patch.object(collector.glob, "glob", return_value=[str(status)]),
              patch.object(collector, "_thermal_zones", return_value=()),
              patch.object(collector, "_target_hardware_state", return_value={})):
            # When xrandr loses the output without either process exiting.
            capture = collector.collect(args)
        # Then connection loss is latched and invalidates the run before the deadline.
        assert capture.get("display_connected") is False, capture
        assert capture["display_survived"] is True
        assert capture["observed_duration_seconds"] < 10
        doc = reporter.build_result(json.loads(json.dumps(capture)), duration_requested="10s", now="2026-10-15T00:00:00Z",
                                    capture_path=None, capture_sha256=None)
        assert doc["verdict"] != "PASS"
        assert "display_disconnected" in {v["metric"] for v in doc["violations"]}


def frozen() -> None:
    # Given a complete-looking live capture whose media counters freeze after startup.
    capture = json.loads((ROOT / "tests/perf/fixtures/wired_perf/nominal_capture.json").read_text())
    capture.update(measurement_origin="live", observed_duration_seconds=1800,
                   early_exit=False, receiver_exit_code=0, display_survived=True,
                   display_connected=True)
    capture["events"].update(startup_ms=10, video_units=1, audio_chunks=1)
    capture["provenance"].update(source_digest="a" * 64, build_artifact_digest="b" * 64,
                                 configuration_digest="c" * 64,
                                 service_definitions=[{"name": "fixture", "digest": "d" * 64}])
    capture["samples"] = [{"t_ns": second * 1_000_000_000, "receiver_rss_mb": 100,
                           "receiver_cpu_ticks": second * 10, "thermal_zones": [], "projection_active": True,
                           "video_units": 1, "audio_chunks": 1, "video_presentations": 1,
                           "audio_writes": 1, "av_pairs": 1} for second in range(0, 1801, 5)]
    capture.update(schema="aa-projection-perf-capture/2", monotonic_origin_ns=0, cpu_ticks_per_second=100,
                   video_evidence={"blocks": [], "ipc_latency_ns": [], "decode_latency_ns": []})
    ledger = CoverageLedger(0)
    for second in (5, 10, 15):
        ledger.advance(second * 1_000_000_000)
    capture["video_evidence"]["blocks"] = [asdict(block) for block in ledger.finish(15 * 1_000_000_000)]
    # When evaluating the actual time-series, not just nonzero startup evidence.
    doc = reporter.build_result(capture, duration_requested="30m", now="2026-10-15T00:00:00Z",
                                capture_path=None, capture_sha256=None)
    # Then living processes with stale media never qualify.
    assert doc["verdict"] != "PASS", doc["run_status"]


def active_window(freeze_after: float | None = None) -> None:
    # Given a receiver that first supplies complete consumer progress at t=4 seconds.
    clock = [0.0]
    logs = {}
    class LoggingProcess(Process):
        def __init__(self, command, *, stdout, **kwargs) -> None:
            super().__init__()
            logs[command[0]] = stdout
    def sleep(seconds: float) -> None:
        clock[0] += seconds
        if clock[0] < 4 or (freeze_after is not None and clock[0] > freeze_after):
            return
        stamp = int(clock[0] * 1e9)
        for event in ({"event": "active", "startup_ms": 4000},
                      {"event": "video_sent", "units": stamp},
                      {"event": "audio_chunks", "count": stamp},
                      {"event": "audio_write_completed", "scheduled_ns": stamp,
                       "scheduled_end_ns": stamp + 20_000_000, "completed_ns": stamp}):
            logs["receiver"].write(json.dumps(event) + "\n")
        logs["receiver"].flush()
        logs["display"].write(json.dumps({"event": "video_presented", "frame": stamp,
                                         "scheduled_ns": stamp, "completed_ns": stamp + 1000}) + "\n")
        logs["display"].flush()
    with tempfile.TemporaryDirectory(prefix="perf-active-") as tmp:
        args = argparse.Namespace(receiver="receiver", display="display", port="fixture",
                                  no_display=False, duration_seconds=10 if freeze_after is None else 30, interval=1,
                                  startup_timeout_seconds=6, store=None, video_socket=None,
                                  input_socket=None, state_dir=tmp)
        with (patch.object(collector.time, "monotonic_ns", side_effect=lambda: int(clock[0] * 1e9)),
              patch.object(collector.time, "sleep", side_effect=sleep),
              patch.object(collector, "_STARTUP_GRACE_SECONDS", 0),
              patch.object(collector.subprocess, "Popen", LoggingProcess),
              patch.object(collector, "_display_connected", return_value=True),
              patch.object(collector, "_thermal_zones", return_value=()),
              patch.object(collector, "_target_hardware_state", return_value={})):
            # When requesting ten seconds of active projection after bounded startup.
            capture = collector.collect(args)
        # Then startup adds time; it never shortens the active window.
        if freeze_after is not None:
            assert capture["projection_failure"].startswith("media_stalled:"), capture
            assert capture["observed_duration_seconds"] < 34
            return
        assert capture["active_projection_seconds"] == 10, capture
        assert capture["startup_wait_seconds"] == 4
        assert capture["observed_duration_seconds"] == 14
        assert capture["projection_failure"] is None

    # Given healthy counters across either contractual window, then full progress validates.
    for duration in (1800, 7200):
        progress = collector.ProjectionProgress()
        for second in range(0, duration + 1, 5):
            progress.observe(Sample(t_ns=second * 1_000_000_000, receiver_rss_mb=None,
                                    receiver_cpu_ticks=None, thermal_zones=(), projection_active=True,
                                    **{key: second + 1 for key in collector._PROGRESS_KEYS}))
        assert progress.seconds == duration and progress.failure is None
    for stream in collector._PROGRESS_KEYS:
        progress = collector.ProjectionProgress()
        for second in range(0, 21, 5):
            progress.observe(Sample(t_ns=second * 1_000_000_000, receiver_rss_mb=None,
                                    receiver_cpu_ticks=None, thermal_zones=(), projection_active=True,
                                    **{key: 1 if key == stream else second + 1
                                       for key in collector._PROGRESS_KEYS}))
        assert progress.failure == "media_stalled:" + stream, (stream, progress.failure)


def collector_frozen() -> None:
    active_window(freeze_after=5)


if __name__ == "__main__":
    globals()[sys.argv[1]]()
    print(f"PASS continuity-{sys.argv[1]}")
