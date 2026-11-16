#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# /// script
# requires-python = ">=3.12"
# dependencies = []
# ///
"""Ten-minute moving-map drive smoke on a real phone, required before any soak (#137).

The soak workload is a near-static screen with a tone. The S24 Ultra video
freeze (#134) happened only with a moving map, so no soak stage could see it.
This smoke projects Google Maps for ten minutes on the phone named by
``--serial``/``--port``. It opens a ``geo:`` intent and pans the map over the
display's test control channel once a second, so the phone keeps encoding new
frames. Optionally it also plays media.

Subcommands:

``run``      Launch a Release ``aa-wired-receiver`` + ``aa-dummy-display`` pair
             on this machine, drive the workload, then analyze and write
             ``result.json`` in ``--out``. Exit 0 PASS, 1 FAIL, 2 usage,
             3 BLOCKED (a prerequisite was missing; nothing was measured).
``analyze``  Re-run the analysis on a ``run`` output directory (no phone needed).
``check``    The soak gate. ``tools/perf/aa-projection-perf.sh`` calls it before
             any live stage: the record must be a PASS for the same receiver
             and display bytes (sha256), for the default video offer, for the
             full duration, and no older than ``RECORD_MAX_AGE_S``.

The pass criteria are the named constants below. The gate qualifies the
receiver's **default video offer**, whose primary profile is
``QUALIFIED_PROFILE`` (1280x720@30 H.264), which is also what the soak stages
negotiate. 1080p60 and 1440p60 do not paint 90 % of 60 fps on the Orin Nano
today, even when healthy (#79, #85), so a smoke run with a ``--video-profiles``
override is reported normally but never satisfies the gate.
"""
from __future__ import annotations

import argparse
import bisect
import hashlib
import json
import math
import os
import re
import signal
import subprocess
import sys
import threading
import time
from datetime import datetime, timezone
from pathlib import Path

TOOLS = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(TOOLS / "perf"))
from aa_build_guard import (  # noqa: E402
    BuildDirMismatchError, BuildGuardError, require_optimized_build, verify_launched_binaries)
from aa_perf_report import BUDGETS  # noqa: E402

SCHEMA = "aa-drive-smoke/1"

# ---- Pass criteria (#137). Do not edit to make a run pass. ------------------
DEFAULT_DURATION_S = 600
# Painted fps is counted in consecutive WINDOW_S windows starting
# FPS_STARTUP_GRACE_S after the first painted frame, to the end of the drive.
# Every full window must reach this share of the negotiated fps. Healthy A26
# runs at the default 720p30 paint 29.8-30.1 fps after startup (2026-11-16), so
# the 27 fps floor has margin, and a one-second freeze in a window (at most 27
# of 30 frames) fails.
WINDOW_S = 10
PAINTED_FPS_MIN_RATIO = 0.90
# The grace applies to the fps criterion only. The phone's encoder ramps up
# after the stream starts. In the 2026-11-16 A26 drive smoke it paused for
# 853 ms 0.1 s after the first frame. It sent 267 frames in the first 10 s and
# all 267 were received, decoded and painted (26.7 fps < 27), while windows
# 1-59 painted 29.8-30.1 fps. That pause is the phone's, not the receiver's,
# and it decides window 0 by chance (the same day's research run painted 28.4).
# waiting_idr, session drop and A/V still cover the whole run from `active`.
# Deviation from the #137 issue text; the owner may veto it.
FPS_STARTUP_GRACE_S = 10
# No waiting_idr episode (producer or consumer, first request to recovery, or
# to the end of the window when it never recovers) may last longer than this.
WAITING_IDR_MAX_S = 1.0
# The existing contractual A/V ceiling. It is checked only where it is valid:
# when the run has matched video-paint/audio-write pairs (media playing).
AV_SYNC_MAX_MS = BUDGETS["av_sync_ms_max"]
# Operational bounds, not pass criteria. There is no first-paint deadline: the
# runner waits this long before it starts the drive clock, and it restarts the
# clock at the first paint if that comes later. Its latency is reported as
# measured.startup_to_first_paint_s.
FIRST_PAINT_WAIT_S = 15.0
READY_TIMEOUT_S = 25.0
STIMULUS_PERIOD_S = 1.0
# The soak gate. Same bytes are checked by hash. The age bound keeps the phone,
# its Android Auto / Maps versions and the target's state close to what the
# smoke saw: the 60+1800+7200 s chain takes about 2.6 h, so one day is a
# same-day rule that an overnight phone app update cannot slip past.
RECORD_MAX_AGE_S = 24 * 3600
RECORD_FUTURE_SKEW_S = 300
QUALIFIED_PROFILE = "1280x720@30:h264"

