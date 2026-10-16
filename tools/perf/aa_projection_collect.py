#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# /// script
# requires-python = ">=3.12"
# dependencies = []
# ///
"""Task-32 wired performance collector.

Stdlib only. Runs where the wired receiver runs (the Jetson, normally invoked
through ``tools/perf/aa-projection-perf.sh`` over a bounded ssh). It launches
the receiver (and optionally the dummy display), samples the receiver process
and the thermal zones for the requested duration, parses the receiver/display
JSON event stream, and writes ONE capture document to stdout:

  {"schema":"aa-projection-perf-capture/1","mode":"wired",
   "duration_seconds":N,"crashes":0,"samples":[...],"events":{...},
   "target_hardware_state":{...}}

The capture is the raw record; budget verdicts are produced by
``aa_perf_report.py``. A metric the collector cannot observe is emitted as
``null`` -- never faked. Every subprocess wait is bounded.

This tool does not modify target state (no installs, no udev, no kernel
changes); it only launches the supplied binaries and reads procfs/sysfs.
"""
from __future__ import annotations

import argparse
from collections import deque
import glob
import json
import math
import os
import platform
import re
import shutil
import signal
import subprocess
import sys
import time
from pathlib import Path

CAPTURE_SCHEMA = "aa-projection-perf-capture/1"
_STARTUP_GRACE_SECONDS = 3.0
_TERMINATE_GRACE_SECONDS = 5.0
_PROJECTION_STARTUP_TIMEOUT_SECONDS = 60
_MEDIA_STALL_SECONDS = 10
_PROGRESS_KEYS = ("video_units", "audio_chunks", "video_presentations", "audio_writes", "av_pairs")

# Diagnostic keys the receiver/display event stream may carry (task-19
# vocabulary). Each maps to one measured metric. Absent keys stay null.
_DECODE_KEYS = {"p50_latency_ms": "decode_p50_ms", "p95_latency_ms": "decode_p95_ms",
                "fps": "decode_fps", "frames": "decode_frames"}
_IPC_KEYS = {"p95_latency_ms": "ipc_p95_added_latency_ms",
             "throughput_mbps": "ipc_throughput_mbps"}


def _read_text(path: str) -> str | None:
    try:
        return Path(path).read_text(encoding="utf-8", errors="replace").strip("\x00\n ")
    except OSError:
        return None


def _rss_mb(pid: int) -> float | None:
    text = _read_text(f"/proc/{pid}/status")
    if text is None:
        return None
    for line in text.splitlines():
        if line.startswith("VmRSS:"):
            parts = line.split()
            if len(parts) >= 2:
                try:
                    return int(parts[1]) / 1024.0
                except ValueError:
                    return None
    return None


def _cpu_ticks(pid: int) -> int | None:
    text = _read_text(f"/proc/{pid}/stat")
    if text is None:
        return None
    try:
        rest = text.rsplit(")", 1)[1].split()
    except IndexError:
        return None
    if len(rest) < 13:
        return None
    try:
        return int(rest[11]) + int(rest[12])
    except ValueError:
        return None


def _thermal_zones() -> list[dict]:
    zones: list[dict] = []
    for temp_path in sorted(glob.glob("/sys/class/thermal/thermal_zone*/temp")):
        zone_dir = os.path.dirname(temp_path)
        raw = _read_text(temp_path)
        if raw is None:
            continue
        try:
            temp_c = float(raw) / 1000.0
        except ValueError:
            continue
        if not math.isfinite(temp_c):
            continue
        name = _read_text(os.path.join(zone_dir, "type")) or os.path.basename(zone_dir)
        zones.append({"zone": name, "temp_c": temp_c})
    return zones


def _target_hardware_state() -> dict:
    model = _read_text("/proc/device-tree/model") or _read_text("/sys/firmware/devicetree/base/model")
    l4t = None
    teg = _read_text("/etc/nv_tegra_release")
    if teg:
        l4t = teg.splitlines()[0]
    power_mode = None
    if shutil.which("nvpmodel"):
        try:
            out = subprocess.run(["nvpmodel", "-q"], capture_output=True, text=True, timeout=5)
            power_mode = out.stdout.strip() or None
        except (OSError, subprocess.SubprocessError):
            power_mode = None
    return {
        "machine": platform.machine(),
        "kernel_release": platform.release(),
        "model": model,
        "l4t_release": l4t,
        "power_mode": power_mode,
        "cpufreq_governor": _read_text("/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor"),
    }


