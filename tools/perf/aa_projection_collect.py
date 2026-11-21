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

  {"schema":"aa-projection-perf-capture/3","mode":"wired",
   "duration_seconds":N,"crashes":0,"samples":[...],"events":{...},
   "av_series":{...},"workload":{...},"target_hardware_state":{...}}

Capture /3 (signed A/V contract, #137) adds ``av_series``: every A/V pair's
paint-completion time and signed skew, and the merged union of valid audio
slots' scheduled intervals, all in ns relative to ``monotonic_origin_ns`` and
frozen at the measurement cutoff, so the reporter judges the four clauses over
its own replayed active window (tools/perf/aa_av_contract.py).

With ``--workload moving-map`` (the soak workload, #137) the collector prepares
the phone (``adb -s --phone-serial``: USB reset, media, then Google Maps),
gives the managed display its test control channel, and drags the map and
keeps the media playing for the whole stage (tools/perf/aa_moving_map.py).
A required phone command that fails during the setup refuses the stage before
the receiver starts (``error: phone_setup_failed:``, BLOCKED by the launcher);
one that fails later ends the workload and is recorded in
``workload.phone_failure``. The capture also records every completed drag's
time (``workload.drag_t_ns``) for the reporter's continuity check, and the
build/compiler processes seen on this host during the stage
(``concurrent_builds``), which disqualify it (#137).

With ``--run-file PATH`` the collector records, for a remote stop
(tools/perf/aa_remote_stop.sh), its own PID and the receiver's and display's,
each with its /proc start time, plus the phone serial and media package. It
removes the file only after its own cleanup, phone restore included, so the
file's removal is the stop's acknowledgement. With a run file the collector
also leads its own process group (recorded as ``group PGID SID``): its adb
commands share it, so a forced stop can kill them with it before it restores
the phone, without touching the ssh session's shell.

The capture is the raw record; budget verdicts are produced by
``aa_perf_report.py``. A metric the collector cannot observe is emitted as
``null`` -- never faked. Every subprocess wait is bounded.

A live qualification stage is refused before anything is launched unless the
build directory that produced the binaries proves an optimized build (see
``aa_build_guard.py``): a Debug build delays GUI-thread paint and the display
pump enough to fail the A/V gate on its own. ``--allow-unoptimized-build`` is a
debugging override only.

With ``--transport wireless`` (default wired) the collector launches
``aa-wireless-receiver --store-dir DIR --phone-id N --diagnostics`` instead:
the receiver brings the session up through the system helper's AP and
Bluetooth bootstrap when the display asks to project and releases them when it
stops (tools/perf/aa_transport.py). The phone's cable must be out: this host's
USB bus must show neither the phone (``--phone-serial``) nor an AOA device
before (else the stage is refused before launch), during and after the stage
(else BLOCKED ``cleanup:usb_attached``). adb runs over the AP link: once the
session is active the collector takes the peer of the receiver's own TCP
session (the only one, and a station of the AP), connects to it on 5555 and
binds it only when the device's ``getprop ro.serialno`` is ``--phone-serial``;
otherwise the stage is refused (``phone_setup_failed: adb_session_*`` /
``adb_unreachable`` / ``adb_phone_mismatch``, nothing measured). Only then is the phone prepared and
the measurement window allowed to start (samples count as active only after
it), and the phone is restored before the session is torn down. The wireless
receiver logs no ``active`` line; its ``session_state_changed`` diagnostic to
``active`` (bare on main, wrapped after #163) is the marker, ``startup_ms`` is
measured by the harness from launch to it (``startup_basis``), and a
re-admission (a second ``wireless_admission``, ``wireless_admission_expired``,
a new diagnostic session id, or reconnecting/disconnected/failed) ends the
active state. The capture's ``mode`` is the transport.

This tool does not modify target state (no installs, no udev, no kernel
changes); it only launches the supplied binaries, reads procfs/sysfs and, with
the moving-map workload, sends adb intents to the named phone.
"""
from __future__ import annotations

# allow: SIZE_OK - preserve the existing collector lifecycle and parser in the frozen owned file set.

import argparse
from collections import deque
from collections.abc import Iterable, Mapping
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
from array import array
from pathlib import Path
from typing import Literal

import aa_log_integrity
from aa_video_measurements import DecodeEndpoint, IpcEndpoint, VideoMeasurements
import aa_moving_map as moving_map
import aa_transport as transport
from aa_measurement_coverage import (
    CoverageError, NANO_PROFILE, Sample, ThermalZone, VideoEvidence, thermal_policy_for_model,
)
from aa_build_guard import (
    BINARY_REFUSAL_PREFIX, BuildDirMismatchError, BuildGuardError, REFUSAL_PREFIX,
    describe_build, require_optimized_build, require_optimized_type, verify_launched_binaries,
)

CAPTURE_SCHEMA = "aa-projection-perf-capture/3"
# Retention bound of the signed A/V series (a 7200 s stage at 60 fps is 432,000
# pairs); past it the series is incomplete and A/V is invalid, never truncated.
_AV_SERIES_LIMIT = 1_000_000
_TERMINATE_GRACE_SECONDS = 5.0
_PROJECTION_STARTUP_TIMEOUT_SECONDS = 60
# Derived wireless startup floor from aa_transport.WIRELESS_STARTUP_BUDGET_S:
# - tools/wireless-receiver/Runtime.hpp:228 (15 s per admission attempt)
# - tools/wireless-receiver/Runtime.hpp:35,207,242 (250 ms initial backoff, 5 s cap)
# - tools/wireless-receiver/Runtime.hpp:251 (attempt held while bootstrapping)
# - include/aa/app/HandoffEligibility.hpp:45,47 (40 s bootstrap #170 + 5 s connect)
WIRELESS_STARTUP_BUDGET_S = transport.WIRELESS_STARTUP_BUDGET_S
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
# Receiver err code for a link from another phone than --phone-serial, and the
# stderr marker the launcher maps to BLOCKED phone_identity_mismatch.
PHONE_MISMATCH_CODE = "phone_serial_mismatch"
PHONE_REFUSAL_PREFIX = "error: phone_identity_mismatch:"
# stderr marker the launcher maps to BLOCKED phone_setup_failed.
PHONE_SETUP_PREFIX = "error: phone_setup_failed:"
# Compiler/build processes (exact /proc comm names) whose presence during a
# stage disqualifies it; the soak chain's quiet gate uses the same list.
BUILD_NAMES = ("cmake", "ninja", "make", "c++", "g++", "gcc", "cc1plus", "ctest", "nvcc", "ld")
_BUILD_RECORDS_LIMIT = 256


class RunFile:
    """The remote-stop record (``--run-file``): one ``ROLE VALUE [START]`` line each.

    Created exclusively (0600, never through an existing path); removed by the
    collector after its cleanup and phone restore, which acknowledges a stop.
    """

    def __init__(self, path: str) -> None:
        self.path = Path(path)
        fd = os.open(self.path, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600)
        self._handle = os.fdopen(fd, "w", encoding="utf-8")

    def process(self, role: str, pid: int) -> None:
        self.line(role, str(pid), str(proc_starttime(pid)))

    def line(self, *fields: str) -> None:
        if any(not field or any(c.isspace() for c in field) for field in fields):
            return  # nothing a stop could misparse
        self._handle.write(" ".join(fields) + "\n")
        self._handle.flush()
        os.fsync(self._handle.fileno())

    def remove(self, cleanup_failures: list[str] | None = None) -> None:
        """Remove the run file (the stop's acknowledgement). With a moving-map
        workload, first write ``PATH.cleanup``: one line per restore step that
        could not be done or verified (empty: a clean restore)."""
        self._handle.close()
        if cleanup_failures is not None:
            try:
                fd = os.open(f"{self.path}.cleanup",
                             os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600)
                with os.fdopen(fd, "w", encoding="utf-8") as report:
                    report.writelines(" ".join(item.split()) + "\n" for item in cleanup_failures)
            except OSError as exc:
                print(f"cleanup failure: cannot write the cleanup report: {exc}", file=sys.stderr)
        try:
            self.path.unlink()
        except FileNotFoundError:
            pass


def proc_starttime(pid: int) -> int | None:
    """A process's start time (clock ticks since boot, /proc/PID/stat field 22)."""
    try:
        return int(Path(f"/proc/{pid}/stat").read_text().rsplit(")", 1)[1].split()[19])
    except (OSError, ValueError, IndexError):
        return None


def _record_process(args: argparse.Namespace, role: str, pid: int) -> None:
    run_file = getattr(args, "run_file_record", None)
    if run_file is not None:
        run_file.process(role, pid)


_ADB_BIND_TIMEOUT_SECONDS = 20.0
# Phone setup after the bind (snapshots, wake, media, Maps) plus one sampling
# interval before the window starts, with margin.
_WIRELESS_PREPARE_HEADROOM_SECONDS = 120
_RECEIVER_MAX_LIFETIME_MS = 86_400_000  # aa-wireless-receiver's --duration-ms limit
_HOST_USB_PERIOD_SECONDS = 10.0


def _host_adb(*args: str) -> subprocess.CompletedProcess:
    """Host-level ``adb`` (connect/disconnect over the AP link), bounded."""
    # The same hard bound (TERM, KILL, abandoned pipes) as every phone command:
    # `adb connect` may start the adb server, which would hold a captured pipe.
    return moving_map.run_adb(["adb", *args])


class PhoneSetupError(RuntimeError):
    """A required phone command failed before the stage started."""


def ancestors(pid: int | None = None) -> set[int]:
    """This process and its ancestors (a stage launched from a build tool is not
    competing with itself)."""
    seen: set[int] = set()
    pid = os.getpid() if pid is None else pid
    while pid > 1 and pid not in seen:
        seen.add(pid)
        try:
            stat = Path(f"/proc/{pid}/stat").read_text()
            pid = int(stat.rsplit(")", 1)[1].split()[1])
        except (OSError, ValueError, IndexError):
            break
    return seen


def build_processes(exclude: Iterable[int] = ()) -> list[str]:
    """Running compiler/build processes ("PID COMM"), skipping ``exclude``."""
    skip = set(exclude)
    found = []
    for path in Path("/proc").glob("[0-9]*/comm"):
        try:
            if int(path.parent.name) in skip:
                continue
            name = path.read_text().strip()
        except (OSError, ValueError):
            continue
        if name in BUILD_NAMES:
            found.append(f"{path.parent.name} {name}")
    return sorted(found, key=lambda entry: int(entry.split()[0]))
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
        # Streams whose advanced_ns is the receiver's last logged event time (set when a
        # long collector gap was judged from the log), not a collector sample time.
        self.event_anchored: set[str] = set()
        self.failure: str | None = None
        self.interferences: list[dict] = []
        self.progress_evidence: list[dict] = []

    @classmethod
    def replay(cls, samples: Iterable[Sample],
               logged_progress_by_ns: Mapping[int, dict[str, dict]] | None = None) -> ProjectionProgress:
        """Replay captured samples with persisted receiver-log evidence using observe()."""
        progress = cls()
        evidence = logged_progress_by_ns or {}
        for sample in samples:
            progress.observe(sample, evidence.get(sample.t_ns))
        return progress

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
        sample (see EventParser.logged_progress). It is consulted when the
        collector's sampling gap exceeds _MEDIA_STALL_SECONDS or when a stream's
        ``advanced_ns`` is anchored to an earlier logged event after such a gap.
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
            used_logged_progress = gap_judged or (
                logged_progress is not None
                and any(key in self.event_anchored and value > self.previous[key]
                        for key, value in counters.items())
            )
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
                        self.event_anchored.add(key)
                    else:
                        self.failure = "media_stalled:" + key
                    continue
                if (key in self.event_anchored and logged_progress is not None
                        and value > self.previous[key]
                        and self._continuous(key, value, stamp, logged_progress)):
                    # Rule: when leaving event anchoring, re-anchor advanced_ns[key] to
                    # this collector sample time (stamp), never to an older receiver-event
                    # last_event_ns. Receiver-clock continuity confirmed no silence over
                    # _MEDIA_STALL_SECONDS from the old event anchor through stamp and the
                    # counter advanced, so subsequent collector-time age checks must measure
                    # from stamp rather than comparing collector time against event time.
                    self.advanced_ns[key] = stamp
                    self.event_anchored.discard(key)
                    continue
                # Check age before accepting a resumed stream: a late recovery cannot heal a freeze.
                if stamp - self.advanced_ns[key] > _MEDIA_STALL_SECONDS * 1_000_000_000:
                    self.failure = "media_stalled:" + key
                if value > self.previous[key]:
                    self.advanced_ns[key] = stamp
                    self.event_anchored.discard(key)
            if used_logged_progress:
                self.progress_evidence.append({
                    "t_ns": stamp,
                    "logged_progress": logged_progress,
                })
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
        # Log-integrity evidence per log path, kept after the measurement freeze:
        # validity is decided after the final read (#90 review).
        self.integrity: dict[str, list[dict]] = {}
        # Source-time end of the measured window (absolute CLOCK_MONOTONIC ns),
        # set when the sampling loop ends (#90 review 4). A metric record read
        # after it is admitted at its own emission time when that falls inside
        # the window, ignored when after it, and invalidates the metric it
        # feeds when its time is unknown or it can no longer be admitted.
        self.cutoff_ns: int | None = None
        self.late_targets: set[str] = set()
        # Peak/cumulative values of in-window records read after the cutoff,
        # merged into the frozen metrics at the end (never into live state).
        self.late_values: dict[str, float | int] = {}
        self._source: str | None = None
        # Wireless: the receiver's launch on the monotonic clock, for startup_ms
        # from its session_state_changed -> active diagnostic (mono_ns).
        self.receiver_started_ns: int | None = None
        # Wireless only: the diag active marker and re-admission rules apply.
        self.wireless = False
        self.session_id: object = None
        self.admissions = 0
        self.readmitted = False  # an admission (or its expiry) after the first active session
        self.media_telemetry_dropped = 0
        self.metrics: dict[str, object] = {
            "frame_drops": None, "decode_p50_ms": None, "decode_p95_ms": None,
            "decode_fps": None, "decode_frames": None, "ipc_throughput_mbps": None,
            "ipc_p95_added_latency_ms": None, "av_sync_ms": None, "audio_latency_ms": None,
            "audio_scheduling_skew_ms": None, "audio_stall_dropouts": 0,
            "audio_stall_dropped_chunks": 0, "audio_stall_unrecovered": 0,
            "audio_catch_up_events": 0, "audio_catch_up_frames": 0, "audio_catch_up_ms": 0.0,
        }
        # Catch-up loss (#154), from the receiver's running lifetime totals:
        # (events, frames, rate) of the newest record, None once malformed.
        self.catch_up: tuple[int, int, int] | None = (0, 0, 0)
        # A media_telemetry_dropped notice after the newest catch-up record
        # may hide a later one: the totals are then unconfirmed (null).
        self.catch_up_unconfirmed = False
        # Every such doubt (a lossy notice, a malformed notice or a truncated
        # catch-up or notice line) ever read: the collector compares it
        # across its final poll, after the metrics froze.
        self.catch_up_doubts = 0
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
        # Signed A/V series (capture /3): absolute paint-completion ns and
        # signed skew ns per pair, and the merged union of valid audio slots.
        self.av_pair_times = array("q")
        self.av_pair_skews = array("q")
        self.audio_union: list[list[int]] = []
        self.audio_writes = 0
        self.playback_slot_absences: dict[str, int] = {}
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

    def _catch_up(self, event: dict) -> None:
        """Account one ``audio_catch_up`` event (#154): router audio dropped at once.

        A placement backlog past the catch-up watermark is trimmed to the drift
        target in one run instead of 5 ms per run. It is neither a sink stall
        nor an ``audio_stall_bypass``, so it never feeds the ``audio_stall_*``
        counters. Each record carries the render worker's running totals over
        the receiver's life (``events_lifetime``, ``frames_lifetime``: output
        frames at ``rate`` beyond the ordinary bounded drift trim), which
        survive a sink reopen and restore any earlier record the droppable
        media log lost. Frames and the event count stay separate units.
        """
        if self.catch_up is None:
            return
        fields = [event.get(key) for key in ("frames", "rate", "events_lifetime", "frames_lifetime")]
        frames, rate, events, total = fields
        last_events, last_total, last_rate = self.catch_up
        if not (all(isinstance(value, int) and not isinstance(value, bool) and value > 0
                    for value in fields)
                and total >= frames and events > last_events and total > last_total
                and last_rate in (0, rate)):
            self.catch_up = None
            self._publish_catch_up()
            return
        self.catch_up = (events, total, rate)
        self.catch_up_unconfirmed = False
        self._publish_catch_up()

    def _media_dropped(self, event: dict) -> None:
        counters = [event.get(key) for key in ("rate_limited", "backpressure")]
        if (not all(isinstance(value, int) and not isinstance(value, bool) and value >= 0
                    for value in counters)
                or sum(counters) > 0):
            self._doubt_catch_up()

    def _doubt_catch_up(self) -> None:
        """A catch-up record may be missing: unconfirmed until a later one."""
        self.catch_up_doubts += 1
        self.catch_up_unconfirmed = True
        self._publish_catch_up()

    def _publish_catch_up(self) -> None:
        keys = ("audio_catch_up_events", "audio_catch_up_frames", "audio_catch_up_ms")
        if self.catch_up is None or self.catch_up_unconfirmed:
            for key in keys:
                self.metrics[key] = None
            return
        events, total, rate = self.catch_up
        self.metrics.update(zip(keys, (events, total, round(total * 1000 / rate, 6) if rate else 0.0)))

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
            # Additive failure diagnostics; null presented_ns below still
            # invalidates A/V regardless of the reason (including unknown
            # future reasons). Older captures need no new field.
            reason = event.get("playback_slot_absence")
            if event.get("presented_ns", 0) is None and isinstance(reason, str):
                self.playback_slot_absences[reason] = self.playback_slot_absences.get(reason, 0) + 1
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
            if self.audio_union and scheduled <= self.audio_union[-1][1]:
                self.audio_union[-1][1] = max(self.audio_union[-1][1], end)
            else:
                self.audio_union.append([scheduled, end])
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
                if len(self.av_pair_times) < _AV_SERIES_LIMIT:
                    self.av_pair_times.append(video_done)
                    self.av_pair_skews.append(skew)
                elif "av_sync_ms" not in self.invalid_telemetry:
                    self.invalid_telemetry.append("av_sync_ms")
                self._record_progress(("av_pairs",), event["completed_ns"])
        self.video_completions = pending
        # Bound retention even if a stream disappears. Missing pairs never become zero.
        while len(self.audio_completions) > 16384:
            self.audio_completions.popleft()
        if len(self.video_completions) > 16384:
            self.video_completions.popleft()
            self.invalid_telemetry.append("av_sync_ms")
            self.metrics["av_sync_ms"] = None

    def av_series(self, origin_ns: int) -> dict:
        """The signed A/V series relative to ``origin_ns`` (capture /3)."""
        return {"pair_t_ns": [t - origin_ns for t in self.av_pair_times],
                "pair_skew_ns": list(self.av_pair_skews),
                "audio_intervals_ns": [[lo - origin_ns, hi - origin_ns]
                                       for lo, hi in self.audio_union]}

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

    def poll(self, path: str, final: bool = False) -> None:
        """Consume the log's new complete lines.

        A line still being written is left for the next poll, so a record is
        never split into two unparseable halves. ``final`` (after the writer
        exited) consumes the unterminated tail too: a record cut off there is
        truncated for good.
        """
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
            with open(path, "rb") as handle:
                handle.seek(offset)
                data = handle.read()
        except OSError:
            return
        # Only complete lines are consumed while the writer runs; an
        # unterminated tail stays unread (offset not advanced past it) until
        # its newline arrives. On `final` (after exit), consume any
        # unterminated tail too so a cut-off record is noticed.
        end = len(data) if final else data.rfind(b"\n") + 1
        self._source = path
        try:
            for line in data[:end].decode("utf-8", errors="replace").split("\n"):
                self._consume(line)
        finally:
            self._source = None
        self.offsets[path] = offset + end

    def integrity_issues(self, receiver_log: str, display_log: str | None) -> list[str]:
        """Fail-closed capture validity from every line read so far (#90 review)."""
        receiver = aa_log_integrity.assess_receiver(self.integrity.get(receiver_log, []))
        issues = []
        if not receiver["complete"]:
            issues.append("receiver_log_incomplete")
        else:
            if receiver["state_lost"]:
                issues.append("state_log_dropped")
            if receiver["media_lost"]:
                issues.append("media_telemetry_dropped")
        if display_log is not None and not aa_log_integrity.assess_display(
                self.integrity.get(display_log, []))["complete"]:
            issues.append("display_log_incomplete")
        return issues

    _METRIC_KEYS = {"audio_scheduling_skew_ms": "audio_scheduling_skew_ms",
                    "audio_latency_ms": "audio_latency_ms", "frame_drops": "frame_drops",
                    "av_sync_ms": "av_sync_ms"}

    def _metric_targets(self, event: dict, envelope: object, kind: object) -> set[str]:
        targets = {target for key, target in self._METRIC_KEYS.items() if key in event}
        if envelope in ("video_ipc_send", "video_ipc_receive"):
            targets |= set(_IPC_KEYS.values())
        if envelope == "video_decode_completed":
            targets |= set(_DECODE_KEYS.values())
        if envelope in ("audio_write_completed", "video_presented"):
            targets.add("av_sync_ms")
        if kind == "decode_stats":
            targets.add("frame_drops")
        if envelope == "audio_stall_bypass":
            targets |= {"audio_stall_dropouts", "audio_stall_dropped_chunks", "audio_stall_unrecovered"}
        return targets

    def _admit_after_cutoff(self, event: dict, envelope: object, kind: object) -> bool:
        """Handle a record read after the window closed; True for a non-metric record.

        A metric record is never fed into the live parser state after the
        cutoff (the exported metrics stay frozen). One emitted inside the
        window is counted at its emission time: its peak and cumulative values
        are kept in ``late_values`` and merged into the frozen export. When its
        emission time is unknown, or its metric cannot be merged (the A/V
        pairing, video summaries, stall bookkeeping), the metric is invalid.
        """
        targets = self._metric_targets(event, envelope, kind)
        if not targets:
            return True
        assert self.cutoff_ns is not None
        stamp = aa_log_integrity.emit_ns(event)
        if stamp is None and envelope in ("video_ipc_send", "video_ipc_receive",
                                          "video_decode_completed", "video_presented"):
            # Video endpoints carry their own monotonic measurement times; the
            # display's video_presented (ProjectionWidget) has no emit_ns.
            value = event.get("observed_ns" if envelope in ("video_ipc_send", "video_ipc_receive")
                              else "completed_ns")
            if type(value) is int and value > 0:
                stamp = value
        if stamp is None:
            self.late_targets |= targets  # unknown source time: inclusion is uncertain
            return False
        if stamp > self.cutoff_ns:
            return False  # emitted after the window: not part of the measurement
        mergeable = {"audio_scheduling_skew_ms": "audio_scheduling_skew_ms",
                     "audio_latency_ms": "audio_latency_ms", "frame_drops": "frame_drops"}
        if kind == "decode_stats":
            for key in ("dropped", "drops"):
                if key in event:
                    self._late_value(event[key], "frame_drops", integer=True)
        for key, target in mergeable.items():
            if key in event:
                self._late_value(event[key], target, integer=target == "frame_drops")
        self.late_targets |= targets - set(mergeable.values())
        return False

    def _late_value(self, raw: object, target: str, *, integer: bool = False) -> None:
        try:
            valid = (not isinstance(raw, bool) and isinstance(raw, (int, float))
                     and math.isfinite(raw) and (not integer or isinstance(raw, int)))
        except OverflowError:
            valid = False
        if not valid:
            self.late_targets.add(target)
            return
        assert isinstance(raw, (int, float))
        prior = self.late_values.get(target)
        self.late_values[target] = raw if prior is None else max(prior, raw)

    def _wireless(self, event: dict, envelope: object) -> None:
        """The wireless receiver's session markers (tools/perf/aa_transport.py)."""
        if envelope == "media_telemetry_dropped":
            self.media_telemetry_dropped += 1
        # Admission retries before the first active session are startup (the
        # receiver gives each attempt 15 s); only after it are they a drop.
        if envelope == "wireless_admission":
            self.admissions += 1
            if self.session_id is not None:
                self.readmitted = True
                self.projection_active = False  # a re-admission: the link was lost
        if envelope == "wireless_admission_expired" and self.session_id is not None:
            self.readmitted = True
            self.projection_active = False
        if transport.diag_kind(event) != "session_state_changed":
            return
        state, mono = event.get("to_state"), event.get("mono_ns")
        session = event.get("session")
        if self.session_id is not None and session != self.session_id:
            self.projection_active = False  # a new session after the first one
        if state == "active" and self.session_id is None:
            self.session_id = session
            if type(mono) is int and self.receiver_started_ns is not None and self.startup_ms is None:
                self.startup_ms = max(0, (mono - self.receiver_started_ns) // 10**6)
            if not self.readmitted:
                self.projection_active = True
        elif state in ("disconnected", "reconnecting", "failed", "stopping"):
            self.projection_active = False

    def _consume(self, line: str) -> None:
        line = line.strip()
        if not line.startswith("{"):
            return
        try:
            event = json.loads(line)
        except json.JSONDecodeError:
            # A cut-off catch-up record or drop notice hides what it carried.
            if '"audio_catch_up"' in line or '"media_telemetry_dropped"' in line:
                self._doubt_catch_up()
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
        if self._source is not None:
            records = self.integrity.setdefault(self._source, [])
            if envelope in aa_log_integrity.RELEVANT_EVENTS:
                records.append(event)
            elif any(r.get("event") in ("log_complete", "telemetry_complete") for r in records[-1:]):
                # A record after the completion record: the log was not proven complete.
                records.append({"event": aa_log_integrity.AFTER_COMPLETION})
        if self.cutoff_ns is not None and not self._admit_after_cutoff(event, envelope, kind):
            return
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
        if envelope == "audio_catch_up":
            self._catch_up(event)
        if envelope == "media_telemetry_dropped":
            self._media_dropped(event)
        if envelope == "video_sent" and isinstance(event.get("units"), int):
            self.video_units = event["units"]
        if envelope == "audio_chunks" and isinstance(event.get("count"), int):
            self.audio_chunks = event["count"]
        if envelope == "ready":
            self.receiver_ready = True
        if envelope == "active" and isinstance(event.get("startup_ms"), int):
            self.startup_ms = event["startup_ms"]
            self.projection_active = True
        if self.wireless:
            self._wireless(event, envelope)
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


def build_record(args: argparse.Namespace, build_type: str | None = None,
                 verified_on: str | None = None,
                 binaries: Mapping[str, dict] | None = None) -> dict:
    """The capture's ``build`` block: what was measured, and how it was verified."""
    build_dir = getattr(args, "build_dir", None)
    record = {
        "build_type": build_type,
        "build_dir": str(build_dir) if build_dir else None,
        "allow_unoptimized_build": bool(getattr(args, "allow_unoptimized_build", False)),
        "verified_on": verified_on if verified_on is not None else
        ("target" if build_dir else "none"),
    }
    if binaries:
        record["binaries"] = binaries
    return record


def startup_timeout_seconds(transport_name: str, override_s: int | None = None) -> int:
    """Resolve and validate the startup deadline: wired stays <= 60 s; wireless >= derived floor."""
    if transport_name == "wireless":
        if override_s is None:
            return WIRELESS_STARTUP_BUDGET_S
        if (type(override_s) is not int or override_s < WIRELESS_STARTUP_BUDGET_S
                or override_s > _RECEIVER_MAX_LIFETIME_MS // 1000):
            raise ValueError(
                f"--startup-timeout-seconds for wireless must be in "
                f"[{WIRELESS_STARTUP_BUDGET_S}, {_RECEIVER_MAX_LIFETIME_MS // 1000}]"
            )
        return override_s
    if override_s is None:
        return _PROJECTION_STARTUP_TIMEOUT_SECONDS
    if type(override_s) is not int or not 0 < override_s <= _PROJECTION_STARTUP_TIMEOUT_SECONDS:
        raise ValueError(
            f"--startup-timeout-seconds must be in [1, {_PROJECTION_STARTUP_TIMEOUT_SECONDS}]"
        )
    return override_s


def collect(args: argparse.Namespace, build: dict | None = None) -> dict:
    selected_policy = getattr(args, "thermal_policy", "strict-all-zones")
    hardware = _target_hardware_state() if selected_policy != "strict-all-zones" else None
    thermal_policy = thermal_policy_for_model(selected_policy, hardware.get("model") if hardware else None)
    state_dir = Path(args.state_dir)
    state_dir.mkdir(parents=True, exist_ok=True)
    receiver_log = state_dir / "receiver.log"
    display_log = state_dir / "display.log"

    wireless = getattr(args, "transport", transport.DEFAULT_TRANSPORT) == "wireless"
    raw_startup = getattr(args, "startup_timeout_seconds", None)
    startup_timeout = (
        raw_startup if raw_startup is not None
        else (WIRELESS_STARTUP_BUDGET_S if wireless else _PROJECTION_STARTUP_TIMEOUT_SECONDS)
    )
    managed_display = bool(args.display and not args.no_display)
    video_socket = getattr(args, "video_socket", None)
    input_socket = getattr(args, "input_socket", None)
    lifetime_s = args.duration_seconds + startup_timeout + 10
    if wireless:
        # Wireless binds adb and prepares the phone after the receiver is up
        # (wired does it before launch): the receiver must outlive that too, or
        # its own --duration-ms timer would end a healthy stage early.
        lifetime_s += _ADB_BIND_TIMEOUT_SECONDS + _WIRELESS_PREPARE_HEADROOM_SECONDS
    lifetime_ms = str(int(min(lifetime_s * 1000, _RECEIVER_MAX_LIFETIME_MS)))
    if wireless:
        receiver_cmd = [args.receiver, "--store-dir", str(Path(args.store).absolute().parent),
                        "--phone-id", str(args.phone_id), "--diagnostics",
                        *(["--ap-channel", str(args.ap_channel)] if getattr(args, "ap_channel", None) else []),
                        "--duration-ms", lifetime_ms]
    else:
        receiver_cmd = [args.receiver, "--port", args.port, "--duration-ms", lifetime_ms]
    if args.store and not wireless:
        receiver_cmd += ["--store", args.store]
    if video_socket:
        receiver_cmd += ["--video-socket", video_socket]
    if input_socket:
        receiver_cmd += ["--input-socket", input_socket]
    phone_serial = getattr(args, "phone_serial", None)
    if phone_serial and not wireless:
        # Same-phone binding at the point of use (#137): the receiver itself
        # refuses a first link or reconnect from any other physical phone.
        receiver_cmd += ["--require-usb-serial", phone_serial]
    if managed_display and not wireless:
        # Contract: --require-video-consumer makes the receiver emit "ready"
        # after listeners/control init and BEFORE version negotiation.
        receiver_cmd += ["--require-video-consumer"]

    samples: list[Sample] = []
    parser = EventParser()
    parser.wireless = wireless
    progress = ProjectionProgress()
    receiver: subprocess.Popen | None = None
    display: subprocess.Popen | None = None
    # The moving-map soak workload (#137): phone intents and map drags through
    # the display's test control channel, media kept playing for the stage.
    workload = None
    stimulus: moving_map.SoakStimulus | None = None
    # Wired: adb by the USB serial. Wireless: the <ip>:5555 bound after active.
    adb_target = {"serial": None if wireless else phone_serial}

    def phone_adb(*a: str) -> subprocess.CompletedProcess:
        if adb_target["serial"] is None:
            return subprocess.CompletedProcess(["adb", *a], 1, "", "adb not bound to the phone")
        return moving_map.adb_runner(adb_target["serial"])(*a)
    prepared = not wireless
    if getattr(args, "workload", "none") == "moving-map":
        workload = moving_map.MovingMap(
            phone_adb, media_uri=args.media_uri,
            media_type=getattr(args, "media_type", None),
            media_component=getattr(args, "media_component", None),
            geo_uri=getattr(args, "geo_uri", None) or moving_map.DEFAULT_GEO_URI,
            usb_reset=not wireless)
        args.cleanup_failures = ["phone_restore_not_reached"]  # until restore_phone runs
    av_series: dict = {"pair_t_ns": [], "pair_skew_ns": [], "audio_intervals_ns": []}
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
    catch_up_doubts_at_cutoff: int | None = None
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
            # Wireless: the window may start only once the phone is prepared.
            projection_active=parser.projection_active and prepared,
            video_units=parser.video_units, audio_chunks=parser.audio_chunks,
            video_presentations=parser.video_presentations,
            audio_writes=parser.audio_writes, av_pairs=parser.av_pairs,
        )

    concurrent_builds: list[dict] = []
    own_processes = ancestors()
    link: dict | None = None
    host_usb_due = 0.0

    def bind_and_prepare() -> bool:
        """Wireless, once active: adb to the expected phone over the AP, then its setup.

        False when the receiver exited meanwhile: the caller's early-exit path
        then judges the stage (a product failure, not an adb block).
        """
        nonlocal deadline
        began = time.monotonic()
        interface = args.ap_interface
        reason = "adb_unreachable"
        while time.monotonic() - began < _ADB_BIND_TIMEOUT_SECONDS:
            if receiver.poll() is not None:
                return False
            candidate, reason = transport.session_candidate(
                transport.receiver_session_peers(receiver.pid), transport.read_ap_stations(interface))
            bound = None
            if candidate:
                bound, reason = transport.bind_adb(phone_serial, [candidate], _host_adb)
            if bound:
                adb_target["serial"] = bound
                break
            time.sleep(1.0)
        if adb_target["serial"] is None:
            raise PhoneSetupError(f"{reason}: adb did not reach --phone-serial over the AP link")
        link["adb_bound"] = True
        link["before"] = transport.link_snapshot(phone_adb)
        if workload is not None and not workload.prepare_phone(
                lambda seconds: (time.sleep(seconds), True)[1]):
            raise PhoneSetupError(workload.phone_failure or "phone setup did not complete")
        deadline += time.monotonic() - began  # the phone setup is not startup budget
        return True

    def sample_host_usb() -> None:
        nonlocal host_usb_due
        if wireless and time.monotonic() >= host_usb_due and len(link["host_usb"]["during"]) < 1000:
            link["host_usb"]["during"].append(transport.host_usb_evidence(phone_serial))
            host_usb_due = time.monotonic() + _HOST_USB_PERIOD_SECONDS

    try:
        if wireless:
            # The cable must be out, shown from this host's USB bus (aa_transport.py).
            link = {"phone_id": args.phone_id, "adb_bound": False, "before": None, "after": None,
                    "host_usb": {"before": transport.host_usb_evidence(phone_serial),
                                 "during": [], "after": None}}
            refusal = transport.host_usb_refusal(link["host_usb"]["before"])
            if refusal:
                raise PhoneSetupError(f"{refusal}: the phone's USB cable must be unplugged "
                                      f"for a wireless stage")
        if workload is not None and not wireless:
            if not workload.prepare_phone(lambda seconds: (time.sleep(seconds), True)[1]):
                # Nothing is launched; the finally restores the phone.
                raise PhoneSetupError(workload.phone_failure or "phone setup did not complete")
            # The phone setup is not part of the startup budget.
            origin_ns = time.monotonic_ns()
            start = origin_ns / 1e9
            deadline = start + startup_timeout
        rec_file = open(receiver_log, "w", encoding="utf-8")
        receiver = subprocess.Popen(receiver_cmd, stdout=rec_file, stderr=subprocess.STDOUT,
                                    start_new_session=True)
        parser.receiver_started_ns = time.monotonic_ns()
        _record_process(args, "receiver", receiver.pid)
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
                if workload is not None:
                    display_cmd += ["--test-only-control-stdin"]  # the map drags
                if video_socket:
                    display_cmd += ["--video-socket", video_socket]
                if input_socket:
                    display_cmd += ["--input-socket", input_socket]
                if time.monotonic_ns() / 1e9 < deadline:
                    display = subprocess.Popen(
                        display_cmd, stdout=disp_file, stderr=subprocess.STDOUT,
                        stdin=subprocess.PIPE if workload is not None else None,
                        start_new_session=True)
                    _record_process(args, "display", display.pid)
                    if workload is not None and display.stdin is not None:
                        os.set_blocking(display.stdin.fileno(), False)

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
            if wireless and not prepared and parser.projection_active:
                if not bind_and_prepare():
                    early_exit = True
                    break
                prepared = True
                continue  # the next sample is the first that may start the window
            if prepared:
                sample_host_usb()
            if (workload is not None and stimulus is None and display is not None
                    and display.stdin is not None and parser.projection_active and prepared):
                stimulus = moving_map.SoakStimulus(workload, display.stdin.fileno(), receiver_log,
                                                   since_ns=time.monotonic_ns())
                stimulus.start()
            if stimulus is not None and stimulus.failure:
                parser.terminal_errors.append({"event": "err", "code": stimulus.failure})
                break
            time.sleep(min(args.interval, 5.0, max(0.0, deadline - now)))
            sample = read_sample()
            logged_progress = None
            if progress.event_anchored and progress.started_ns is not None:
                logged_progress = parser.logged_progress(origin_ns, progress.last_ns, sample.t_ns)
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
            running = build_processes(own_processes)
            if running and len(concurrent_builds) < _BUILD_RECORDS_LIMIT:
                concurrent_builds.append({"t_ns": sample.t_ns, "processes": running[:16]})
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
        # The window closes at the last accepted sample; records still in flight
        # are admitted by their own emission time after the final read.
        parser.cutoff_ns = origin_ns + progress.last_ns
        events = {key: value for key, value in parser.metrics.items()
                  if key not in {*_DECODE_KEYS.values(), *_IPC_KEYS.values()}}
        catch_up_doubts_at_cutoff = parser.catch_up_doubts
        events["helper_rss_mb"] = None
        events["video_units"] = parser.video_units
        events["audio_chunks"] = parser.audio_chunks
        events["startup_ms"] = parser.startup_ms
        av_series = parser.av_series(origin_ns)
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
        if parser.cutoff_ns is None:
            parser.cutoff_ns = origin_ns + progress.last_ns
        if stimulus is not None:
            stimulus.stop()
            # A control failure in the final interval is only seen after the
            # join (the loop may have reached its deadline first): never drop it.
            if stimulus.failure and not any(error.get("code") == stimulus.failure
                                            for error in parser.terminal_errors):
                parser.terminal_errors.append({"event": "err", "code": stimulus.failure})
        if display is not None and display.poll() is not None:
            display_survived = False
        if wireless and workload is not None:
            # adb reaches the phone only while the session (the AP link) is up.
            if adb_target["serial"] is not None:
                for failure in workload.restore_phone():
                    print(f"cleanup failure: {failure}", file=sys.stderr)
                link["after"] = transport.link_snapshot(phone_adb)
            args.cleanup_failures = list(workload.cleanup_failures)
        for name, proc in (("display", display), ("receiver", receiver)):
            cleanup = _terminate(proc)
            if cleanup != "clean":
                parser.terminal_errors.append({"event": "err", "code": name + "_cleanup_" + cleanup})
        if rec_file is not None:
            rec_file.close()
        if disp_file is not None:
            disp_file.close()
        if wireless and link is not None:
            if adb_target["serial"] is not None:
                _host_adb("disconnect", adb_target["serial"])
            if receiver is not None:
                host = link["host_usb"]
                host["after"] = transport.host_usb_evidence(phone_serial)
                for moment, evidence in [("during", e) for e in host["during"]] + [("after", host["after"])]:
                    refusal = transport.host_usb_refusal(evidence)
                    if refusal and workload is not None:
                        workload.cleanup_failures.append(f"{refusal.split(':')[0]}_{moment}: {refusal}")
                        print(f"cleanup failure: {refusal}", file=sys.stderr)
                        break
                if workload is not None:
                    args.cleanup_failures = list(workload.cleanup_failures)
        elif workload is not None:
            # Media stopped and verified first, USB mode last (it re-enumerates USB).
            for failure in workload.restore_phone():
                print(f"cleanup failure: {failure}", file=sys.stderr)
            args.cleanup_failures = list(workload.cleanup_failures)  # for --run-file
        parser.poll(str(receiver_log), final=True)
        if display is not None:
            parser.poll(str(display_log), final=True)

    # In-window records read after the cutoff count at their emission time
    # (#90 review 4): merged into the frozen export, or the metric is invalid.
    for target, value in parser.late_values.items():
        if target in invalid_telemetry or target in parser.late_targets:
            continue
        prior = events.get(target)
        events[target] = value if prior is None else max(prior, value)
    if parser.late_targets:
        invalid_telemetry = sorted(set(invalid_telemetry) | parser.late_targets
                                   | {"late_in_window_telemetry"})
    if catch_up_doubts_at_cutoff is not None and parser.catch_up_doubts != catch_up_doubts_at_cutoff:
        # #154: the catch-up totals froze at the cutoff, but a drop notice is
        # reported only with a later media record - up to the receiver's stop
        # (SIGTERM). One read only in this final poll may stand for a catch-up
        # record from before the cutoff: the frozen totals are unconfirmed.
        for key in ("audio_catch_up_events", "audio_catch_up_frames", "audio_catch_up_ms"):
            events[key] = None
    if receiver is not None:
        receiver_exit = receiver.returncode
    if not stopped_by_us and receiver_exit is not None and receiver_exit != 0:
        crashes = 1
    if not stopped_by_us and not early_exit and receiver_exit is None:
        crashes = 1

    capture = {
        "schema": CAPTURE_SCHEMA,
        "mode": "wireless" if wireless else "wired",
        "duration_seconds": args.duration_seconds,
        "observed_duration_seconds": observed_duration,
        "active_projection_seconds": progress.seconds,
        "startup_wait_seconds": progress.started_ns / 1e9 if progress.started_ns is not None else None,
        "projection_failure": progress.failure,
        "interference": list(progress.interferences),
        "progress_evidence": list(progress.progress_evidence),
        "measurement_origin": "live",
        "crashes": crashes,
        "early_exit": early_exit,
        "receiver_exit_code": receiver_exit,
        "receiver_stopped_by_collector": stopped_by_us,
        "display_survived": display_survived,
        "display_connected": display_connected,
        "terminal_errors": parser.terminal_errors,
        "playback_slot_absences": dict(parser.playback_slot_absences),
        "invalid_telemetry": invalid_telemetry,
        "monotonic_origin_ns": origin_ns,
        "cpu_ticks_per_second": clk,
        "samples": [asdict(sample) for sample in samples],
        "video_evidence": asdict(video_evidence),
        "events": events,
        "av_series": av_series,
        "video_telemetry": video_telemetry,
        "target_hardware_state": hardware if hardware is not None else _target_hardware_state(),
    }
    # Provenance of the measured binaries: the build type the guard verified, the
    # build directory it checked and whether the qualification override was used,
    # so no run can be read as a qualification result it is not.
    # Validity is decided after the final log reads (#90 review): a state
    # overflow, a media drop or a missing completion record that only drained
    # during shutdown still invalidates the capture. Metrics keep their cutoff.
    issues = parser.integrity_issues(str(receiver_log), str(display_log) if display is not None else None)
    if issues:
        capture["invalid_telemetry"] = sorted(set(capture["invalid_telemetry"]) | set(issues))
    if {"media_telemetry_dropped", "receiver_log_incomplete"} & set(issues):
        # Pairing a subset of audio slots can move the signed A/V contract
        # either way (a dropped slot turned a FAIL into a PASS in review), so
        # A/V evidence is invalid. Peak metrics (audio latency, frame drops)
        # stay: a lost sample only lowers an observed peak, so an observed
        # breach is still a confirmed FAIL, and invalid_telemetry keeps any
        # in-budget value from qualifying.
        capture["invalid_telemetry"] = sorted(set(capture["invalid_telemetry"]) | {"av_sync_ms"})
    capture["build"] = build if build is not None else build_record(args)
    capture["workload"] = ({"kind": "moving-map", "media_relaunches": workload.media_relaunches,
                            "drags": workload.drags, "stimulus_started": stimulus is not None,
                            "phone_failure": workload.phone_failure,
                            "cleanup_failures": list(workload.cleanup_failures),
                            "drag_t_ns": [t - origin_ns for t in workload.drag_ns][:_AV_SERIES_LIMIT]}
                           if workload is not None else {"kind": "none"})
    capture["concurrent_builds"] = concurrent_builds
    if wireless:
        ap_channel = getattr(args, "ap_channel", None)
        capture["link"] = link
        capture["ap_interface"] = getattr(args, "ap_interface", None)
        capture["ap_channel"] = (
            ap_channel if ap_channel is not None else transport.DEFAULT_AP_CHANNEL
        )
        capture["startup_basis"] = "harness_launch_to_active"
        capture["media_telemetry_dropped"] = parser.media_telemetry_dropped
    if thermal_policy is not None:
        capture["thermal_policy"] = asdict(thermal_policy)
    return capture


def _cli(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--receiver", required=True, help="receiver binary to launch")
    parser.add_argument("--display", default=None, help="dummy display binary (optional)")
    parser.add_argument("--port", required=True, help="USB port context passed to the receiver "
                        "(wired; ignored for --transport wireless)")
    parser.add_argument("--transport", choices=transport.TRANSPORTS, default=transport.DEFAULT_TRANSPORT,
                        help="session transport (default wired)")
    parser.add_argument("--phone-id", type=int, default=None,
                        help="wireless: the approved wireless phone id in the trust store")
    parser.add_argument("--ap-interface", default=None,
                        help="wireless: the helper's AP interface (required; its root-only "
                             "ap.conf is never read)")
    parser.add_argument("--ap-channel", type=int, choices=transport.AP_CHANNELS, default=None,
                        help="wireless: the AP channel ap.conf names (receiver default 36)")
    parser.add_argument("--duration-seconds", required=True, type=int)
    parser.add_argument("--interval", type=float, default=5.0)
    parser.add_argument("--store", default=None, help="approved-phone store path passed to receiver")
    parser.add_argument("--state-dir", default="/tmp/aa-projection-perf")
    parser.add_argument("--video-socket", default=None)
    parser.add_argument("--input-socket", default=None)
    parser.add_argument("--no-display", action="store_true")
    parser.add_argument("--phone-serial", default=None,
                        help="pin the run to this phone (receiver --require-usb-serial)")
    parser.add_argument("--workload", choices=("none", "moving-map"), default="none",
                        help="moving-map: drive Maps and keep media playing (needs --phone-serial, "
                             "--media-uri and a managed --display)")
    parser.add_argument("--media-uri", default=None)
    parser.add_argument("--media-type", default=None)
    parser.add_argument("--media-component", default=None)
    parser.add_argument("--geo-uri", default=None)
    parser.add_argument("--thermal-policy", choices=("strict-all-zones", NANO_PROFILE),
                        default="strict-all-zones", help="explicit approved Nano CV availability exception")
    parser.add_argument("--startup-timeout-seconds", type=int,
                        default=None,
                        help=f"startup timeout (wired: [1, {_PROJECTION_STARTUP_TIMEOUT_SECONDS}], "
                             f"default {_PROJECTION_STARTUP_TIMEOUT_SECONDS}; wireless: >= "
                             f"{WIRELESS_STARTUP_BUDGET_S}, default {WIRELESS_STARTUP_BUDGET_S})")
    parser.add_argument("--build-dir", default=os.environ.get("AA_BUILD_DIR"),
                        help="build directory (CMakeCache.txt) that produced the launched binaries; "
                             "required for a live qualification stage")
    parser.add_argument("--build-dir-verified-locally", default=None, metavar="BUILD_TYPE",
                        help="cross build: the host that built the binaries already verified "
                             "--build-dir and derived the launched binaries from it, so this "
                             "target must not re-read a path it cannot see")
    parser.add_argument("--allow-unoptimized-build", action="store_true",
                        help="debugging override: start even without an optimized build")
    parser.add_argument("--run-file", default=None,
                        help="new file recording this run's processes for a remote stop "
                             "(tools/perf/aa_remote_stop.sh); removed after cleanup")
    args = parser.parse_args(argv)
    args.run_file_record = None
    args.cleanup_failures = []  # nothing has touched the phone yet
    if args.transport == "wireless" and (not args.phone_id or args.phone_id <= 0 or not args.store
                                         or Path(args.store).name != "approved-phones.json"
                                         or not args.phone_serial
                                         or not transport.valid_interface(args.ap_interface)):
        print("usage error: --transport wireless needs --phone-id, --phone-serial (adb, for the "
              "workload and the link record), --ap-interface IFACE and "
              "--store <dir>/approved-phones.json",
              file=sys.stderr)
        return 2
    if args.run_file:
        # Lead a process group of our own (already true for a session leader):
        # every phone command we start is in it, and nothing else is.
        try:
            os.setpgid(0, 0)
        except PermissionError:
            pass
        if os.getpgid(0) != os.getpid():
            print("error: --run-file: cannot lead a process group of our own", file=sys.stderr)
            return 2
        try:
            args.run_file_record = RunFile(args.run_file)
        except OSError as exc:
            print(f"error: cannot create --run-file: {exc}", file=sys.stderr)
            return 2
        args.run_file_record.process("collector", os.getpid())
        args.run_file_record.line("group", str(os.getpgid(0)), str(os.getsid(0)))
        if args.phone_serial:
            args.run_file_record.line("phone", args.phone_serial)
        if args.media_component:
            args.run_file_record.line("media_package", args.media_component.split("/")[0])
    try:
        return _run(args)
    finally:
        # After collect()'s own cleanup and phone restore: the stop's acknowledgement.
        if args.run_file_record is not None:
            args.run_file_record.remove(getattr(args, "cleanup_failures", None))


def _run(args: argparse.Namespace) -> int:

    if args.duration_seconds <= 0:
        print("error: --duration-seconds must be positive", file=sys.stderr)
        return 2
    if args.workload == "moving-map" and (not args.phone_serial or not args.media_uri
                                          or not args.display or args.no_display):
        print("error: --workload moving-map needs --phone-serial, --media-uri and a managed "
              "--display", file=sys.stderr)
        return 2
    if args.workload == "moving-map" and not moving_map.media_package(args.media_component):
        print("error: --workload moving-map needs --media-component PKG/ACTIVITY, so the media "
              "player can be stopped and verified after the stage", file=sys.stderr)
        return 2
    try:
        args.startup_timeout_seconds = startup_timeout_seconds(
            args.transport, args.startup_timeout_seconds
        )
    except ValueError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2
    if not math.isfinite(args.interval) or args.interval <= 0:
        print("error: --interval must be positive", file=sys.stderr)
        return 2

    # The build guard runs here, before collect() launches the receiver or the
    # display: an unoptimized build is refused instead of being measured.
    host_verified = args.build_dir_verified_locally
    if host_verified is not None:
        # Cross build: the build tree lives on the host that built these
        # binaries, so the target cannot read its cache; the host verified it and
        # the value is recorded as such (never as a target-verified build).
        if not (args.build_dir and str(args.build_dir).strip()):
            print("error: --build-dir-verified-locally requires --build-dir", file=sys.stderr)
            return 2
        if not re.fullmatch(r"[A-Za-z0-9_+.-]+", host_verified):
            print("error: --build-dir-verified-locally must name a CMAKE_BUILD_TYPE value",
                  file=sys.stderr)
            return 2
        try:
            build_type = require_optimized_type(
                host_verified, allow_unoptimized=args.allow_unoptimized_build,
                source=f"{args.build_dir} (verified on the host that built the binaries)")
        except BuildGuardError as exc:
            print(f"{REFUSAL_PREFIX} {exc}", file=sys.stderr)
            return 2
        build = build_record(args, build_type, verified_on="host")
    else:
        try:
            build_type = require_optimized_build(
                args.build_dir, allow_unoptimized=args.allow_unoptimized_build)
        except BuildGuardError as exc:
            print(f"{REFUSAL_PREFIX} {exc}", file=sys.stderr)
            return 2
        binaries: dict[str, dict] = {}
        if args.build_dir and str(args.build_dir).strip():
            # Tie the binaries that will actually be launched to the build that
            # was just verified: another build's binaries must not run here.
            try:
                binaries = verify_launched_binaries(args.build_dir, {
                    "receiver": args.receiver,
                    "display": None if args.no_display else args.display,
                })
            except BuildDirMismatchError as exc:
                print(f"{BINARY_REFUSAL_PREFIX} {exc}", file=sys.stderr)
                return 2
        build = build_record(args, build_type, binaries=binaries)
    # The capture is stdout, so the run-report record goes to stderr.
    print(describe_build(args.build_dir, build_type), file=sys.stderr)
    for role, entry in sorted(build.get("binaries", {}).items()):
        print(f"build binary: {role} {entry['verified']} {entry['build_path']}", file=sys.stderr)

    old_handler = signal.getsignal(signal.SIGTERM)
    signal.signal(signal.SIGTERM, lambda *_: sys.exit(3))
    try:
        capture = collect(args, build=build)
    except PhoneSetupError as exc:
        print(f"{PHONE_SETUP_PREFIX} {exc}; the phone was restored and nothing was measured",
              file=sys.stderr)
        return 2
    except CoverageError as exc:
        print(f"contract violation: {exc}", file=sys.stderr)
        return 4
    finally:
        signal.signal(signal.SIGTERM, old_handler)

    if any(error.get("code") == PHONE_MISMATCH_CODE for error in capture.get("terminal_errors", [])):
        # The run measured some other phone than the soak's: no capture at all.
        print(f"{PHONE_REFUSAL_PREFIX} the phone on --port {args.port} is not --phone-serial "
              f"(at the first link or a reconnect); the stage measured nothing qualifying",
              file=sys.stderr)
        return 2
    json.dump(capture, sys.stdout, indent=2, sort_keys=True, allow_nan=False)
    sys.stdout.write("\n")
    return 0


if __name__ == "__main__":
    sys.exit(_cli(sys.argv[1:]))