PER_FRAME = ("video_ipc_send", "video_ipc_receive", "video_decode_completed", "video_presented",
             "audio_write_completed", "input_forwarded", "media_timing", "ok", "status")
PROCESS_NAMES = ("aa-wired-receiv", "aa-dummy-displa")  # /proc comm is 15 chars
BUILD_NAMES = ("cmake", "ninja", "make", "c++", "g++", "gcc", "cc1plus", "ctest", "nvcc")


def read_jsonl(path: Path) -> list[dict]:
    out = []
    if path.exists():
        for line in path.read_text(errors="replace").splitlines():
            if line.startswith("{"):
                try:
                    value = json.loads(line)
                except ValueError:
                    continue
                if isinstance(value, dict):
                    out.append(value)
    return out


# ---- Analysis ---------------------------------------------------------------

def _criterion(name: str, passed: bool | None, **detail) -> dict:
    return {"name": name, "passed": passed, **detail}


def waiting_idr_episodes(stamped: list[dict], end_ns: int, run_start_ns: int | None) -> list[dict]:
    """Episodes from the runner-stamped ``idr_*`` lines (the lines carry no time).

    An episode opens at a source's first ``idr_requested`` and closes at its
    ``idr_recovered``. The binary's own ``latency_ms`` starts at the wait
    itself, which can precede the first successful request, so a recovery
    starts at the earlier of that request and (recovery - latency_ms). When
    latency_ms is absent, negative, nonfinite or not a number, the start cannot
    be established and is taken as ``run_start_ns`` (``active``), even with a
    logged request: fail closed. A recovery without a logged request still
    counts (the display emits one even when every request failed). An episode
    still open at ``end_ns`` runs from its first request to ``end_ns``. Every
    episode is intersected with [run_start_ns, end_ns]; without a known
    ``run_start_ns`` its length is infinite.
    """
    episodes, open_at = [], {}

    def add(source: str, start: int, stop: int, recovered: bool) -> None:
        if run_start_ns is None:
            seconds = math.inf
        else:
            seconds = max(0, min(stop, end_ns) - max(start, run_start_ns)) / 1e9
        episodes.append({"source": source, "start_ns": start, "duration_s": seconds,
                         "recovered": recovered})

    for item in sorted(stamped, key=lambda x: x["observed_ns"]):
        event, t = item["event"], item["observed_ns"]
        source = event.get("source", "unknown")
        if event.get("event") == "idr_requested" and t <= end_ns:
            open_at.setdefault(source, t)
        elif event.get("event") == "idr_recovered":
            requested = open_at.pop(source, None)
            latency = event.get("latency_ms")
            if _finite_number(latency) and latency >= 0:
                by_latency = t - int(latency * 10**6)
                start = by_latency if requested is None else min(requested, by_latency)
            else:
                start = run_start_ns if run_start_ns is not None else t
            add(source, start, t, True)
    for source, start in open_at.items():
        add(source, start, end_ns, False)
    return episodes


def av_pairs_ms(presented: list[dict], audio: list[dict]) -> tuple[list[float], int]:
    """|video paint delay - audio write delay| for each video frame inside an audio slot."""
    valid = sorted((a for a in audio if isinstance(a.get("presented_ns"), int)),
                   key=lambda a: a["scheduled_ns"])
    invalid = sum(1 for a in audio if not isinstance(a.get("presented_ns"), int))
    starts = [a["scheduled_ns"] for a in valid]
    out = []
    for frame in presented:
        j = bisect.bisect_right(starts, frame["scheduled_ns"]) - 1
        if j >= 0 and frame["scheduled_ns"] < valid[j]["scheduled_end_ns"]:
            slot = valid[j]
            out.append(abs((frame["completed_ns"] - frame["scheduled_ns"])
                           - (slot["presented_ns"] - slot["scheduled_ns"])) / 1e6)
    return out, invalid