def _display_connected() -> bool:
    if any(_read_text(path) == "connected"
           for path in glob.glob("/sys/class/drm/card*-*/status")):
        return True
    try:
        query = subprocess.run(["xrandr", "--query"], capture_output=True, text=True, timeout=5)
    except (OSError, subprocess.SubprocessError):
        return False
    return query.returncode == 0 and re.search(
        r"^\S+\s+connected(?:\s|$)", query.stdout, re.MULTILINE) is not None


class ProjectionProgress:
    """Accumulate a fail-closed active window from observed, increasing counters."""

    def __init__(self) -> None:
        self.started_ms: int | None = None
        self.last_ms = 0
        self.previous: dict[str, int] = {}
        self.advanced_ms: dict[str, int] = {}
        self.failure: str | None = None

    @property
    def seconds(self) -> float:
        return (self.last_ms - self.started_ms) / 1000 if self.started_ms is not None else 0.0

    def observe(self, sample: dict) -> None:
        if self.failure:
            return
        stamp = sample.get("t_ms")
        counters = {key: sample.get(key) for key in _PROGRESS_KEYS}
        valid = (type(stamp) is int and stamp >= 0
                 and all(type(value) is int and value >= 0 for value in counters.values()))
        if not valid:
            if self.started_ms is not None:
                self.failure = "missing_progress"
            return
        if self.started_ms is None:
            if sample.get("projection_active") is not True or not all(counters.values()):
                return
            self.started_ms = stamp
            self.previous = counters
            self.advanced_ms = {key: stamp for key in counters}
        else:
            if stamp < self.last_ms or stamp - self.last_ms > _MEDIA_STALL_SECONDS * 1000:
                self.failure = "sampling_gap"
            if sample.get("projection_active") is not True:
                self.failure = "projection_inactive"
            for key, value in counters.items():
                if value < self.previous[key]:
                    self.failure = "counter_reset:" + key
                # Check age before accepting a resumed stream: a late recovery cannot heal a freeze.
                if stamp - self.advanced_ms[key] > _MEDIA_STALL_SECONDS * 1000:
                    self.failure = "media_stalled:" + key
                if value > self.previous[key]:
                    self.advanced_ms[key] = stamp
            self.previous = counters
        self.last_ms = stamp


