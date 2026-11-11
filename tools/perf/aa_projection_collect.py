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

  {"schema":"aa-projection-perf-capture/2","mode":"wired",
   "duration_seconds":N,"crashes":0,"samples":[...],"events":{...},
   "target_hardware_state":{...}}

The capture is the raw record; budget verdicts are produced by
``aa_perf_report.py``. A metric the collector cannot observe is emitted as
``null`` -- never faked. Every subprocess wait is bounded.

This tool does not modify target state (no installs, no udev, no kernel
changes); it only launches the supplied binaries and reads procfs/sysfs.
"""
from __future__ import annotations

# allow: SIZE_OK - preserve the existing collector lifecycle and parser in the frozen owned file set.

import argparse
from collections import deque
from collections.abc import Mapping
from dataclasses import asdict
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
from typing import Literal

from aa_video_measurements import DecodeEndpoint, IpcEndpoint, VideoMeasurements
from aa_measurement_coverage import (
    CoverageError, NANO_PROFILE, Sample, ThermalZone, VideoEvidence, thermal_policy_for_model,
)

CAPTURE_SCHEMA = "aa-projection-perf-capture/2"
_TERMINATE_GRACE_SECONDS = 5.0
_PROJECTION_STARTUP_TIMEOUT_SECONDS = 60
_MEDIA_STALL_SECONDS = 10
# A normal sample interval is at most 5 s (see the sleeps in collect) plus a short
# parse. Anything beyond this is the collector itself having been frozen, so it is
# recorded as host interference; the margin below _MEDIA_STALL_SECONDS means gaps
# that are close to, but under, the stall threshold are not silently dropped (#90).
_COLLECTOR_INTERFERENCE_SECONDS = 7
# The I/O freeze that stops the collector also delays the receiver's log reaching
# disk, so the first sample after a gap can miss events the receiver already
# logged. After a gap over _MEDIA_STALL_SECONDS the collector re-polls for at most
# this long before judging. Five seconds is half the stall threshold: long enough
# for the page cache to drain once the disk is back, short enough that a dead
# receiver is still reported, and a bounded addition to the observed gap.
_COLLECTOR_LOG_GRACE_SECONDS = 5.0
_COLLECTOR_LOG_POLL_SECONDS = 0.25
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
        ticks = int(rest[11]) + int(rest[12])
        return ticks if ticks >= 0 else None
    except ValueError:
        return None


def _thermal_inventory() -> Mapping[str, str]:
    return {os.path.basename(path) + ":" + (_read_text(os.path.join(path, "type")) or "unknown"): path
            for path in sorted(glob.glob("/sys/class/thermal/thermal_zone*"))}


def _thermal_zones(expected: tuple[ThermalZone, ...] = (),
                   observed: Mapping[str, str] | None = None) -> tuple[ThermalZone, ...]:
    inventory = _thermal_inventory() if observed is None else observed
    zones: list[ThermalZone] = []
    for name in sorted(set(inventory) | {zone.zone for zone in expected}):
        path = inventory.get(name)
        raw = _read_text(os.path.join(path, "temp")) if path is not None else None
        try:
            temp_c = float(raw) / 1000.0 if raw is not None else None
        except ValueError:
            temp_c = None
        if temp_c is not None and not math.isfinite(temp_c):
            temp_c = None
        zones.append(ThermalZone(name, temp_c))
    return tuple(zones)


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
        self.started_ns: int | None = None
        self.last_ns = 0
        self.previous: dict[str, int] = {}
        self.advanced_ns: dict[str, int] = {}
        self.failure: str | None = None
        self.interferences: list[dict] = []

    @property
    def seconds(self) -> float:
        return (self.last_ns - self.started_ns) / 1e9 if self.started_ns is not None else 0.0

    def covers_gap(self, sample: Sample, logged_progress: dict[str, dict] | None) -> bool:
        """Whether receiver-logged events show every stream flowing through a gap.

        Judged on the receiver's own event times, never on counter deltas: a count
        that moved by one says nothing about where inside the window it moved.
        """
        return (logged_progress is not None and bool(self.previous)
                and all(self._continuous(key, getattr(sample, key), sample.t_ns, logged_progress)
                        for key in _PROGRESS_KEYS))

    def _continuous(self, key: str, value: int | None, stamp: int,
                    logged_progress: dict[str, dict]) -> bool:
        evidence = logged_progress.get(key)
        if not isinstance(evidence, dict) or type(value) is not int:
            return False
        count, silence, last = (evidence.get("event_count"), evidence.get("max_silence_ns"),
                                evidence.get("last_event_ns"))
        return (value > self.previous.get(key, value)
                and type(count) is int and count > 0
                and type(silence) is int and 0 <= silence <= _MEDIA_STALL_SECONDS * 1_000_000_000
                and type(last) is int and self.last_ns < last <= stamp)

    def observe(self, sample: Sample, logged_progress: dict[str, dict] | None = None) -> None:
        """Fold in one sample.

        ``logged_progress`` is receiver-clock evidence for the window since the last
        sample (see EventParser.logged_progress). It is consulted only when the
        collector's own sampling gap exceeds _MEDIA_STALL_SECONDS; with no such gap
        this is the historical age-before-resume check, unchanged.
        """
        if self.failure:
            return
        stamp = sample.t_ns
        counters = {key: value for key, value in zip(_PROGRESS_KEYS, (
            sample.video_units, sample.audio_chunks, sample.video_presentations,
            sample.audio_writes, sample.av_pairs)) if value is not None}
        valid = (len(counters) == len(_PROGRESS_KEYS) and type(stamp) is int and stamp >= 0
                 and all(type(value) is int and value >= 0 for value in counters.values()))
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
            sampling_gap_ns = stamp - self.last_ns
            interference = None
            if sampling_gap_ns > _COLLECTOR_INTERFERENCE_SECONDS * 1_000_000_000:
                interference = {
                    "reason": "collector_sampling_gap",
                    "classification": "host_interference",
                    "gap_seconds": sampling_gap_ns / 1e9,
                    "gap_ns": sampling_gap_ns,
                    "start_ns": self.last_ns,
                    "t_ns": stamp,
                }
                self.interferences.append(interference)
            gap_judged = (logged_progress is not None
                          and sampling_gap_ns > _MEDIA_STALL_SECONDS * 1_000_000_000)
            if not gap_judged and (stamp <= self.last_ns
                                   or sampling_gap_ns > _MEDIA_STALL_SECONDS * 1_000_000_000):
                self.failure = "sampling_gap"
            if not sample.projection_active:
                self.failure = "projection_inactive"
            for key, value in counters.items():
                if value < self.previous[key]:
                    self.failure = "counter_reset:" + key
                if gap_judged:
                    if self._continuous(key, value, stamp, logged_progress):
                        self.advanced_ns[key] = logged_progress[key]["last_event_ns"]
                    else:
                        self.failure = "media_stalled:" + key
                    continue
                # Check age before accepting a resumed stream: a late recovery cannot heal a freeze.
                if stamp - self.advanced_ns[key] > _MEDIA_STALL_SECONDS * 1_000_000_000:
                    self.failure = "media_stalled:" + key
                if value > self.previous[key]:
                    self.advanced_ns[key] = stamp
            if interference is not None:
                # The gap is context for, never a substitute for, the verdict.
                interference["projection_failure"] = self.failure
                if gap_judged:
                    interference["logged_progress"] = logged_progress
            self.previous = counters
        self.last_ns = stamp


_STALL_SURVIVED = frozenset({"write_returned", "sink_reopened"})


class EventParser:
    """Incrementally parse newline-delimited JSON events from log files."""

    def __init__(self) -> None:
        self.offsets: dict[str, int] = {}
        self.metrics: dict[str, object] = {
            "frame_drops": None, "decode_p50_ms": None, "decode_p95_ms": None,
            "decode_fps": None, "decode_frames": None, "ipc_throughput_mbps": None,
            "ipc_p95_added_latency_ms": None, "av_sync_ms": None, "audio_latency_ms": None,
            "audio_scheduling_skew_ms": None, "audio_stall_dropouts": 0,
            "audio_stall_dropped_chunks": 0, "audio_stall_unrecovered": 0,
        }
        self.stall_open = False
        self.stall_failed = 0
        self.video_units: int | None = None
        self.audio_chunks: int | None = None
        self.startup_ms: int | None = None
        self.invalid_telemetry: list[str] = []
        self.terminal_errors: list[dict] = []
        self.audio_completions: deque[tuple[int, int, int]] = deque()
        self.video_completions: deque[tuple[int, int]] = deque()
        self.av_pairs = 0
        self.audio_writes = 0
        # Receiver-clock times (the receiver's own monotonic stamps) of the events
        # behind each progress counter, kept only since the last accepted sample,
        # plus the newest earlier event per stream so a window's leading silence is
        # measured from the stream's real last event. video_units / audio_chunks
        # log no timestamp of their own; their evidence is the timestamped
        # video_presented / audio_write_completed events of the same stream.
        self.media_event_times: dict[str, list[int]] = {key: [] for key in _PROGRESS_KEYS}
        self.media_last_before: dict[str, int | None] = {key: None for key in _PROGRESS_KEYS}
        # The audio A/V endpoint the run's first write used (#60):
        # "playback" (presented_ns) or "handoff" (legacy completed_ns).
        self.audio_endpoint: str | None = None
        self.video_presentations = 0
        self.receiver_ready = False
        self.projection_active = False
        self.last_audio_scheduled = -1
        self.last_video_scheduled = -1
        self.video_measurements = VideoMeasurements()

    def _record_progress(self, keys: tuple[str, ...], stamp: int) -> None:
        for key in keys:
            self.media_event_times[key].append(stamp)

    def progress_sampled(self, sample_ns: int, origin_ns: int) -> None:
        """Close the observation window at an accepted sample (collector clock)."""
        absolute = origin_ns + sample_ns
        for key, times in self.media_event_times.items():
            older = [stamp for stamp in times if stamp <= absolute]
            if older:
                prior = self.media_last_before[key]
                self.media_last_before[key] = max(older) if prior is None else max(prior, max(older))
            self.media_event_times[key] = [stamp for stamp in times if stamp > absolute]

    def logged_progress(self, origin_ns: int, start_ns: int, end_ns: int) -> dict[str, dict]:
        """Receiver-clock continuity of each stream over (start_ns, end_ns].

        ``max_silence_ns`` is the longest stretch with no logged event, measured from
        the stream's last event before the window (or the window start if it has
        none) through to the window end.
        """
        result: dict[str, dict] = {}
        for key, recorded in self.media_event_times.items():
            times = sorted(stamp - origin_ns for stamp in recorded
                           if start_ns < stamp - origin_ns <= end_ns)
            prior = self.media_last_before[key]
            cursor = min(start_ns, prior - origin_ns) if prior is not None else start_ns
            max_silence = 0
            for stamp in times:
                max_silence = max(max_silence, stamp - cursor)
                cursor = stamp
            max_silence = max(max_silence, end_ns - cursor)
            result[key] = {
                "event_count": len(times),
                "first_event_ns": times[0] if times else None,
                "last_event_ns": times[-1] if times else None,
                "max_silence_ns": max_silence,
            }
        return result

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
            # Owner-approved 2026-11-08 (#60): audio is "done" when it starts
            # playing on the sink's clock (presented_ns), not at the element
            # handoff; pulsesink commits ahead of playback. A receiver on this
            # playback-time contract logs the key on EVERY write (null when
            # the sink gave no slot); legacy logs never carry it and keep the
            # handoff endpoint. One run never mixes the two: a key missing
            # from some writes, or a null/malformed value, invalidates A/V.
            contract = "playback" if "presented_ns" in event else "handoff"
            if self.audio_endpoint is None:
                self.audio_endpoint = contract
            if contract != self.audio_endpoint:
                self.invalid_telemetry.append("av_sync_ms")
                self.metrics["av_sync_ms"] = None
                return
            if contract == "playback":
                presented = event["presented_ns"]
                if type(presented) is not int or presented < completed:
                    self.invalid_telemetry.append("av_sync_ms")
                    self.metrics["av_sync_ms"] = None
                    return
                completed = presented
            if end <= scheduled:
                self.invalid_telemetry.append("av_sync_ms")
                self.metrics["av_sync_ms"] = None
                return
            if scheduled <= self.last_audio_scheduled:
                return
            self.last_audio_scheduled = scheduled
            self.audio_writes += 1
            self._record_progress(("audio_chunks", "audio_writes"), event["completed_ns"])
            self.audio_completions.append((scheduled, end, completed))
        else:
            if scheduled <= self.last_video_scheduled:
                return
            self.last_video_scheduled = scheduled
            self.video_presentations += 1
            self._record_progress(("video_units", "video_presentations"), event["completed_ns"])
            self.video_completions.append((scheduled, completed))
        pending: deque[tuple[int, int]] = deque()
        for stamp, video_done in self.video_completions:
            while self.audio_completions and self.audio_completions[0][1] <= stamp:
                self.audio_completions.popleft()
            match = next(((begin, done) for begin, end, done in self.audio_completions
                          if begin <= stamp < end), None)
            if match is None:
                pending.append((stamp, video_done))
            else:
                # Owner-approved 2026-10-20: A/V sync is the difference between the
                # two streams' schedule-to-completion latencies, so the frame's
                # position inside the audio chunk is not counted as skew.
                audio_begin, audio_done = match
                skew = (video_done - stamp) - (audio_done - audio_begin)
                self._number({"delta": abs(skew) / 1e6}, "delta",
                             "av_sync_ms", peak=True)
                self.av_pairs += 1
                self._record_progress(("av_pairs",), event["completed_ns"])
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
        if envelope in ("video_ipc_send", "video_ipc_receive") and not self.video_measurements.frozen:
            endpoint = IpcEndpoint.parse(event)
            if endpoint is not None:
                measured = self.video_measurements.ipc(
                    "send" if envelope == "video_ipc_send" else "receive", endpoint)
                for target in measured:
                    self._number(measured, target, target)
            else:
                for target in _IPC_KEYS.values():
                    self._number({"invalid": None}, "invalid", target)
        if envelope == "video_decode_completed" and not self.video_measurements.frozen:
            decoded = DecodeEndpoint.parse(event)
            if decoded is not None:
                measured = self.video_measurements.decode(decoded)
                for target in measured:
                    self._number(measured, target, target, integer=target == "decode_frames")
            else:
                for target in _DECODE_KEYS.values():
                    self._number({"invalid": None}, "invalid", target)
        if envelope == "err":
            self.terminal_errors.append({"event": "err", "code": str(event.get("code", "unknown"))})
        # A sink stall (#60) is an audio output gap, never a silent one. Only a
        # resumed sink counts as survived; a failed reopen, a stop during
        # recovery or a bypass still open at the end is unrecovered.
        if envelope == "audio_stall_bypass" and event.get("phase") == "begin":
            self.stall_open = True
        if envelope == "audio_stall_bypass" and event.get("phase") == "end":
            self.stall_open = False
            if event.get("outcome") in _STALL_SURVIVED:
                self.metrics["audio_stall_dropouts"] = int(self.metrics["audio_stall_dropouts"] or 0) + 1
            else:
                self.stall_failed += 1
            lost = sum(event[key] for key in ("dropped", "discarded", "partial")
                       if isinstance(event.get(key), int) and not isinstance(event.get(key), bool))
            self.metrics["audio_stall_dropped_chunks"] = (
                int(self.metrics["audio_stall_dropped_chunks"] or 0) + lost)
        if envelope == "audio_stall_bypass":
            self.metrics["audio_stall_unrecovered"] = self.stall_failed + int(self.stall_open)
        if envelope == "video_sent" and isinstance(event.get("units"), int):
            self.video_units = event["units"]
        if envelope == "audio_chunks" and isinstance(event.get("count"), int):
            self.audio_chunks = event["count"]
        if envelope == "ready":
            self.receiver_ready = True
        if envelope == "active" and isinstance(event.get("startup_ms"), int):
            self.startup_ms = event["startup_ms"]
            self.projection_active = True
        if envelope in ("stopping", "disconnected", "err"):
            self.projection_active = False
        if kind == "decode_stats":
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


def _terminate(proc: subprocess.Popen | None) -> Literal["clean", "forced_kill", "unreaped"]:
    if proc is None or proc.poll() is not None:
        return "clean"
    proc.terminate()
    try:
        proc.wait(timeout=_TERMINATE_GRACE_SECONDS)
    except subprocess.TimeoutExpired:
        proc.kill()
        try:
            proc.wait(timeout=_TERMINATE_GRACE_SECONDS)
        except subprocess.TimeoutExpired:
            return "unreaped"
        return "forced_kill"
    return "clean"


def collect(args: argparse.Namespace) -> dict:
    selected_policy = getattr(args, "thermal_policy", "strict-all-zones")
    hardware = _target_hardware_state() if selected_policy != "strict-all-zones" else None
    thermal_policy = thermal_policy_for_model(selected_policy, hardware.get("model") if hardware else None)
    state_dir = Path(args.state_dir)
    state_dir.mkdir(parents=True, exist_ok=True)
    receiver_log = state_dir / "receiver.log"
    display_log = state_dir / "display.log"

    startup_timeout = getattr(args, "startup_timeout_seconds", _PROJECTION_STARTUP_TIMEOUT_SECONDS)
    managed_display = bool(args.display and not args.no_display)
    video_socket = getattr(args, "video_socket", None)
    input_socket = getattr(args, "input_socket", None)
    receiver_cmd = [args.receiver, "--port", args.port, "--duration-ms",
                    str((args.duration_seconds + startup_timeout + 10) * 1000)]
    if args.store:
        receiver_cmd += ["--store", args.store]
    if video_socket:
        receiver_cmd += ["--video-socket", video_socket]
    if input_socket:
        receiver_cmd += ["--input-socket", input_socket]
    if managed_display:
        # Contract: --require-video-consumer makes the receiver emit "ready"
        # after listeners/control init and BEFORE version negotiation.
        receiver_cmd += ["--require-video-consumer"]

    samples: list[Sample] = []
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
    origin_ns = time.monotonic_ns()
    start = origin_ns / 1e9
    clk = os.sysconf("SC_CLK_TCK")
    inventory: tuple[ThermalZone, ...] = ()
    video_evidence = VideoEvidence((), (), ())
    observed_duration = 0.0
    deadline = start + startup_timeout

    def read_sample() -> Sample:
        nonlocal inventory
        parser.poll(str(receiver_log))
        if display is not None:
            parser.poll(str(display_log))
        observed = _thermal_inventory() if thermal_policy is not None else None
        if observed is not None and inventory and set(observed) != {zone.zone for zone in inventory}:
            parser.invalid_telemetry.append("thermal_inventory_changed")
        zones = (_thermal_zones(inventory, observed) if observed is not None
                 else _thermal_zones(inventory))
        assert receiver is not None
        return Sample(
            t_ns=time.monotonic_ns() - origin_ns,
            receiver_rss_mb=_rss_mb(receiver.pid),
            receiver_cpu_ticks=_cpu_ticks(receiver.pid),
            thermal_zones=zones,
            projection_active=parser.projection_active,
            video_units=parser.video_units, audio_chunks=parser.audio_chunks,
            video_presentations=parser.video_presentations,
            audio_writes=parser.audio_writes, av_pairs=parser.av_pairs,
        )

    try:
        rec_file = open(receiver_log, "w", encoding="utf-8")
        receiver = subprocess.Popen(receiver_cmd, stdout=rec_file, stderr=subprocess.STDOUT,
                                    start_new_session=True)
        if managed_display:
            # Bounded receiver-ready observation: the managed display launches on
            # THIS receiver's "ready" event, never on a fixed sleep. The wait is
            # bounded by the one absolute startup deadline above, measured from
            # the pre-launch origin and never refreshed by readiness.
            while True:
                parser.poll(str(receiver_log))
                now = time.monotonic_ns() / 1e9
                if receiver.poll() is not None:
                    early_exit = True
                    break
                if parser.terminal_errors:
                    break
                if now >= deadline or parser.receiver_ready:
                    break
                time.sleep(min(args.interval, 5.0, max(0.0, deadline - now)))
            if (parser.receiver_ready and now < deadline
                    and not early_exit and not parser.terminal_errors):
                disp_file = open(display_log, "w", encoding="utf-8")
                display_cmd = [args.display, "--test-only-autostart"]
                if video_socket:
                    display_cmd += ["--video-socket", video_socket]
                if input_socket:
                    display_cmd += ["--input-socket", input_socket]
                if time.monotonic_ns() / 1e9 < deadline:
                    display = subprocess.Popen(display_cmd, stdout=disp_file, stderr=subprocess.STDOUT,
                                               start_new_session=True)

        while True:
            now = time.monotonic_ns() / 1e9
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
            sample = read_sample()
            logged_progress = None
            if (progress.started_ns is not None
                    and sample.t_ns - progress.last_ns > _MEDIA_STALL_SECONDS * 1_000_000_000):
                # The collector itself was frozen. The receiver's log may not have
                # reached disk yet, so do not judge the first post-gap read: re-poll
                # for a bounded grace, stopping early once the logged events show
                # every stream flowing, then decide from the receiver's own clock.
                grace_end_ns = sample.t_ns + int(_COLLECTOR_LOG_GRACE_SECONDS * 1_000_000_000)
                while True:
                    logged_progress = parser.logged_progress(origin_ns, progress.last_ns,
                                                             sample.t_ns)
                    if progress.covers_gap(sample, logged_progress) or sample.t_ns >= grace_end_ns:
                        break
                    time.sleep(_COLLECTOR_LOG_POLL_SECONDS)
                    sample = read_sample()
            samples.append(sample)
            was_started = progress.started_ns is not None
            progress.observe(sample, logged_progress)
            parser.progress_sampled(sample.t_ns, origin_ns)
            if not was_started and progress.started_ns is not None:
                inventory = sample.thermal_zones
                parser.video_measurements.start_window(origin_ns, progress.started_ns)
                deadline = start + progress.started_ns / 1e9 + args.duration_seconds
            elif was_started and progress.last_ns == sample.t_ns:
                parser.video_measurements.advance(sample.t_ns)
            if progress.failure:
                break

        video_evidence = parser.video_measurements.finish(progress.last_ns)
        events = {key: value for key, value in parser.metrics.items()
                  if key not in {*_DECODE_KEYS.values(), *_IPC_KEYS.values()}}
        events["helper_rss_mb"] = None
        events["video_units"] = parser.video_units
        events["audio_chunks"] = parser.audio_chunks
        events["startup_ms"] = parser.startup_ms
        invalid_telemetry = sorted(set(parser.invalid_telemetry) | set(parser.video_measurements.issues))
        video_telemetry = {
            "ipc_matched": parser.video_measurements.ipc_matched,
            "ipc_consumed_bytes": parser.video_measurements.receive_bytes,
            "ipc_matched_samples_retained": len(parser.video_measurements.ipc_samples),
            "ipc_unmatched_or_expired": parser.video_measurements.unmatched,
            "ipc_pending_send": len(parser.video_measurements.sends),
            "ipc_pending_receive": len(parser.video_measurements.receives),
            "decode_samples_retained": len(parser.video_measurements.decode_samples),
        }
        observed_duration = (time.monotonic_ns() - origin_ns) / 1e9
        if display is not None and display_survived is None:
            display_survived = display.poll() is None
        if receiver.poll() is None:
            try:
                receiver.wait(timeout=min(args.interval, _TERMINATE_GRACE_SECONDS))
            except subprocess.TimeoutExpired:
                stopped_by_us = True
    except FileNotFoundError as exc:
        raise SystemExit(f"error: cannot launch receiver: {exc}") from exc
    finally:
        parser.video_measurements.frozen = True
        if display is not None and display.poll() is not None:
            display_survived = False
        for name, proc in (("display", display), ("receiver", receiver)):
            cleanup = _terminate(proc)
            if cleanup != "clean":
                parser.terminal_errors.append({"event": "err", "code": name + "_cleanup_" + cleanup})
        if rec_file is not None:
            rec_file.close()
        if disp_file is not None:
            disp_file.close()
        parser.poll(str(receiver_log))
        if display is not None:
            parser.poll(str(display_log))

    if receiver is not None:
        receiver_exit = receiver.returncode
    if not stopped_by_us and receiver_exit is not None and receiver_exit != 0:
        crashes = 1
    if not stopped_by_us and not early_exit and receiver_exit is None:
        crashes = 1

    capture = {
        "schema": CAPTURE_SCHEMA,
        "mode": "wired",
        "duration_seconds": args.duration_seconds,
        "observed_duration_seconds": observed_duration,
        "active_projection_seconds": progress.seconds,
        "startup_wait_seconds": progress.started_ns / 1e9 if progress.started_ns is not None else None,
        "projection_failure": progress.failure,
        "interference": list(progress.interferences),
        "measurement_origin": "live",
        "crashes": crashes,
        "early_exit": early_exit,
        "receiver_exit_code": receiver_exit,
        "receiver_stopped_by_collector": stopped_by_us,
        "display_survived": display_survived,
        "display_connected": display_connected,
        "terminal_errors": parser.terminal_errors,
        "invalid_telemetry": invalid_telemetry,
        "monotonic_origin_ns": origin_ns,
        "cpu_ticks_per_second": clk,
        "samples": [asdict(sample) for sample in samples],
        "video_evidence": asdict(video_evidence),
        "events": events,
        "video_telemetry": video_telemetry,
        "target_hardware_state": hardware if hardware is not None else _target_hardware_state(),
    }
    if thermal_policy is not None:
        capture["thermal_policy"] = asdict(thermal_policy)
    return capture


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
    parser.add_argument("--thermal-policy", choices=("strict-all-zones", NANO_PROFILE),
                        default="strict-all-zones", help="explicit approved Nano CV availability exception")
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
    except CoverageError as exc:
        print(f"contract violation: {exc}", file=sys.stderr)
        return 4
    finally:
        signal.signal(signal.SIGTERM, old_handler)

    json.dump(capture, sys.stdout, indent=2, sort_keys=True, allow_nan=False)
    sys.stdout.write("\n")
    return 0


if __name__ == "__main__":
    sys.exit(_cli(sys.argv[1:]))