def analyze(receiver: list[dict], display: list[dict], stamped: list[dict], run: dict) -> dict:
    """Evaluate the four #137 criteria over one run. ``run`` is the runner's record."""
    duration_s = run["duration_s"]
    criteria: list[dict] = []
    profile = next((e for e in receiver if e.get("event") == "video_profile"), None)
    negotiated_fps = profile.get("fps") if profile else None
    paints = sorted((e for e in display if e.get("event") == "video_presented"
                     and isinstance(e.get("completed_ns"), int)), key=lambda e: e["completed_ns"])
    active_ns = run.get("active_ns")
    first = next((e["completed_ns"] for e in paints if active_ns and e["completed_ns"] >= active_ns),
                 None)
    # The drive is [first paint, first paint + duration]; the other criteria
    # also cover the startup from `active` before it.
    start_ns = first
    end_ns = start_ns + int(duration_s * 1e9) if start_ns else None
    stopped_ns = run.get("window_end_ns")

    # 1. Painted fps per window, after the startup grace.
    windows: list[float] = []
    if start_ns and negotiated_fps:
        times = [e["completed_ns"] for e in paints]
        width = WINDOW_S * 10**9
        grace = FPS_STARTUP_GRACE_S * 10**9
        limit = min(end_ns, stopped_ns or end_ns)
        for k in range(max(0, int((limit - start_ns - grace) // width))):
            lo = start_ns + grace + k * width
            count = bisect.bisect_left(times, lo + width) - bisect.bisect_left(times, lo)
            windows.append(count / WINDOW_S)
        floor = PAINTED_FPS_MIN_RATIO * negotiated_fps
        failing = [{"window": k, "start_offset_s": FPS_STARTUP_GRACE_S + k * WINDOW_S,
                    "painted_fps": fps} for k, fps in enumerate(windows) if fps < floor]
        expected = int((duration_s - FPS_STARTUP_GRACE_S) // WINDOW_S)
        criteria.append(_criterion(
            "painted_fps_per_window", not failing and len(windows) == expected,
            floor_fps=floor, negotiated_fps=negotiated_fps, startup_grace_s=FPS_STARTUP_GRACE_S,
            windows=len(windows), expected_windows=expected,
            min_fps=min(windows) if windows else None, failing_windows=failing[:20]))
    else:
        criteria.append(_criterion(
            "painted_fps_per_window", False,
            reason="no negotiated video_profile" if not negotiated_fps
            else "no painted frame after active"))

    # 2. waiting_idr episodes.
    cutoff = end_ns or stopped_ns or 0
    episodes = waiting_idr_episodes(stamped, cutoff, active_ns) if cutoff else []
    long = [e for e in episodes if e["duration_s"] > WAITING_IDR_MAX_S]
    criteria.append(_criterion("waiting_idr_max", not long and bool(cutoff),
                               max_s=WAITING_IDR_MAX_S, episodes=len(episodes),
                               longest_s=max((e["duration_s"] for e in episodes), default=0.0),
                               over_limit=long[:20]))

    # 3. Session drop: both processes alive at the end of the window, it ran
    # its full length, and the receiver never left the connected session inside it.
    dropped = [e for e in receiver if e.get("event") == "diag"
               and e.get("kind") == "session_state_changed" and e.get("to_state") == "disconnected"
               and isinstance(e.get("mono_ns"), int) and cutoff and e["mono_ns"] < cutoff]
    terminal = [e for e in receiver + display if e.get("event") == "terminal_error"]
    full = bool(end_ns and stopped_ns and stopped_ns >= end_ns)
    survived = bool(run.get("receiver_alive_at_end") and run.get("display_alive_at_end"))
    criteria.append(_criterion("no_session_drop", full and survived and not dropped and not terminal,
                               full_window=full, processes_survived=survived,
                               disconnects=len(dropped), terminal_errors=terminal[:5]))

    # 4. A/V within the existing ceiling where valid.
    window_paints = [e for e in paints if active_ns and active_ns <= e["completed_ns"] <= cutoff
                     and isinstance(e.get("scheduled_ns"), int)]
    audio = [e for e in receiver if e.get("event") == "audio_write_completed"
             and isinstance(e.get("scheduled_ns"), int) and isinstance(e.get("scheduled_end_ns"), int)]
    pairs, invalid_audio = av_pairs_ms(window_paints, audio)
    if pairs:
        worst = max(pairs)
        criteria.append(_criterion("av_sync_within_ceiling", worst <= AV_SYNC_MAX_MS,
                                   ceiling_ms=AV_SYNC_MAX_MS, max_ms=worst, pairs=len(pairs),
                                   invalid_audio_slots=invalid_audio))
    else:
        criteria.append(_criterion("av_sync_within_ceiling", None, ceiling_ms=AV_SYNC_MAX_MS,
                                   pairs=0, invalid_audio_slots=invalid_audio,
                                   reason="not valid: no matched video/audio pairs (no media)"))

    verdict = "PASS" if all(c["passed"] is not False for c in criteria) else "FAIL"
    seconds = (min(cutoff, stopped_ns or cutoff) - start_ns) / 1e9 if start_ns and cutoff else None
    return {
        "verdict": verdict,
        "criteria": criteria,
        "negotiated_profile": profile and {k: profile.get(k) for k in ("width", "height", "fps", "codec")},
        "measured": {
            "window_seconds": seconds,
            "startup_to_first_paint_s": (first - active_ns) / 1e9 if first and active_ns else None,
            "painted_fps": (sum(1 for e in window_paints if e["completed_ns"] >= start_ns) / seconds
                            if seconds else None),
            "painted_fps_windows": windows,
            "idr_event_counts": {name: sum(1 for x in stamped if x["event"].get("event") == name)
                                 for name in ("idr_requested", "idr_escalated", "idr_recovered",
                                              "idr_transition_failed")},
            "video_dropped": [e for e in receiver if e.get("event") == "video_dropped"][-10:],
        },
    }


def analyze_dir(out: Path) -> dict:
    run = json.loads((out / "run.json").read_text())
    return analyze(read_jsonl(out / "receiver.log"), read_jsonl(out / "display.log"),
                   read_jsonl(out / "idr-events.jsonl"), run)


# ---- Soak gate --------------------------------------------------------------

DIGEST = re.compile(r"[0-9a-f]{64}")


def _finite_number(value: object) -> bool:
    return (isinstance(value, (int, float)) and not isinstance(value, bool)
            and math.isfinite(value))


def _reject_constant(name: str):
    raise ValueError(f"non-standard JSON constant {name}")


def load_record(text: str) -> dict | None:
    """Strict JSON: NaN/Infinity are refused, and the result must be an object."""
    value = json.loads(text, parse_constant=_reject_constant)
    return value if isinstance(value, dict) else None


def check_record(record: dict | None, *, receiver_sha256: str, display_sha256: str,
                 now: datetime) -> tuple[str | None, str]:
    """Return (blocked reason, detail); reason None means the soak may start.

    Both launched binaries must have been hashed (64 lowercase hex digits) and
    both must match the smoke's; every field the gate uses is type-checked.
    """
    if not isinstance(record, dict) or record.get("schema") != SCHEMA:
        return "drive_smoke_missing", "no aa-drive-smoke/1 result document"
    for role, digest in (("receiver", receiver_sha256), ("display", display_sha256)):
        if not isinstance(digest, str) or not DIGEST.fullmatch(digest):
            return "drive_smoke_hash_failed", (f"the launched {role} could not be hashed "
                                               f"(or none is configured)")
    binaries = record.get("binaries")
    duration = record.get("duration_s")
    finished_text = record.get("finished_utc")
    if (not isinstance(record.get("verdict"), str) or not isinstance(binaries, dict)
            or not all(isinstance(binaries.get(r), dict)
                       and isinstance(binaries[r].get("sha256"), str)
                       and DIGEST.fullmatch(binaries[r]["sha256"]) for r in ("receiver", "display"))
            or not _finite_number(duration) or not isinstance(finished_text, str)
            or "video_profiles" not in record
            or not (record["video_profiles"] is None or isinstance(record["video_profiles"], str))):
        return "drive_smoke_malformed", "the result lacks a well-typed verdict, binaries, " \
            "duration_s, finished_utc or video_profiles"
    if record["verdict"] != "PASS":
        return "drive_smoke_not_passed", f"verdict {record['verdict']!r}"
    for role, digest in (("receiver", receiver_sha256), ("display", display_sha256)):
        if binaries[role]["sha256"] != digest:
            return "drive_smoke_build_mismatch", f"{role} sha256 differs from the smoke's"
    if record["video_profiles"] is not None:
        return "drive_smoke_profile", "the smoke overrode the default video offer"
    if duration < DEFAULT_DURATION_S:
        return "drive_smoke_short", f"duration {duration} s < {DEFAULT_DURATION_S} s"
    try:
        finished = datetime.fromisoformat(finished_text)
    except ValueError:
        return "drive_smoke_malformed", "finished_utc is not an ISO-8601 time"
    if finished.tzinfo is None:
        return "drive_smoke_malformed", "finished_utc has no time zone"
    age = (now - finished).total_seconds()
    if age > RECORD_MAX_AGE_S or age < -RECORD_FUTURE_SKEW_S:
        return "drive_smoke_stale", f"finished {age / 3600:.1f} h ago (limit {RECORD_MAX_AGE_S // 3600} h)"
    return None, f"PASS {age / 3600:.1f} h ago on the same receiver/display bytes"


def _check_cli(args) -> int:
    malformed = False
    try:
        record = load_record(Path(args.record).read_text())
    except OSError:
        record = None
    except ValueError:  # not strict JSON (including NaN/Infinity)
        record, malformed = None, True
    # --now exists only for deterministic tests and offline review. The soak
    # launcher never passes it, so live freshness uses the real clock.
    now = datetime.fromisoformat(args.now) if args.now else datetime.now(timezone.utc)
    if now.tzinfo is None:
        now = now.replace(tzinfo=timezone.utc)
    reason, detail = check_record(record, receiver_sha256=args.receiver_sha256,
                                  display_sha256=args.display_sha256, now=now)
    if malformed:
        reason, detail = "drive_smoke_malformed", "the result is not strict JSON"
    if reason:
        print(reason)
        print(f"BLOCKED: {reason}: {detail}; run tools/wired-qa/aa_drive_smoke.py run "
              f"on this build first (docs/qualification/wired-perf-runbook.md)", file=sys.stderr)
        return 3
    print(f"drive smoke: {detail}", file=sys.stderr)
    return 0


# ---- Live runner ------------------------------------------------------------

def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        for block in iter(lambda: handle.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def _comms() -> list[str]:
    out = []
    for path in Path("/proc").glob("[0-9]*/comm"):
        try:
            out.append(path.read_text().strip())
        except OSError:
            pass
    return out


class Logger(threading.Thread):
    """Copy one process's output to its log; stamp idr lines and note milestones."""

    def __init__(self, stream, log: Path, stamped, lock, name: str):
        super().__init__(daemon=True)
        self.stream, self.log, self.stamped, self.lock, self.label = stream, log, stamped, lock, name
        self.seen: dict[str, int] = {}

    def run(self) -> None:
        with self.log.open("w") as out:
            for line in self.stream:
                now = time.monotonic_ns()
                out.write(line)
                if not line.startswith("{"):
                    continue
                if any(f'{{"event":"{n}"' in line for n in PER_FRAME) and not (
                        "first_paint" not in self.seen and '{"event":"video_presented"' in line):
                    continue  # per-frame lines are analyzed from the log afterwards
                try:
                    event = json.loads(line)
                except ValueError:
                    continue
                name = event.get("event", "")
                if name == "video_presented":
                    self.seen.setdefault("first_paint", event.get("completed_ns", now))
                    continue
                self.seen.setdefault(name, now)
                if name.startswith("idr_"):
                    with self.lock:
                        self.stamped.write(json.dumps({"observed_ns": now, "log": self.label,
                                                       "event": event}) + "\n")
                        self.stamped.flush()


def _adb(serial: str, *args: str) -> subprocess.CompletedProcess:
    return subprocess.run(["adb", "-s", serial, *args], capture_output=True, text=True, timeout=20)


STOP_SIGNALS = (signal.SIGTERM, signal.SIGINT)


class StopFlag:
    """SIGTERM/SIGINT handler that only records the first signal; it never raises.

    The runner polls it at every wait point and unwinds through its normal
    control flow, so a signal can never skip registering a child or interrupt
    cleanup: a second signal just finds the flag already set.
    """

    def __init__(self) -> None:
        self.signum: int | None = None

    def __call__(self, signum: int, _frame) -> None:
        if self.signum is None:
            self.signum = signum

    def __bool__(self) -> bool:
        return self.signum is not None

    def sleep(self, seconds: float) -> bool:
        """Sleep in short steps; False as soon as a stop is requested."""
        deadline = time.monotonic() + seconds
        while not self:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                return True
            time.sleep(min(0.1, remaining))
        return False


def _stop(process) -> None:
    """Stop a child started with start_new_session: its whole process group."""
    if process is None:
        return
    for sig, wait in ((signal.SIGTERM, 8), (signal.SIGKILL, 5)):
        try:
            os.killpg(process.pid, sig)
        except (ProcessLookupError, PermissionError):
            pass
        try:
            process.wait(timeout=wait)
        except subprocess.TimeoutExpired:
            continue
        if sig == signal.SIGTERM:
            try:  # the leader is gone; take any member it left behind too
                os.killpg(process.pid, signal.SIGKILL)
            except (ProcessLookupError, PermissionError):
                pass
        return


def _spawn(children: list, command: list[str], **kwargs) -> subprocess.Popen:
    """Start a child in its own session and register it for cleanup at once."""
    process = subprocess.Popen(command, start_new_session=True, **kwargs)
    children.append(process)
    return process


def _wait(predicate, timeout: float, stop: StopFlag, *alive) -> bool:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline and not stop:
        if predicate():
            return True
        if any(p.poll() is not None for p in alive):
            return False
        time.sleep(0.1)
    return predicate() and not stop


def _blocked(out: Path, base: dict, reason: str) -> int:
    base.update(verdict="BLOCKED", blocked_on=reason, finished_utc=datetime.now(timezone.utc).isoformat())
    (out / "result.json").write_text(json.dumps(base, indent=2) + "\n")
    print(f"BLOCKED: {reason}", file=sys.stderr)
    return 3


def _run_cli(args) -> int:
    out = Path(args.out)
    out.mkdir(mode=0o700, parents=True, exist_ok=True)
    receiver_bin, display_bin = Path(args.receiver), Path(args.display)
    store = Path(args.store)
    base: dict = {"schema": SCHEMA, "duration_s": args.duration, "port": args.port,
                  "video_profiles": args.video_profiles, "criteria_constants": {
                      "window_s": WINDOW_S, "painted_fps_min_ratio": PAINTED_FPS_MIN_RATIO,
                      "fps_startup_grace_s": FPS_STARTUP_GRACE_S,
                      "waiting_idr_max_s": WAITING_IDR_MAX_S, "av_sync_max_ms": AV_SYNC_MAX_MS},
                  "qualified_profile": QUALIFIED_PROFILE if args.video_profiles is None else None}
    try:
        build_type = require_optimized_build(args.build_dir)
        verify_launched_binaries(args.build_dir, {"receiver": str(receiver_bin),
                                                  "display": str(display_bin)})
    except BuildGuardError as error:
        print(f"error: unoptimized_build: {error}", file=sys.stderr)
        return _blocked(out, base, "unoptimized_build")
    except BuildDirMismatchError as error:
        print(f"error: build_dir_mismatch: {error}", file=sys.stderr)
        return _blocked(out, base, "build_dir_mismatch")
    base["build"] = {"build_dir": str(args.build_dir), "build_type": build_type}
    base["binaries"] = {"receiver": {"path": str(receiver_bin), "sha256": sha256(receiver_bin)},
                        "display": {"path": str(display_bin), "sha256": sha256(display_bin)}}
    if any(name in BUILD_NAMES for name in _comms()):
        return _blocked(out, base, "build_running")
    if any(name in PROCESS_NAMES for name in _comms()):
        return _blocked(out, base, "projection_process_running")
    devices = subprocess.run(["adb", "devices", "-l"], capture_output=True, text=True, timeout=20).stdout
    line = next((entry for entry in devices.splitlines() if entry.split()[:1] == [args.serial]), "")
    if " device " not in f" {line} " or f"usb:{args.port}" not in line.split():
        return _blocked(out, base, "phone_absent_on_port")
    base["phone_model"] = _adb(args.serial, "shell", "getprop", "ro.product.model").stdout.strip()
    store_before = sha256(store) if store.exists() else None

    env = os.environ.copy()
    uid = os.getuid()
    env.setdefault("XDG_RUNTIME_DIR", f"/run/user/{uid}")
    env.setdefault("DBUS_SESSION_BUS_ADDRESS", f"unix:path=/run/user/{uid}/bus")
    if args.display_env:
        display, _, xauth = args.display_env.partition(",")
        env["DISPLAY"] = display
        if xauth:
            env["XAUTHORITY"] = xauth
    socket = f"{env['XDG_RUNTIME_DIR']}/aa-drive-smoke-{os.getpid()}"
    profiles = ["--video-profiles", args.video_profiles] if args.video_profiles else []
    receiver_cmd = [str(receiver_bin), "--port", args.port, "--store", str(store),
                    "--require-video-consumer", "--video-socket", socket + ".video",
                    "--input-socket", socket + ".input", *profiles, "--duration-ms",
                    str(int((2 * args.duration + READY_TIMEOUT_S * 2 + FIRST_PAINT_WAIT_S + 30) * 1000))]
    display_cmd = [str(display_bin), "--test-only-autostart", "--test-only-control-stdin",
                   "--video-socket", socket + ".video", "--input-socket", socket + ".input", *profiles]
    run = {"duration_s": args.duration, "receiver_command": receiver_cmd, "display_command": display_cmd,
           "started_utc": datetime.now(timezone.utc).isoformat()}
    lock = threading.Lock()
    loggers: list[Logger] = []
    children: list[subprocess.Popen] = []
    stop = StopFlag()
    previous = {sig: signal.signal(sig, stop) for sig in STOP_SIGNALS}
    stamped = (out / "idr-events.jsonl").open("w")

    def drive() -> None:
        _adb(args.serial, "shell", "svc", "usb", "setFunctions", "mtp")
        if not stop.sleep(3):
            return
        _adb(args.serial, "shell", "input", "keyevent", "KEYCODE_WAKEUP")
        if args.media_uri:
            media = ["-d", args.media_uri] + (["-t", args.media_type] if args.media_type else [])
            media += ["-n", args.media_component] if args.media_component else []
            _adb(args.serial, "shell", "am", "start", "-a", "android.intent.action.VIEW", *media)
            if not stop.sleep(4):
                return
        _adb(args.serial, "shell", "am", "start", "-a", "android.intent.action.VIEW",
             "-d", args.geo_uri, "-p", "com.google.android.apps.maps")
        if stop:
            return
        receiver = _spawn(children, receiver_cmd, stdout=subprocess.PIPE,
                          stderr=subprocess.STDOUT, env=env, text=True)
        loggers.append(Logger(receiver.stdout, out / "receiver.log", stamped, lock, "receiver"))
        loggers[-1].start()
        rx = loggers[0].seen
        if not _wait(lambda: "ready" in rx, READY_TIMEOUT_S, stop, receiver):
            run["failure"] = "receiver not ready"
            return
        display = _spawn(children, display_cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                         stderr=subprocess.STDOUT, env=env, text=True)
        loggers.append(Logger(display.stdout, out / "display.log", stamped, lock, "display"))
        loggers[-1].start()
        dx = loggers[1].seen
        if not _wait(lambda: "active" in rx, READY_TIMEOUT_S, stop, receiver, display):
            run["failure"] = "no active session within the startup budget"
            return
        run["active_ns"] = rx["active"]
        _wait(lambda: "first_paint" in dx, FIRST_PAINT_WAIT_S, stop, receiver, display)
        span = int(args.duration * 1e9) + 5 * 10**8
        end = dx.get("first_paint", time.monotonic_ns()) + span
        step, last = 0, 0.0
        print(f"drive smoke: active, window {args.duration} s", file=sys.stderr, flush=True)
        while (not stop and time.monotonic_ns() < end
               and receiver.poll() is None and display.poll() is None):
            if "first_paint" in dx:  # a late first paint restarts the drive clock
                end = max(end, dx["first_paint"] + span)
            if time.monotonic() - last >= STIMULUS_PERIOD_S:
                a, b = (440, 700) if step % 2 == 0 else (700, 440)
                lines = [f"touch down {a} 260"]
                lines += [f"touch move {int(a + (b - a) * j / 6)} 260" for j in range(1, 7)]
                try:
                    for entry in lines + [f"touch up {b} 260"]:
                        display.stdin.write(entry + "\n")
                        display.stdin.flush()
                        time.sleep(0.06)
                except (BrokenPipeError, OSError):
                    pass
                step, last = step + 1, time.monotonic()
            time.sleep(0.1)
        run["window_end_ns"] = time.monotonic_ns()
        run["receiver_alive_at_end"] = receiver.poll() is None
        run["display_alive_at_end"] = display.poll() is None

    try:
        drive()
    finally:
        # The single cleanup, reached by normal flow (signals only set the flag)
        # or by an exception: every registered child's group, then the phone.
        for process in reversed(children):
            _stop(process)
        for logger in loggers:
            logger.join(timeout=5)
        stamped.close()
        _adb(args.serial, "shell", "svc", "usb", "setFunctions", "mtp")
        if args.media_uri and args.media_component:
            _adb(args.serial, "shell", "am", "force-stop", args.media_component.split("/")[0])
    receiver = children[0] if children else None
    display = children[1] if len(children) > 1 else None
    if stop:
        print(f"drive smoke: interrupted by {signal.Signals(stop.signum).name}; cleaned up",
              file=sys.stderr, flush=True)
        run["interrupted"] = signal.Signals(stop.signum).name
        (out / "run.json").write_text(json.dumps(run, indent=2) + "\n")
        _blocked(out, base, "interrupted")
        for sig, handler in previous.items():
            signal.signal(sig, handler)
        return 128 + stop.signum
    run.update(receiver_exit=receiver.returncode if receiver else None,
               display_exit=display.returncode if display else None)
    (out / "run.json").write_text(json.dumps(run, indent=2) + "\n")
    result = {**base, **analyze_dir(out), "finished_utc": datetime.now(timezone.utc).isoformat()}
    if run.get("failure"):
        result["failure"] = run["failure"]
    store_after = sha256(store) if store.exists() else None
    result["trust_store_unchanged"] = store_before == store_after
    if not result["trust_store_unchanged"]:
        result["verdict"] = "FAIL"
    (out / "result.json").write_text(json.dumps(result, indent=2) + "\n")
    _print_summary(result)
    for sig, handler in previous.items():
        signal.signal(sig, handler)
    return 0 if result["verdict"] == "PASS" else 1


def _print_summary(result: dict) -> None:
    print(f"drive smoke: {result['verdict']}")
    for c in result.get("criteria", []):
        state = {True: "pass", False: "FAIL", None: "n/a"}[c["passed"]]
        extra = {k: v for k, v in c.items() if k in ("min_fps", "floor_fps", "longest_s", "max_ms",
                                                     "pairs", "reason", "windows")}
        print(f"  {state:4} {c['name']} {json.dumps(extra)}")


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    sub = parser.add_subparsers(dest="command", required=True)
    run = sub.add_parser("run", help="drive the smoke on the phone (live)")
    run.add_argument("--serial", required=True, help="adb serial of the phone")
    run.add_argument("--port", default=os.environ.get("AA_PERF_PORT", "1-2.1"),
                     help="USB port context the phone is on (default 1-2.1)")
    run.add_argument("--build-dir", default=os.environ.get("AA_BUILD_DIR"),
                     help="optimized build directory that produced the binaries")
    run.add_argument("--receiver", help="default: <build-dir>/aa-wired-receiver")
    run.add_argument("--display", help="default: <build-dir>/aa-dummy-display")
    run.add_argument("--store", default=str(Path.home() / ".local/state/android-auto-receiver/"
                                            "approved-phones.json"))
    run.add_argument("--duration", type=int, default=DEFAULT_DURATION_S)
    run.add_argument("--display-env", help="DISPLAY[,XAUTHORITY] of the owner's session")
    run.add_argument("--video-profiles", help="diagnostic override; such a run never satisfies the gate")
    run.add_argument("--geo-uri", default="geo:0,0?q=Atlanta")
    run.add_argument("--media-uri", help="optional media to play on the phone (validates A/V)")
    run.add_argument("--media-type")
    run.add_argument("--media-component", help="explicit PKG/ACTIVITY for --media-uri")
    run.add_argument("--out", required=True, help="new private directory for logs and result.json")
    analyze_p = sub.add_parser("analyze", help="re-analyze a run directory")
    analyze_p.add_argument("dir")
    check = sub.add_parser("check", help="soak gate: is there a fresh PASS for these bytes?")
    check.add_argument("--record", required=True)
    check.add_argument("--receiver-sha256", required=True)
    check.add_argument("--display-sha256", required=True)
    check.add_argument("--now", help="ISO-8601 time; tests and offline review only, never live")
    args = parser.parse_args(argv)
    if args.command == "check":
        return _check_cli(args)
    if args.command == "analyze":
        result = analyze_dir(Path(args.dir))
        _print_summary(result)
        return 0 if result["verdict"] == "PASS" else 1
    if args.duration <= 0:
        parser.error("--duration must be positive")
    if not args.build_dir:
        parser.error("--build-dir (or AA_BUILD_DIR) is required: the smoke qualifies a Release build")
    args.receiver = args.receiver or str(Path(args.build_dir) / "aa-wired-receiver")
    args.display = args.display or str(Path(args.build_dir) / "aa-dummy-display")
    return _run_cli(args)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