class EventParser:
    """Incrementally parse newline-delimited JSON events from log files."""

    def __init__(self) -> None:
        self.offsets: dict[str, int] = {}
        self.metrics: dict[str, object] = {
            "frame_drops": None, "decode_p50_ms": None, "decode_p95_ms": None,
            "decode_fps": None, "decode_frames": None, "ipc_throughput_mbps": None,
            "ipc_p95_added_latency_ms": None, "av_sync_ms": None, "audio_latency_ms": None,
            "audio_scheduling_skew_ms": None,
        }
        self.video_units: int | None = None
        self.audio_chunks: int | None = None
        self.startup_ms: int | None = None
        self.invalid_telemetry: list[str] = []
        self.terminal_errors: list[dict] = []
        self.audio_completions: deque[tuple[int, int, int]] = deque()
        self.video_completions: deque[tuple[int, int]] = deque()
        self.av_pairs = 0
        self.audio_writes = 0
        self.video_presentations = 0
        self.projection_active = False
        self.last_audio_scheduled = -1
        self.last_video_scheduled = -1

    def _completion(self, event: dict) -> None:
        """Join endpoint events by concurrent scheduled presentation, not log arrival."""
        audio = event["event"] == "audio_write_completed"
        keys = ("scheduled_ns", "scheduled_end_ns", "completed_ns") if audio else (
            "scheduled_ns", "completed_ns")
        values = [event.get(key) for key in keys]
        if any(type(value) is not int or value < 0 for value in values):
            self.invalid_telemetry.append("av_sync_ms")
            self.metrics["av_sync_ms"] = None
            return
        scheduled, completed = event["scheduled_ns"], event["completed_ns"]
        if audio:
            end = event["scheduled_end_ns"]
            if end <= scheduled:
                self.invalid_telemetry.append("av_sync_ms")
                self.metrics["av_sync_ms"] = None
                return
            if scheduled <= self.last_audio_scheduled:
                return
            self.last_audio_scheduled = scheduled
            self.audio_writes += 1
            self.audio_completions.append((scheduled, end, completed))
        else:
            if scheduled <= self.last_video_scheduled:
                return
            self.last_video_scheduled = scheduled
            self.video_presentations += 1
            self.video_completions.append((scheduled, completed))
        pending: deque[tuple[int, int]] = deque()
        for stamp, video_done in self.video_completions:
            while self.audio_completions and self.audio_completions[0][1] <= stamp:
                self.audio_completions.popleft()
            match = next((done for begin, end, done in self.audio_completions
                          if begin <= stamp < end), None)
            if match is None:
                pending.append((stamp, video_done))
            else:
                self._number({"delta": abs(video_done - match) / 1e6}, "delta",
                             "av_sync_ms", peak=True)
                self.av_pairs += 1
        self.video_completions = pending
        # Bound retention even if a stream disappears. Missing pairs never become zero.
        while len(self.audio_completions) > 16384:
            self.audio_completions.popleft()
        if len(self.video_completions) > 16384:
            self.video_completions.popleft()
            self.invalid_telemetry.append("av_sync_ms")
            self.metrics["av_sync_ms"] = None

    def _number(self, event: dict, key: str, target: str, *, integer: bool = False,
                peak: bool = False) -> None:
        if key not in event:
            return
        raw = event[key]
        try:
            valid = (not isinstance(raw, bool) and isinstance(raw, (int, float))
                     and math.isfinite(raw) and (not integer or isinstance(raw, int)))
        except OverflowError:
            valid = False
        if not valid:
            self.metrics[target] = None
            if target not in self.invalid_telemetry:
                self.invalid_telemetry.append(target)
            return
        if target in self.invalid_telemetry:
            return
        value = int(raw) if integer else float(raw)
        prior = self.metrics[target]
        self.metrics[target] = max(prior, value) if peak and prior is not None else value

    def poll(self, path: str) -> None:
        try:
            size = os.path.getsize(path)
        except OSError:
            return
        offset = self.offsets.get(path, 0)
        if size < offset:
            offset = 0
        if size == offset:
            return
        try:
            with open(path, "r", encoding="utf-8", errors="replace") as handle:
                handle.seek(offset)
                for line in handle:
                    self._consume(line)
                self.offsets[path] = handle.tell()
        except OSError:
            return

    def _consume(self, line: str) -> None:
        line = line.strip()
        if not line.startswith("{"):
            return
        try:
            event = json.loads(line)
        except json.JSONDecodeError:
            return
        if not isinstance(event, dict):
            return
        envelope = event.get("event")
        # Diagnostics arrive wrapped as {"event":"diag","kind":"<kind>",...};
        # a bare event line names its kind in "event" itself.
        inner = event.get("kind")
        kind = inner if isinstance(inner, str) else envelope
        if envelope == "err":
            self.terminal_errors.append({"event": "err", "code": str(event.get("code", "unknown"))})
        if envelope == "video_sent" and isinstance(event.get("units"), int):
            self.video_units = event["units"]
        if envelope == "audio_chunks" and isinstance(event.get("count"), int):
            self.audio_chunks = event["count"]
        if envelope == "active" and isinstance(event.get("startup_ms"), int):
            self.startup_ms = event["startup_ms"]
            self.projection_active = True
        if envelope in ("stopping", "disconnected", "err"):
            self.projection_active = False
        if kind == "transport_stats":
            for key, target in _IPC_KEYS.items():
                self._number(event, key, target)
        if kind == "decode_stats":
            for key, target in _DECODE_KEYS.items():
                self._number(event, key, target, integer=target == "decode_frames")
            for drops_key in ("dropped", "drops"):
                self._number(event, drops_key, "frame_drops", integer=True)
        if envelope in ("audio_write_completed", "video_presented"):
            self._completion(event)
        # A legacy audio-only av_sync_ms field is not cross-stream evidence.
        if "av_sync_ms" in event:
            raw = event["av_sync_ms"]
            if not isinstance(raw, (int, float)) or isinstance(raw, bool) or not math.isfinite(raw):
                self.invalid_telemetry.append("av_sync_ms")
                self.metrics["av_sync_ms"] = None
        self._number(event, "audio_scheduling_skew_ms", "audio_scheduling_skew_ms", peak=True)
        self._number(event, "audio_latency_ms", "audio_latency_ms", peak=True)
        self._number(event, "frame_drops", "frame_drops", integer=True)


