#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# /// script
# requires-python = ">=3.12"
# dependencies = []
# ///
"""Run: python3 -B tests/perf/test_measurement_freeze.py (synthetic HOST only)."""
from __future__ import annotations

import json
from pathlib import Path
import shlex
import signal
import subprocess
import sys
import tempfile
from unittest.mock import patch

import test_startup_repair as helper
from test_startup_repair import collector, SECOND, ROOT


def batch(stamp: int, *, latency: int = 10, av: int = 1) -> list[dict]:
    return [{"event": "video_sent", "units": stamp // SECOND},
            {"event": "audio_chunks", "count": stamp // SECOND},
            {"event": "audio_write_completed", "scheduled_ns": stamp,
             "scheduled_end_ns": stamp + 20_000_000, "completed_ns": stamp},
            {"event": "video_presented", "scheduled_ns": stamp,
             "completed_ns": stamp + av * 1_000_000},
            {"event": "media_timing", "audio_latency_ms": latency, "audio_scheduling_skew_ms": 2},
            {"event": "decode_stats", "dropped": 3}]


def virtual(mode: str) -> dict:
    """Use existing clock/process seams; leave parsing and reporting real."""
    helper.reset()
    parsers = []

    class Parser(collector.EventParser):
        def __init__(self) -> None:
            super().__init__()
            parsers.append(self)

    class Process(helper.Process):
        def drain(self) -> None:
            helper.clock[0] += SECOND
            late = batch(5 * SECOND, latency=250, av=80)
            late += [{"event": "active", "startup_ms": 999},
                     {"event": "video_sent", "units": 999},
                     {"event": "audio_chunks", "count": 888},
                     {"event": "media_timing", "audio_scheduling_skew_ms": 300},
                     {"event": "decode_stats", "dropped": 77}]
            if mode == "split":
                late = [{"event": "video_presented", "scheduled_ns": SECOND,
                         "completed_ns": SECOND + 80_000_000}]
            if mode in {"err", "exit7", "crash", "display", "owned_display", "forced", "unreaped", "startup"}:
                late = []
            if mode in {"malformed", "breach", "whole"}:
                late += [{"event": "media_timing", "audio_latency_ms": "bad", "av_sync_ms": "bad"}]
            if mode == "err":
                late += [{"event": "err", "code": "shutdown-diagnostic"}]
            if mode != "baseline":
                for event in late:
                    self.stdout.write(json.dumps(event) + "\n")
                self.stdout.flush()
            if mode == "whole":
                parser = parsers[0]
                parser.metrics.update({key: 999 for key in parser.metrics})
                parser.invalid_telemetry.append("late-invalid")
                video = parser.video_measurements
                video.capacity_lost = True
                video.ipc_matched = video.receive_bytes = video.unmatched = 999
                video.ipc_samples.append(999)
                video.decode_samples.append(999)
                endpoint = collector.IpcEndpoint((7, 8, 999, 999, 999), 0, SECOND, 1024)
                video.sends[endpoint.key] = endpoint
                video.receives[endpoint.key] = endpoint

        def terminate(self) -> None:
            if self is helper.spawn[0]:
                self.drain()
            if mode not in {"forced", "unreaped"}:
                self.returncode = (-15 if mode == "owned_display" and self is helper.spawn[1]
                                   else 7 if mode == "exit7" else 0)

        def kill(self) -> None:
            if mode != "unreaped":
                self.returncode = -9

        def wait(self, timeout: float) -> None:
            if mode in {"wait", "crash", "display"} and self.returncode is None:
                self.drain()
                self.returncode = 7 if mode == "crash" else 0
                if mode == "display":
                    helper.spawn[1].returncode = 0
            super().wait(timeout)

    events = {}
    for second in range(1, 5):
        breach = mode == "breach" or (mode == "startup" and second == 1)
        lines = batch(second * SECOND, latency=250 if breach else 10, av=80 if breach else 1)
        if mode == "split":
            lines = [event for event in lines if event["event"] != "video_presented"]
        if mode != "inactive" and not (mode == "startup" and second == 1):
            lines += [{"event": "active", "startup_ms": 1}]
        if mode in {"display", "owned_display"}:
            lines += [helper.READY]
        events[second * SECOND] = [("receiver", json.dumps(event)) for event in lines]
    with (patch.object(collector, "EventParser", Parser),
          patch.object(collector, "_rss_mb", return_value=10.0),
          patch.object(collector, "_cpu_ticks", return_value=0)):
        managed = mode in {"display", "owned_display"}
        return helper.run_collect(helper.args_ns(display="display" if managed else None,
            no_display=not managed, duration_seconds=3, interval=1,
            startup_timeout_seconds=4), events, Process)


def test_drain_cannot_mutate_any_measurement_field() -> None:
    # Given identical samples, compare termination, grace-wait and malformed drains.
    baseline = virtual("baseline")
    fields = ("events", "invalid_telemetry", "video_telemetry", "samples", "video_evidence",
              "active_projection_seconds", "startup_wait_seconds", "projection_failure")
    for mode in ("late", "wait", "malformed", "whole"):
        # When the actual collector seals then drains, all measurement exports stay equal.
        capture = virtual(mode)
        assert {key: capture[key] for key in fields} == {key: baseline[key] for key in fields}, mode
        assert helper.result(capture, "3s")["verdict"] == "BLOCKED", mode


def test_late_activation_and_backdated_pair_cannot_heal() -> None:
    # Given no eligible active window or no video counterpart before cutoff.
    for mode in ("inactive", "split"):
        # When late active/counters/pairs are drained, absence remains absence.
        capture = virtual(mode)
        assert capture["active_projection_seconds"] == 0
        if mode == "inactive":
            assert capture["events"]["startup_ms"] is None
            assert capture["events"]["video_units"] == 4
        else:
            assert capture["events"]["av_sync_ms"] is None
        assert helper.result(capture, "3s")["verdict"] == "BLOCKED"


def test_lifecycle_failures_remain_live() -> None:
    # Given late lifecycle failures, including successful unexpected display exit.
    for mode, metric in (("err", "terminal_errors"), ("exit7", "receiver_exit_code"),
                         ("crash", "crashes"), ("display", "display_survived"),
                         ("forced", "terminal_errors"), ("unreaped", "terminal_errors"),
                         ("breach", "audio_latency_ms")):
        # When cleanup completes, unchanged reporter FAIL precedence still applies.
        capture = virtual(mode)
        doc = helper.result(capture, "3s")
        assert doc["verdict"] == "FAIL", (mode, doc)
        assert metric in {item["metric"] for item in doc["violations"]}, mode
        if mode == "crash":
            assert capture["crashes"] == 1 and not capture["receiver_stopped_by_collector"]
        if mode == "exit7":
            assert capture["receiver_exit_code"] == 7 and capture["crashes"] == 0
        if mode in {"forced", "unreaped"}:
            assert capture["terminal_errors"] == [{"event": "err", "code": "receiver_cleanup_" +
                ("forced_kill" if mode == "forced" else "unreaped")}]
    owned = virtual("owned_display")
    assert owned["display_survived"] is True and owned["crashes"] == 0
    assert not helper.result(owned, "3s")["violations"]


def test_admitted_startup_peaks_are_not_reset_at_active_start() -> None:
    # Given startup-only breaches followed by lower active observations.
    capture = virtual("startup")
    # Then sealing retains cumulative peaks without subtracting at active start.
    assert capture["startup_wait_seconds"] == 2
    assert capture["events"]["audio_latency_ms"] == 250
    assert capture["events"]["av_sync_ms"] == 80
    assert helper.result(capture, "3s")["verdict"] == "FAIL"


def fixture(mode: str) -> None:
    """Finite real receiver child; only its SIGTERM handler emits late input."""
    def emit(events: list[dict]) -> None:
        for event in events:
            print(json.dumps(event), flush=True)

    def stop(signum, frame) -> None:
        emit([{"event": "sigterm_hook", "observed_ns": collector.time.monotonic_ns()}])
        if mode in {"late", "av75"}:
            emit(batch(5 * SECOND, latency=250, av=75 if mode == "av75" else 80))
            emit([{"event": "media_timing", "audio_scheduling_skew_ms": 300},
                  {"event": "decode_stats", "dropped": 77},
                  {"event": "video_sent", "units": 999}, {"event": "audio_chunks", "count": 888}])
        if mode == "err":
            emit([{"event": "err", "code": "shutdown-diagnostic"}])
        raise SystemExit(7 if mode == "exit7" else 0)

    signal.alarm(6)
    if mode != "signal":
        signal.signal(signal.SIGTERM, stop)
    emit(batch(SECOND, latency=250 if mode == "runtime" else 10))
    signal.pause()


def test_real_cli_shutdown() -> None:
    # Given a bounded executable HOST child in a private directory, no active claim.
    for mode in ("baseline", "late", "av75", "runtime", "err", "exit7", "signal"):
        with tempfile.TemporaryDirectory(prefix="perf-freeze-cli-") as tmp:
            receiver = Path(tmp) / "receiver"
            receiver.write_text("#!/bin/sh\nset -eu\nexec " + shlex.join(
                [sys.executable, "-B", str(Path(__file__).resolve()), "--fixture", mode]) + "\n")
            receiver.chmod(0o755)
            command = [sys.executable, "-B", str(ROOT / "tools/perf/aa_projection_collect.py"),
                       "--receiver", str(receiver), "--port", "synthetic-host", "--no-display",
                       "--duration-seconds", "1", "--startup-timeout-seconds", "1",
                       "--interval", "0.1", "--state-dir", tmp]
            # When the real CLI reaches cutoff and sends SIGTERM, it must reap the child.
            with subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True) as proc:
                try:
                    stdout, stderr = proc.communicate(timeout=8)
                except subprocess.TimeoutExpired:
                    proc.terminate()
                    proc.communicate(timeout=12)
                    raise
            assert proc.returncode == 0, stderr
            assert not stderr, stderr
            capture = json.loads(stdout)
            doc = helper.result(capture, "1s")
            log = [json.loads(line) for line in (Path(tmp) / "receiver.log").read_text().splitlines()]
            if mode != "signal":
                marker = next(event["observed_ns"] for event in log if event["event"] == "sigterm_hook")
                assert marker > capture["monotonic_origin_ns"] + capture["samples"][-1]["t_ns"]
            if mode in {"late", "av75"}:
                assert any(event.get("audio_latency_ms") == 250 for event in log)
                assert any(event.get("units") == 999 for event in log)
                assert any(event.get("count") == 888 for event in log)
                assert any(event.get("completed_ns") == 5 * SECOND +
                           (75 if mode == "av75" else 80) * 1_000_000 for event in log)
            expected = (250 if mode == "runtime" else 10, 1, 2, 3)
            actual = tuple(capture["events"][key] for key in
                           ("audio_latency_ms", "av_sync_ms", "audio_scheduling_skew_ms", "frame_drops"))
            assert actual == expected, (mode, actual)
            assert capture["events"]["video_units"] == capture["events"]["audio_chunks"] == 1
            assert capture["active_projection_seconds"] == 0
            assert doc["verdict"] == ("BLOCKED" if mode in {"baseline", "late", "av75"} else "FAIL")
            assert doc["qualification_status"] == "INCOMPLETE"
            print("CLI", mode, actual, doc["verdict"], flush=True)


if __name__ == "__main__":
    if len(sys.argv) > 1 and sys.argv[1] == "--fixture":
        fixture(sys.argv[2])
    else:
        tests = [value for name, value in list(globals().items()) if name.startswith("test_")]
        for test in tests:
            test()
        print(f"PASS {len(tests)} measurement freeze tests")
