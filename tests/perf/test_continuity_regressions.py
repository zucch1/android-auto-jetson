#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# /// script
# requires-python = ">=3.12"
# dependencies = []
# ///
"""Run collector continuity scenarios with a virtual clock, no hardware or sleeps."""
from __future__ import annotations

import argparse
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
        with (patch.object(collector.time, "monotonic", side_effect=lambda: clock[0]),
              patch.object(collector.time, "sleep", side_effect=sleep),
              patch.object(collector, "_STARTUP_GRACE_SECONDS", 0),
              patch.object(collector.subprocess, "Popen", Process),
              patch.object(collector.subprocess, "run", side_effect=query),
              patch.object(collector.glob, "glob", return_value=[str(status)]),
              patch.object(collector, "_thermal_zones", return_value=[]),
              patch.object(collector, "_target_hardware_state", return_value={})):
            # When xrandr loses the output without either process exiting.
            capture = collector.collect(args)
        # Then connection loss is latched and invalidates the run before the deadline.
        assert capture.get("display_connected") is False, capture
        assert capture["display_survived"] is True
        assert capture["observed_duration_seconds"] < 10
        doc = reporter.build_result(capture, duration_requested="10s", now="2026-10-15T00:00:00Z",
                                    capture_path=None, capture_sha256=None)
        assert doc["verdict"] != "PASS"
        assert "display_disconnected" in {v["metric"] for v in doc["violations"]}


if __name__ == "__main__":
    globals()[sys.argv[1]]()
    print(f"PASS continuity-{sys.argv[1]}")