def _terminate(proc: subprocess.Popen | None) -> None:
    if proc is None or proc.poll() is not None:
        return
    proc.terminate()
    try:
        proc.wait(timeout=_TERMINATE_GRACE_SECONDS)
    except subprocess.TimeoutExpired:
        proc.kill()
        try:
            proc.wait(timeout=_TERMINATE_GRACE_SECONDS)
        except subprocess.TimeoutExpired:
            pass


def collect(args: argparse.Namespace) -> dict:
    state_dir = Path(args.state_dir)
    state_dir.mkdir(parents=True, exist_ok=True)
    receiver_log = state_dir / "receiver.log"
    display_log = state_dir / "display.log"

    startup_timeout = getattr(args, "startup_timeout_seconds", _PROJECTION_STARTUP_TIMEOUT_SECONDS)
    receiver_cmd = [args.receiver, "--port", args.port, "--duration-ms",
                    str((args.duration_seconds + startup_timeout + 10) * 1000)]
    if args.store:
        receiver_cmd += ["--store", args.store]

    samples: list[dict] = []
    parser = EventParser()
    progress = ProjectionProgress()
    receiver: subprocess.Popen | None = None
    display: subprocess.Popen | None = None
    rec_file = None
    disp_file = None
    crashes = 0
    receiver_exit: int | None = None
    early_exit = False
    stopped_by_us = False
    display_survived: bool | None = None
    display_connected: bool | None = None
    start = time.monotonic()
    observed_duration = 0.0

    try:
        rec_file = open(receiver_log, "w", encoding="utf-8")
        receiver = subprocess.Popen(receiver_cmd, stdout=rec_file, stderr=subprocess.STDOUT,
                                    start_new_session=True)
        if args.display and not args.no_display:
            time.sleep(_STARTUP_GRACE_SECONDS)
            disp_file = open(display_log, "w", encoding="utf-8")
            display_cmd = [args.display, "--test-only-autostart"]
            if args.video_socket:
                display_cmd += ["--video-socket", args.video_socket]
            if args.input_socket:
                display_cmd += ["--input-socket", args.input_socket]
            display = subprocess.Popen(display_cmd, stdout=disp_file, stderr=subprocess.STDOUT,
                                       start_new_session=True)

        clk = os.sysconf("SC_CLK_TCK")
        prev_ticks = _cpu_ticks(receiver.pid)
        prev_time = time.monotonic()
        deadline = start + startup_timeout
        while True:
            now = time.monotonic()
            if display is not None:
                display_connected = _display_connected()
                if not display_connected:
                    break
            if now >= deadline:
                break
            if receiver.poll() is not None:
                early_exit = True
                break
            if display is not None and display.poll() is not None:
                display_survived = False
                break
            if parser.terminal_errors:
                break
            time.sleep(min(args.interval, 5.0, max(0.0, deadline - now)))
            now = time.monotonic()
            ticks = _cpu_ticks(receiver.pid)
            cpu = None
            if ticks is not None and prev_ticks is not None and now > prev_time:
                cpu = (ticks - prev_ticks) / clk / (now - prev_time) * 100.0
            rss = _rss_mb(receiver.pid)
            zones = _thermal_zones()
            peak = max((z["temp_c"] for z in zones), default=None)
            parser.poll(str(receiver_log))
            if display is not None:
                parser.poll(str(display_log))
            sample = {
                "t_ms": int((now - start) * 1000),
                "receiver_rss_mb": rss,
                "receiver_cpu_pct_one_core": cpu,
                "thermal_peak_c": peak,
                "thermal_zones": zones,
                "projection_active": parser.projection_active,
                "video_units": parser.video_units,
                "audio_chunks": parser.audio_chunks,
                "video_presentations": parser.video_presentations,
                "audio_writes": parser.audio_writes,
                "av_pairs": parser.av_pairs,
            }
            samples.append(sample)
            was_started = progress.started_ms is not None
            progress.observe(sample)
            if not was_started and progress.started_ms is not None:
                deadline = start + progress.started_ms / 1000 + args.duration_seconds
            if progress.failure:
                break
            prev_ticks, prev_time = ticks, now

        observed_duration = time.monotonic() - start
        if display is not None and display_survived is None:
            display_survived = display.poll() is None
        if receiver.poll() is None:
            try:
                receiver.wait(timeout=min(args.interval, _TERMINATE_GRACE_SECONDS))
            except subprocess.TimeoutExpired:
                stopped_by_us = True
                _terminate(receiver)
        receiver_exit = receiver.returncode
    except FileNotFoundError as exc:
        raise SystemExit(f"error: cannot launch receiver: {exc}") from exc
    finally:
        _terminate(display)
        _terminate(receiver)
        if rec_file is not None:
            rec_file.close()
        if disp_file is not None:
            disp_file.close()
        parser.poll(str(receiver_log))
        if display is not None:
            parser.poll(str(display_log))

    if receiver_exit is None and receiver is not None:
        receiver_exit = receiver.returncode
    if not stopped_by_us and receiver_exit is not None and receiver_exit != 0:
        crashes = 1
    if not stopped_by_us and not early_exit and receiver_exit is None:
        crashes = 1

    events = dict(parser.metrics)
    events["helper_rss_mb"] = None
    events["video_units"] = parser.video_units
    events["audio_chunks"] = parser.audio_chunks
    events["startup_ms"] = parser.startup_ms

    return {
        "schema": CAPTURE_SCHEMA,
        "mode": "wired",
        "duration_seconds": args.duration_seconds,
        "observed_duration_seconds": observed_duration,
        "active_projection_seconds": progress.seconds,
        "startup_wait_seconds": progress.started_ms / 1000 if progress.started_ms is not None else None,
        "projection_failure": progress.failure,
        "measurement_origin": "live",
        "crashes": crashes,
        "early_exit": early_exit,
        "receiver_exit_code": receiver_exit,
        "receiver_stopped_by_collector": stopped_by_us,
        "display_survived": display_survived,
        "display_connected": display_connected,
        "terminal_errors": parser.terminal_errors,
        "invalid_telemetry": parser.invalid_telemetry,
        "samples": samples,
        "events": events,
        "target_hardware_state": _target_hardware_state(),
    }


def _cli(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--receiver", required=True, help="receiver binary to launch")
    parser.add_argument("--display", default=None, help="dummy display binary (optional)")
    parser.add_argument("--port", required=True, help="USB port context passed to the receiver")
    parser.add_argument("--duration-seconds", required=True, type=int)
    parser.add_argument("--interval", type=float, default=5.0)
    parser.add_argument("--store", default=None, help="approved-phone store path passed to receiver")
    parser.add_argument("--state-dir", default="/tmp/aa-projection-perf")
    parser.add_argument("--video-socket", default=None)
    parser.add_argument("--input-socket", default=None)
    parser.add_argument("--no-display", action="store_true")
    parser.add_argument("--startup-timeout-seconds", type=int,
                        default=_PROJECTION_STARTUP_TIMEOUT_SECONDS,
                        help="startup timeout, may only shorten the default 60-second grace")
    args = parser.parse_args(argv)

    if args.duration_seconds <= 0:
        print("error: --duration-seconds must be positive", file=sys.stderr)
        return 2
    if not 0 < args.startup_timeout_seconds <= _PROJECTION_STARTUP_TIMEOUT_SECONDS:
        print("error: --startup-timeout-seconds must be in [1, 60]", file=sys.stderr)
        return 2
    if not math.isfinite(args.interval) or args.interval <= 0:
        print("error: --interval must be positive", file=sys.stderr)
        return 2

    old_handler = signal.getsignal(signal.SIGTERM)
    signal.signal(signal.SIGTERM, lambda *_: sys.exit(3))
    try:
        capture = collect(args)
    finally:
        signal.signal(signal.SIGTERM, old_handler)

    json.dump(capture, sys.stdout, indent=2, sort_keys=True, allow_nan=False)
    sys.stdout.write("\n")
    return 0


if __name__ == "__main__":
    sys.exit(_cli(sys.argv[1:]))
