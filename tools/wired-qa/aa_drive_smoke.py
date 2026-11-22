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
frames. It also plays the media named by ``--media-uri`` for the whole drive
(re-sending the intent when audio stops), because the signed A/V contract
requires audio across at least 90 % of the drive.

Subcommands:

``run``      Launch a Release ``aa-wired-receiver`` + ``aa-dummy-display`` pair
             on this machine, drive the workload, then analyze and write
             ``result.json`` in ``--out``. Exit 0 PASS, 1 FAIL, 2 usage,
             3 BLOCKED (a prerequisite was missing and nothing was measured,
             or the phone was not shown restored afterwards: cleanup:*).
``analyze``  Re-run the analysis on a ``run`` output directory (no phone needed).
``check``    The soak gate. ``tools/perf/aa-projection-perf.sh`` calls it before
             any live stage: the record must be a PASS from the soak's own
             phone (serial, verified on its port), for the same receiver and
             display bytes (sha256), for the default video offer and
             decode/presentation configuration, for the full duration, and no
             older than ``RECORD_MAX_AGE_S``.

The pass criteria are the named constants below. The gate qualifies the
receiver's **default video offer**, whose primary profile is
``QUALIFIED_PROFILE`` (1280x720@30 H.264), which is also what the soak stages
negotiate. 1080p60 and 1440p60 do not paint 90 % of 60 fps on the Orin Nano
today, even when healthy (#79, #85), so a smoke run with a ``--video-profiles``
override is reported normally but never satisfies the gate.

``--transport wired|wireless`` (default wired) picks the session's transport
(tools/perf/aa_transport.py). Wireless launches ``aa-wireless-receiver`` for
the approved ``--phone-id``; the receiver brings the session up through the
system helper's AP and Bluetooth bootstrap when the display asks to project,
and tears it down when it stops. The criteria are the same.

* The phone's USB cable must be out: the Jetson's USB bus must show neither the
  phone (``--serial``) nor an AOA device before, during and after the drive.
* adb runs over the AP link to the phone the receiver admitted: after the
  session is active the runner takes the peer of the receiver's own TCP session
  (it must be the only one and a station of the AP on ``--ap-interface``, which
  the operator names: the helper's ap.conf is root-only and never read; pass
  ``--ap-channel`` when ap.conf names a channel other than 36), connects to it on 5555 and
  binds it only when the device's ``getprop ro.serialno`` is ``--serial``; only
  then does it prepare the phone (wake, media, Maps) and start the drive clock
  (``drive_from_ns``: the drive is the first paint after it). Otherwise:
  BLOCKED ``adb_session_*`` / ``adb_unreachable`` / ``adb_phone_mismatch``.
  The media player is stopped and verified before the session is torn down,
  while adb still reaches the phone.
* Wireless has no ``active`` line: the receiver's own ``session_state_changed``
  to ``active`` diagnostic (``--diagnostics``, bare on main, wrapped as
  ``"event":"diag"`` after #163) is the marker. A session drop is also any
  re-admission after it (a second ``wireless_admission``,
  ``wireless_admission_expired``, a new diagnostic session id, or a state of
  reconnecting/disconnected/failed).
* The record names its transport, ``phone_id`` and ``link``;
  ``check --transport`` refuses a record of another transport and, for
  wireless, of another ``--phone-id``.

Either transport BLOCKS ``projection_process_running`` while any receiver
(wired or wireless) or display is already running.
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
import aa_av_contract as av_contract  # noqa: E402
import aa_log_integrity as integrity  # noqa: E402
import aa_moving_map as moving_map  # noqa: E402
import aa_transport as transport  # noqa: E402
from aa_build_guard import (  # noqa: E402
    BuildDirMismatchError, BuildGuardError, require_optimized_build, verify_launched_binaries)

SCHEMA = "aa-drive-smoke/2"

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
# Signed A/V contract: the four clauses and their limits live in
# tools/perf/aa_av_contract.py, shared with the soak stages (rationale in the
# runbook). Re-exported here because the record's criteria_constants name them.
AV_MEAN_WINDOW_S = av_contract.AV_MEAN_WINDOW_S
AV_MEAN_MAX_ABS_MS = av_contract.AV_MEAN_MAX_ABS_MS
AV_P99_ABS_MAX_MS = av_contract.AV_P99_ABS_MAX_MS
AV_MAX_ABS_MS = av_contract.AV_MAX_ABS_MS
AV_COVERAGE_MIN = av_contract.AV_COVERAGE_MIN
window_means = av_contract.window_means
# The moving-map workload (drags, control channel, media keep-alive) lives in
# tools/perf/aa_moving_map.py, shared with the soak stages.
MEDIA_RELAUNCH_SILENCE_S = moving_map.MEDIA_RELAUNCH_SILENCE_S
STIMULUS_PERIOD_S = moving_map.STIMULUS_PERIOD_S
CONTROL_WRITE_TIMEOUT_S = moving_map.CONTROL_WRITE_TIMEOUT_S
# Operational bounds, not pass criteria. There is no first-paint deadline: the
# runner waits this long before it starts the drive clock, and it restarts the
# clock at the first paint if that comes later. Its latency is reported as
# measured.startup_to_first_paint_s.
FIRST_PAINT_WAIT_S = 15.0
READY_TIMEOUT_S = 25.0
# Wireless startup budget derived in aa_transport.WIRELESS_STARTUP_BUDGET_S from:
# - tools/wireless-receiver/Runtime.hpp:228 (15 s per admission attempt)
# - tools/wireless-receiver/Runtime.hpp:35,207,242 (250 ms initial backoff, 5 s cap)
# - tools/wireless-receiver/Runtime.hpp:251 (attempt held while bootstrapping)
# - include/aa/app/HandoffEligibility.hpp:45,47 (40 s bootstrap #170 + 5 s connect)
# - src/app/WirelessLink.cpp:58,84,143-146 (10 s post-TCP negotiation before ACTIVE)
# - src/helper/BusClient.cpp:21,104,176,184 (2 x 4 helper RPCs @ 1 s)
# - Ready poll (5 s) + display probe fallback (5 s) + safety margin (12 s) = 120 s
WIRELESS_STARTUP_BUDGET_S = float(transport.WIRELESS_STARTUP_BUDGET_S)
# tools/wireless-receiver/main.cpp:146 caps --duration-ms at 86,400,000 ms (86,400 s).
_RECEIVER_MAX_LIFETIME_S = 86_400
# The soak gate. Same bytes are checked by hash. The age bound keeps the phone,
# its Android Auto / Maps versions and the target's state close to what the
# smoke saw: the 1800+7200 s chain takes about 2.5 h, so one day is a
# same-day rule that an overnight phone app update cannot slip past.
RECORD_MAX_AGE_S = 24 * 3600
RECORD_FUTURE_SKEW_S = 300
QUALIFIED_PROFILE = "1280x720@30:h264"

PER_FRAME = ("video_ipc_send", "video_ipc_receive", "video_decode_completed", "video_presented",
             "audio_write_completed", "input_forwarded", "media_timing", "ok", "status")
PROCESS_NAMES = (*transport.RECEIVER_COMMS, "aa-dummy-displa")  # /proc comm is 15 chars
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


def idr_time(item: dict) -> int:
    """When an ``idr_*`` event happened: its source emission stamp (``emit_ns``,
    CLOCK_MONOTONIC like the runner), never when the line reached the runner
    (#90 review: a log delivered late must not move an episode out of the window).
    Unstamped lines fall back to their arrival time; analyze() fails them closed."""
    stamp = integrity.emit_ns(item["event"])
    if stamp is not None:
        return stamp
    observed = item.get("observed_ns")
    return observed if type(observed) is int else 0


IDR_OPENERS = ("idr_wait_started", "idr_requested")


def idr_records(receiver: list[dict], display: list[dict]) -> list[dict]:
    """Every ``idr_*`` record of the completed raw logs (#90 review 4).

    The verdict is built from the logs the integrity check proved complete, not
    from the runner's live sidecar (which only says when a line reached it).
    """
    return [{"event": event, "log": label}
            for label, log in (("receiver", receiver), ("display", display))
            for event in log if str(event.get("event", "")).startswith("idr_")]


def _attach_observed(stamped: list[dict], sidecar: list[dict] | None) -> None:
    """Give each raw-log IDR record the runner's arrival time of the same line."""
    pending: dict[str, list[int]] = {}
    for item in sidecar or []:
        if type(item.get("observed_ns")) is int and isinstance(item.get("event"), dict):
            pending.setdefault(json.dumps(item["event"], sort_keys=True), []).append(item["observed_ns"])
    for item in stamped:
        times = pending.get(json.dumps(item["event"], sort_keys=True))
        if times:
            item["observed_ns"] = times.pop(0)


def waiting_idr_episodes(stamped: list[dict], end_ns: int, run_start_ns: int | None) -> list[dict]:
    """Episodes from the ``idr_*`` lines, timed at emission (see idr_time).

    An episode opens at a source's first ``idr_wait_started`` (the wait
    transition itself, logged even when every request fails) or
    ``idr_requested``, and closes at its
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

    for item in sorted(stamped, key=idr_time):
        event, t = item["event"], idr_time(item)
        source = event.get("source", "unknown")
        if event.get("event") in IDR_OPENERS and t <= end_ns:
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


def _valid_audio(audio: list[dict]) -> tuple[list[dict], int]:
    """Audio slots with a usable playback endpoint, by schedule; and the count of the rest."""
    valid, invalid = [], 0
    for slot in audio:
        if all(type(slot.get(k)) is int for k in ("scheduled_ns", "scheduled_end_ns", "presented_ns")) \
                and slot["scheduled_end_ns"] > slot["scheduled_ns"]:
            valid.append(slot)
        else:
            invalid += 1
    valid.sort(key=lambda slot: slot["scheduled_ns"])
    return valid, invalid


def av_pairs(presented: list[dict], valid_audio: list[dict]) -> list[tuple[int, float]]:
    """(paint completion ns, signed skew ms) for each frame whose schedule lies in a slot.

    A frame pairs with the audio slot whose half-open scheduled interval holds
    the frame's scheduled time; skew is video delay minus audio delay.
    """
    starts = [slot["scheduled_ns"] for slot in valid_audio]
    out = []
    for frame in presented:
        j = bisect.bisect_right(starts, frame["scheduled_ns"]) - 1
        if j >= 0 and frame["scheduled_ns"] < valid_audio[j]["scheduled_end_ns"]:
            slot = valid_audio[j]
            out.append((frame["completed_ns"],
                        ((frame["completed_ns"] - frame["scheduled_ns"])
                         - (slot["presented_ns"] - slot["scheduled_ns"])) / 1e6))
    return out


def analyze(receiver: list[dict], display: list[dict], sidecar: list[dict] | None, run: dict) -> dict:
    """Evaluate the four #137 criteria over one run. ``run`` is the runner's record.

    Every time comes from the records' own source stamps (#90 review 4): the
    drive is anchored at the ``active`` record's ``emit_ns`` and IDR episodes at
    theirs, so a late-delivered ``active`` or IDR line cannot move or hide an
    episode. ``sidecar`` is the runner's ``idr-events.jsonl`` (None when it is
    missing); a missing or inconsistent sidecar is BLOCKED, never PASS.
    """
    duration_s = run["duration_s"]
    wireless = run.get("transport") == "wireless"
    stamped = idr_records(receiver, display)
    if wireless:
        # #158: the wireless receiver does not stamp emit_ns on its lines yet, so
        # its IDR lines are timed by the runner's sidecar (observed_ns), as before.
        _attach_observed(stamped, sidecar)
    if wireless:
        # #158: no `active` line; the receiver's session_state_changed -> active
        # diagnostic is the marker, timed by its own source stamp.
        active_record = next((e for e in receiver if transport.diag_kind(e) == "session_state_changed"
                              and e.get("to_state") == "active"), None)
        active_ns = None
        if active_record is not None:
            active_ns = integrity.emit_ns(active_record)
            mono = active_record.get("mono_ns")
            if active_ns is None and type(mono) is int and mono > 0:
                active_ns = mono
    else:
        active_record = next((e for e in receiver if e.get("event") == "active"), None)
        active_ns = integrity.emit_ns(active_record) if active_record is not None else None
    anchor_issue = None
    if active_ns is None:
        anchor_issue = "active_unstamped" if active_record is not None else "active_missing"
        active_ns = run.get("active_ns")  # provisional only: every anchored criterion is BLOCKED
    criteria: list[dict] = []
    profile = next((e for e in receiver if e.get("event") == "video_profile"), None)
    negotiated_fps = profile.get("fps") if profile else None
    paints = sorted((e for e in display if e.get("event") == "video_presented"
                     and isinstance(e.get("completed_ns"), int)), key=lambda e: e["completed_ns"])
    # Wireless prepares the phone after `active` (adb reaches it only over the
    # AP): the drive starts at the first paint after that preparation.
    from_ns = run.get("drive_from_ns") or active_ns
    first = next((e["completed_ns"] for e in paints if from_ns and e["completed_ns"] >= from_ns),
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
    # A wireless re-admission is a drop too: the link went away and came back.
    drop_states = ("disconnected", "reconnecting", "failed") if wireless else ("disconnected",)
    states = [e for e in receiver if transport.diag_kind(e) == "session_state_changed"
              and isinstance(e.get("mono_ns"), int)]
    dropped = [e for e in states if e.get("to_state") in drop_states and cutoff and e["mono_ns"] < cutoff]
    if wireless and active_ns and cutoff:
        first_active = next((e for e in states if e.get("to_state") == "active"), None)
        dropped += [e for e in states if first_active
                    and e.get("session") != first_active.get("session")
                    and active_ns <= e["mono_ns"] < cutoff and e not in dropped]
        # Re-admission lines carry no time: the runner's sidecar stamps them (#158).
        dropped += [x["event"] for x in sidecar or [] if x.get("event", {}).get("event") in WIRELESS_STAMPED
                    and type(x.get("observed_ns")) is int and active_ns < x["observed_ns"] < cutoff]
    terminal = [e for e in receiver + display if e.get("event") == "terminal_error"]
    full = bool(end_ns and stopped_ns and stopped_ns >= end_ns)
    survived = bool(run.get("receiver_alive_at_end") and run.get("display_alive_at_end"))
    criteria.append(_criterion("no_session_drop", full and survived and not dropped and not terminal,
                               full_window=full, processes_survived=survived,
                               disconnects=len(dropped), terminal_errors=terminal[:5]))

    # 4. Signed A/V contract (tools/perf/aa_av_contract.py, shared with the soak).
    window_paints = [e for e in paints if active_ns and active_ns <= e["completed_ns"] <= cutoff
                     and isinstance(e.get("scheduled_ns"), int)]
    audio = [e for e in receiver if e.get("event") == "audio_write_completed"]
    valid_audio, invalid_audio = _valid_audio(audio)
    av = av_contract.evaluate(av_pairs(window_paints, valid_audio),
                              [(slot["scheduled_ns"], slot["scheduled_end_ns"]) for slot in valid_audio],
                              start_ns, end_ns, max_clause="cap")  # the smoke keeps max <= 90 ms
    criteria.append(_criterion("av_signed_contract", av.pop("passed"), limits=av_contract.LIMITS,
                               invalid_audio_slots=invalid_audio, **av))

    # #90 review: a record that was lost (or a log never proven complete) can
    # fake or hide a result. A criterion whose inputs may be incomplete is
    # tainted in the direction(s) the loss could move it; a confirmed failure
    # (one no lost record could have produced) stays FAIL, anything a loss could
    # have decided is BLOCKED, never PASS.
    rec = integrity.assess_receiver(receiver)
    disp = integrity.assess_display(display)
    receiver_state = not rec["complete"] or rec["state_lost"]
    receiver_media = not rec["complete"] or rec["media_lost"]
    display_lost = not disp["complete"]
    # Wireless IDR lines may be timed by the sidecar instead (#158); a line with
    # neither time is unstamped on either transport.
    unstamped = [x["event"].get("event") for x in stamped if integrity.emit_ns(x["event"]) is None
                 and not (wireless and type(x.get("observed_ns")) is int)]
    sidecar_issue = None
    if sidecar is None:
        sidecar_issue = "idr_sidecar_missing"
    else:
        raw = {json.dumps(x["event"], sort_keys=True) for x in stamped}
        if any(json.dumps(x.get("event"), sort_keys=True) not in raw for x in sidecar
               if str((x.get("event") or {}).get("event", "")).startswith("idr_")):
            sidecar_issue = "idr_sidecar_mismatch"  # the runner saw an IDR line the log lacks
    by_name = {c["name"]: c for c in criteria}
    tainted: dict[str, str] = {}
    fps = by_name["painted_fps_per_window"]
    # Lost paints can only lower fps (a PASS stands); a lost profile only
    # removes the criterion's input.
    if fps["passed"] is False and (display_lost or (profile is None and receiver_state)):
        tainted["painted_fps_per_window"] = "display_log_incomplete" if display_lost else "receiver_state_log_lost"
    # A lost request or recovery can open or close an episode: either way. A
    # missing sidecar only removes the runner's own copy: with complete,
    # stamped logs a confirmed episode stays FAIL (#90 review 5). A mismatch
    # means the log lacks an IDR record the runner saw, which could close an
    # episode early, so it taints a failure too.
    sidecar_taint = sidecar_issue
    if sidecar_issue == "idr_sidecar_missing" and by_name["waiting_idr_max"]["passed"] is False:
        sidecar_taint = None
    if receiver_state or display_lost or unstamped or sidecar_taint:
        tainted["waiting_idr_max"] = ("idr_events_unstamped" if unstamped else
                                      sidecar_taint if sidecar_taint else
                                      "display_log_incomplete" if display_lost else "receiver_state_log_lost")
    # An observed disconnect or terminal error is confirmed; absence is not.
    drop = by_name["no_session_drop"]
    if drop["passed"] is not False and receiver_state:
        tainted["no_session_drop"] = "receiver_state_log_lost"
    elif drop["passed"] is not False and wireless and sidecar is None:
        # Wireless re-admission evidence lives only in the sidecar.
        tainted["no_session_drop"] = "idr_sidecar_missing"
    # Pairing a subset of slots or frames can move the signed contract either way.
    if receiver_media or display_lost:
        tainted["av_signed_contract"] = "media_telemetry_lost" if receiver_media else "display_log_incomplete"
    if anchor_issue:
        # Without the source-stamped activation the window itself is unknown.
        for name in ("painted_fps_per_window", "waiting_idr_max", "av_signed_contract"):
            tainted[name] = anchor_issue
    confirmed_fail = [c["name"] for c in criteria if c["passed"] is False and c["name"] not in tainted]
    blocked_on = None
    if confirmed_fail:
        verdict = "FAIL"
    elif tainted:
        verdict = "BLOCKED"
        blocked_on = ",".join(f"{name}:{why}" for name, why in sorted(tainted.items()))
    else:
        verdict = "PASS"
    failure = None
    # PASS needs both receiver and display to exit 0 (#163 review, #174). A
    # nonzero exit or a latched stdout logger error (sink failure,
    # pending_lossless overflow) is a confirmed process failure that no lost
    # record could have produced or excused, so it is FAIL even where log loss
    # left criteria BLOCKED. A missing or null exit code only means the run
    # cannot show a clean end: it never passes (FAIL), but it does not override
    # a BLOCKED verdict.
    receiver_exit = run.get("receiver_exit")
    display_exit = run.get("display_exit")
    confirmed_process_failure = False
    if type(receiver_exit) is not int or receiver_exit != 0:
        failure = f"receiver_exit_{'missing' if receiver_exit is None else receiver_exit}"
        confirmed_process_failure = type(receiver_exit) is int
    elif run.get("logger_error"):
        failure = run["logger_error"]
        confirmed_process_failure = True
    if not confirmed_process_failure and (type(display_exit) is not int or display_exit != 0):
        display_confirmed = type(display_exit) is int
        if failure is None or display_confirmed:
            failure = f"display_exit_{'missing' if display_exit is None else display_exit}"
            confirmed_process_failure = display_confirmed
    if failure and (verdict == "PASS" or (verdict == "BLOCKED" and confirmed_process_failure)):
        verdict = "FAIL"
        blocked_on = None
    for c in criteria:
        if c["name"] in tainted:
            c["inconclusive"] = tainted[c["name"]]
    seconds = (min(cutoff, stopped_ns or cutoff) - start_ns) / 1e9 if start_ns and cutoff else None
    return {
        "verdict": verdict,
        **({"blocked_on": blocked_on} if blocked_on else {}),
        **({"failure": failure} if failure else {}),
        "log_integrity": {"receiver": rec, "display": disp, "idr_unstamped": len(unstamped),
                          "idr_sidecar": sidecar_issue or "consistent",
                          "active_anchor": anchor_issue or "emit_ns"},
        "criteria": criteria,
        "negotiated_profile": profile and {k: profile.get(k) for k in ("width", "height", "fps", "codec")},
        "measured": {
            "window_seconds": seconds,
            "startup_to_first_paint_s": (first - active_ns) / 1e9 if first and active_ns else None,
            "painted_fps": (sum(1 for e in window_paints if e["completed_ns"] >= start_ns) / seconds
                            if seconds else None),
            "painted_fps_windows": windows,
            "idr_event_counts": {name: sum(1 for x in stamped if x["event"].get("event") == name)
                                 for name in ("idr_wait_started", "idr_requested", "idr_escalated",
                                              "idr_recovered", "idr_transition_failed")},
            "video_dropped": [e for e in receiver if e.get("event") == "video_dropped"][-10:],
        },
    }


def analyze_dir(out: Path) -> dict:
    run = json.loads((out / "run.json").read_text())
    if "logger_error" not in run and (out / "receiver.log").is_file():
        for line in (out / "receiver.log").read_text(errors="replace").splitlines():
            if line.startswith("stdout_log errno="):
                run = {**run, "logger_error": line.strip()}
                break
    sidecar = out / "idr-events.jsonl"
    return analyze(read_jsonl(out / "receiver.log"), read_jsonl(out / "display.log"),
                   read_jsonl(sidecar) if sidecar.is_file() else None, run)


# ---- Soak gate --------------------------------------------------------------

DIGEST = re.compile(r"[0-9a-f]{64}")
SERIAL = re.compile(r"[A-Za-z0-9._:-]+")
# Harness-only options (sockets, port, store, lifetime, the test control
# channel). Anything else on a command line is configuration: decoder,
# presentation, video profiles, IDR strategy, audio sink, screen geometry...
HARNESS_FLAGS = {
    "receiver": {"--port": 1, "--store": 1, "--require-video-consumer": 0, "--video-socket": 1,
                 "--input-socket": 1, "--duration-ms": 1,
                 # aa-wireless-receiver: the store directory, the approved phone and
                 # the redacted diagnostics the run needs for its active marker.
                 "--store-dir": 1, "--phone-id": 1, "--diagnostics": 0,
                 # The AP channel the helper's ap.conf names: a link parameter of
                 # the target, not a decode/presentation configuration.
                 "--ap-channel": 1},
    "display": {"--test-only-autostart": 0, "--test-only-control-stdin": 0, "--video-socket": 1,
                "--input-socket": 1},
}
DEFAULT_CONFIGURATION = {"receiver_flags": [], "display_flags": [], "receiver_decoder": "default",
                         "display_decoder": "default", "presentation": "default"}


def configuration(receiver_cmd: list[str], display_cmd: list[str]) -> dict:
    """The decode/presentation-relevant configuration of the two launched commands."""
    flags: dict[str, list[str]] = {}
    for role, command in (("receiver", receiver_cmd), ("display", display_cmd)):
        harness, rest, i = HARNESS_FLAGS[role], [], 1
        while i < len(command):
            if command[i] in harness:
                i += 1 + harness[command[i]]
            else:
                rest.append(command[i])
                i += 1
        flags[role] = rest

    def value(args: list[str], *names: str) -> str:
        for i, arg in enumerate(args):
            if arg in names:
                return args[i + 1] if i + 1 < len(args) else ""
        return "default"
    return {"receiver_flags": flags["receiver"], "display_flags": flags["display"],
            "receiver_decoder": value(flags["receiver"], "--decoder"),
            "display_decoder": value(flags["display"], "--decoder", "--decoder-backend"),
            "presentation": value(flags["display"], "--presentation")}


def _finite_number(value: object) -> bool:
    return (isinstance(value, (int, float)) and not isinstance(value, bool)
            and math.isfinite(value))


def _reject_constant(name: str):
    raise ValueError(f"non-standard JSON constant {name}")


def load_record(text: str) -> dict | None:
    """Strict JSON: NaN/Infinity are refused, and the result must be an object."""
    value = json.loads(text, parse_constant=_reject_constant)
    return value if isinstance(value, dict) else None


def max_wireless_startup_budget(duration_s: float = DEFAULT_DURATION_S) -> float:
    """Largest wireless startup wait that fits inside the receiver's --duration-ms cap."""
    return _RECEIVER_MAX_LIFETIME_S - (
        2.0 * duration_s + READY_TIMEOUT_S + FIRST_PAINT_WAIT_S + 30.0
    )


def startup_budget(transport_name: str, override_s: float | None = None, *,
                   duration_s: float = DEFAULT_DURATION_S) -> float:
    """Active-marker wait budget: wired stays 25 s; wireless uses its derived floor or extension."""
    if transport_name == "wireless":
        if not _finite_number(duration_s) or duration_s <= 0:
            raise ValueError("--duration must be finite and positive")
        max_budget = max_wireless_startup_budget(duration_s)
        if max_budget < WIRELESS_STARTUP_BUDGET_S:
            raise ValueError(
                f"--duration {duration_s:g} s leaves only {max_budget:g} s under the "
                f"{_RECEIVER_MAX_LIFETIME_S} s receiver lifetime (< {WIRELESS_STARTUP_BUDGET_S:g} s floor)"
            )
        if override_s is None:
            return WIRELESS_STARTUP_BUDGET_S
        if (not _finite_number(override_s) or override_s < WIRELESS_STARTUP_BUDGET_S
                or override_s > max_budget):
            raise ValueError(
                f"--startup-timeout-seconds for wireless must be in "
                f"[{WIRELESS_STARTUP_BUDGET_S:g}, {max_budget:g}] s for --duration {duration_s:g} s"
            )
        return float(override_s)
    if override_s is not None:
        raise ValueError("--startup-timeout-seconds is only supported for --transport wireless")
    return READY_TIMEOUT_S


def check_record(record: dict | None, *, receiver_sha256: str, display_sha256: str,
                 phone_serial: str, now: datetime,
                 transport_name: str = transport.DEFAULT_TRANSPORT,
                 phone_id: int | None = None,
                 ap_channel: int | None = None,
                 ap_interface: str | None = None) -> tuple[str | None, str]:
    """Return (blocked reason, detail); reason None means the soak may start.

    Both launched binaries must have been hashed (64 lowercase hex digits) and
    both must match the smoke's. The smoke must come from the soak's own phone
    (``phone_serial``, verified on the soak's port by the launcher; empty when
    it could not be) and must have run the default decode/presentation
    configuration, which is the one every soak stage runs. Every field the gate
    uses must be present and well typed; anything missing fails closed.
    """
    if not isinstance(record, dict) or record.get("schema") != SCHEMA:
        return "drive_smoke_missing", f"no {SCHEMA} result document"
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
            or not (record["video_profiles"] is None or isinstance(record["video_profiles"], str))
            or not isinstance(record.get("phone_serial"), str)
            or not SERIAL.fullmatch(record["phone_serial"])
            or not isinstance(record.get("configuration"), dict)
            or set(record["configuration"]) != set(DEFAULT_CONFIGURATION)):
        return "drive_smoke_malformed", "the result lacks a well-typed verdict, binaries, " \
            "duration_s, finished_utc, video_profiles, phone_serial or configuration"
    if transport.record_transport(record) != transport_name:
        # A wired smoke never qualifies a wireless soak, nor the reverse.
        return "drive_smoke_transport_mismatch", (
            f"the smoke ran over {transport.record_transport(record) or 'an unknown transport'!s}, "
            f"the soak is {transport_name}")
    if transport_name == "wireless":
        # The approved wireless phone the soak projects must be the smoke's.
        recorded = record.get("phone_id")
        if type(recorded) is not int or recorded <= 0:
            return "drive_smoke_malformed", "a wireless smoke must record its phone_id"
        if type(phone_id) is not int or recorded != phone_id:
            return "drive_smoke_phone_mismatch", "the smoke projected a different approved phone id"
        recorded_iface = record.get("ap_interface")
        if not transport.valid_interface(recorded_iface):
            return "drive_smoke_malformed", "a wireless smoke must record its ap_interface"
        if not transport.valid_interface(ap_interface) or recorded_iface != ap_interface:
            return "drive_smoke_ap_interface_mismatch", (
                f"the smoke ran on AP interface {recorded_iface!r}, stage uses {ap_interface!r}"
            )
        recorded_channel = record.get("ap_channel")
        if type(recorded_channel) is not int or recorded_channel not in transport.AP_CHANNELS:
            return "drive_smoke_malformed", "a wireless smoke must record its ap_channel"
        expected_channel = transport.DEFAULT_AP_CHANNEL if ap_channel is None else ap_channel
        if (type(expected_channel) is not int or expected_channel not in transport.AP_CHANNELS
                or recorded_channel != expected_channel):
            return "drive_smoke_ap_channel_mismatch", (
                f"the smoke ran on AP channel {recorded_channel}, stage uses {expected_channel}"
            )
        # A wireless PASS counts only with its link evidence: adb bound to the
        # admitted phone, and this host's USB bus readable and clean (no phone,
        # no AOA device) before, at every sample during, and after the drive.
        gap = transport.link_gap(record.get("link"))
        if gap is not None:
            return "drive_smoke_link_unproven", f"the wireless smoke's link record: {gap}"
    if record["verdict"] != "PASS":
        return "drive_smoke_not_passed", f"verdict {record['verdict']!r}"
    for role, digest in (("receiver", receiver_sha256), ("display", display_sha256)):
        if binaries[role]["sha256"] != digest:
            return "drive_smoke_build_mismatch", f"{role} sha256 differs from the smoke's"
    if not isinstance(phone_serial, str) or not SERIAL.fullmatch(phone_serial):
        return "drive_smoke_phone_unverified", "the soak's phone serial is not verified on its port"
    if record["phone_serial"] != phone_serial:
        return "drive_smoke_phone_mismatch", "the smoke ran on a different phone than the soak's"
    if record["configuration"] != DEFAULT_CONFIGURATION:
        return "drive_smoke_configuration", ("the smoke ran a non-default decode/presentation "
                                             "configuration; the soak runs the default")
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
    return None, f"PASS {age / 3600:.1f} h ago on the same phone and receiver/display bytes"


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
                                  display_sha256=args.display_sha256,
                                  phone_serial=args.phone_serial, now=now,
                                  transport_name=args.transport, phone_id=args.phone_id,
                                  ap_channel=args.ap_channel, ap_interface=args.ap_interface)
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
        self.logger_error: str | None = None

    def run(self) -> None:
        with self.log.open("w") as out:
            for line in self.stream:
                now = time.monotonic_ns()
                out.write(line)
                if not line.startswith("{"):
                    if self.logger_error is None and line.startswith("stdout_log errno="):
                        self.logger_error = line.strip()
                    continue
                if line.startswith('{"event":"audio_write_completed"'):
                    self.seen["last_audio"] = now  # media keep-alive (MEDIA_RELAUNCH_SILENCE_S)
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
                if (transport.diag_kind(event) == "session_state_changed"
                        and event.get("to_state") == "active"):
                    # The wireless receiver's active marker (it logs no "active" line).
                    self.seen.setdefault("diag_active", now)
                if name:
                    self.seen.setdefault(name, now)
                if name.startswith("idr_") or name in WIRELESS_STAMPED:
                    with self.lock:
                        self.stamped.write(json.dumps({"observed_ns": now, "log": self.label,
                                                       "event": event}) + "\n")
                        self.stamped.flush()


# Untimed wireless receiver lines the runner stamps (session re-admission evidence).
WIRELESS_STAMPED = ("wireless_admission", "wireless_admission_expired")
ADB_BIND_TIMEOUT_S = 20.0
HOST_USB_PERIOD_S = 10.0


def _host_adb(*args: str) -> subprocess.CompletedProcess:
    """Host-level ``adb`` (connect/disconnect), bounded like every phone command."""
    # The same hard bound (TERM, KILL, abandoned pipes) as every phone command:
    # `adb connect` may start the adb server, which would hold a captured pipe.
    return moving_map.run_adb(["adb", *args])


def _adb(serial: str, *args: str) -> subprocess.CompletedProcess:
    """``adb -s SERIAL`` bounded at 20 s; a timeout or missing adb is a failed result."""
    return moving_map.adb_runner(serial)(*args)


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


def _send_control(fd: int, data: bytes, stop: StopFlag,
                  timeout: float = CONTROL_WRITE_TIMEOUT_S) -> str:
    """aa_moving_map.send_control with this runner's stop flag."""
    return moving_map.send_control(fd, data, lambda: bool(stop), timeout)


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
    if not moving_map.media_package(args.media_component):
        # The cleanup must be able to stop and verify the player (#137).
        print("usage error: --media-component PKG/ACTIVITY is required, so the media player "
              "can be stopped and verified after the drive", file=sys.stderr)
        return 2
    out = Path(args.out)
    out.mkdir(mode=0o700, parents=True, exist_ok=True)
    receiver_bin, display_bin = Path(args.receiver), Path(args.display)
    wireless = args.transport == "wireless"
    # The wireless receiver needs an absolute --store-dir.
    store = Path(args.store).absolute() if wireless else Path(args.store)
    base: dict = {"schema": SCHEMA, "transport": args.transport, "duration_s": args.duration,
                  "port": None if wireless else args.port,
                  "video_profiles": args.video_profiles, "criteria_constants": {
                      "window_s": WINDOW_S, "painted_fps_min_ratio": PAINTED_FPS_MIN_RATIO,
                      "fps_startup_grace_s": FPS_STARTUP_GRACE_S,
                      "waiting_idr_max_s": WAITING_IDR_MAX_S,
                      "av_mean_window_s": AV_MEAN_WINDOW_S, "av_mean_max_abs_ms": AV_MEAN_MAX_ABS_MS,
                      "av_p99_abs_max_ms": AV_P99_ABS_MAX_MS, "av_max_abs_ms": AV_MAX_ABS_MS,
                      "av_coverage_min": AV_COVERAGE_MIN},
                  "qualified_profile": QUALIFIED_PROFILE if args.video_profiles is None else None}
    if wireless:
        base["phone_id"] = args.phone_id
        base["ap_interface"] = args.ap_interface
        base["ap_channel"] = (
            args.ap_channel if args.ap_channel is not None else transport.DEFAULT_AP_CHANNEL
        )
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
    if wireless:
        # Android Auto suppresses wireless while the phone is on USB: the cable
        # must be out, shown from this host's own USB bus, not over adb.
        base["link"] = {"adb_bound": False, "before": None, "after": None,
                        "host_usb": {"before": transport.host_usb_evidence(args.serial),
                                     "during": [], "after": None}}
        refusal = transport.host_usb_refusal(base["link"]["host_usb"]["before"])
        if refusal:
            base["phone_serial"] = args.serial
            return _blocked(out, base, refusal)
    else:
        devices = subprocess.run(["adb", "devices", "-l"], capture_output=True, text=True,
                                 timeout=20).stdout
        line = next((entry for entry in devices.splitlines() if entry.split()[:1] == [args.serial]), "")
        if " device " not in f" {line} " or f"usb:{args.port}" not in line.split():
            return _blocked(out, base, "phone_absent_on_port")
        base["phone_model"] = _adb(args.serial, "shell", "getprop", "ro.product.model").stdout.strip()
    # The soak gate binds to this phone. The record is a local file and must not
    # be committed or exported (runbook, Privacy). Wireless: the expected serial,
    # proven over the AP link (ro.serialno of the receiver's session peer) first.
    base["phone_serial"] = args.serial
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
    active_timeout = startup_budget(
        args.transport, getattr(args, "startup_timeout_seconds", None), duration_s=args.duration
    )
    lifetime_ms = str(int((2 * args.duration + READY_TIMEOUT_S + active_timeout
                           + FIRST_PAINT_WAIT_S + 30) * 1000))
    if wireless:
        # The receiver acquires the helper's AP and bootstraps the phone over
        # Bluetooth when the display asks to project; it releases both on stop.
        receiver_cmd = [str(receiver_bin), "--store-dir", str(store.parent),
                        "--phone-id", str(args.phone_id), "--diagnostics",
                        *(["--ap-channel", str(args.ap_channel)] if args.ap_channel else []),
                        "--video-socket", socket + ".video", "--input-socket", socket + ".input",
                        *profiles, "--duration-ms", lifetime_ms]
    else:
        receiver_cmd = [str(receiver_bin), "--port", args.port, "--store", str(store),
                        "--require-video-consumer", "--video-socket", socket + ".video",
                        "--input-socket", socket + ".input", *profiles, "--duration-ms", lifetime_ms]
    display_cmd = [str(display_bin), "--test-only-autostart", "--test-only-control-stdin",
                   "--video-socket", socket + ".video", "--input-socket", socket + ".input", *profiles]
    base["configuration"] = configuration(receiver_cmd, display_cmd)
    run = {"duration_s": args.duration, "transport": args.transport,
           "startup_budget_s": active_timeout,
           "receiver_command": receiver_cmd, "display_command": display_cmd,
           "started_utc": datetime.now(timezone.utc).isoformat(), "media_relaunches": 0}
    lock = threading.Lock()
    loggers: list[Logger] = []
    children: list[subprocess.Popen] = []
    stop = StopFlag()
    previous = {sig: signal.signal(sig, stop) for sig in STOP_SIGNALS}
    stamped = (out / "idr-events.jsonl").open("w")

    # Wired: adb by the USB serial. Wireless: the <ip>:5555 bound after `active`;
    # until then nothing is sent to the phone.
    adb_target = {"serial": None if wireless else args.serial}

    def phone_adb(*a: str) -> subprocess.CompletedProcess:
        if adb_target["serial"] is None:
            return subprocess.CompletedProcess(["adb", *a], 1, "", "adb not bound to the phone")
        return _adb(adb_target["serial"], *a)

    workload = moving_map.MovingMap(phone_adb, media_uri=args.media_uri,
                                    media_type=args.media_type,
                                    media_component=args.media_component, geo_uri=args.geo_uri,
                                    usb_reset=not wireless)

    def bind_phone(receiver: subprocess.Popen) -> bool:
        """Wireless: adb to the receiver's admitted phone over the AP link, then its link state."""
        interface = args.ap_interface
        deadline, reason = time.monotonic() + ADB_BIND_TIMEOUT_S, "adb_unreachable"
        while not stop and time.monotonic() < deadline:
            if receiver.poll() is not None:
                # A receiver crash is a product failure, not an adb harness block.
                run["failure"] = "receiver exited before adb was bound"
                return False
            candidate, reason = transport.session_candidate(
                transport.receiver_session_peers(receiver.pid), transport.read_ap_stations(interface))
            bound = None
            if candidate:
                bound, reason = transport.bind_adb(args.serial, [candidate], _host_adb)
            if bound:
                adb_target["serial"] = bound
                base["link"]["adb_bound"] = True
                base["link"]["before"] = transport.link_snapshot(phone_adb)
                base["phone_model"] = phone_adb("shell", "getprop", "ro.product.model").stdout.strip()
                return True
            stop.sleep(1.0)
        run["adb_failure"] = reason
        return False

    def sample_host_usb() -> None:
        during = base["link"]["host_usb"]["during"]
        if len(during) < 1000:  # 10 s period: a 2 h stage is 720 samples
            during.append(transport.host_usb_evidence(args.serial))

    def drive() -> None:
        if not wireless and (not workload.prepare_phone(stop.sleep) or stop):
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
        control = display.stdin.fileno()
        os.set_blocking(control, False)  # writes go through aa_moving_map.send_control only
        loggers.append(Logger(display.stdout, out / "display.log", stamped, lock, "display"))
        loggers[-1].start()
        dx = loggers[1].seen
        marker = "diag_active" if wireless else "active"
        # Wired keeps READY_TIMEOUT_S (25 s); wireless uses its derived admission/bootstrap budget.
        if not _wait(lambda: marker in rx, active_timeout, stop, receiver, display):
            run["failure"] = "no active session within the startup budget"
            return
        run["active_ns"] = rx[marker]
        run["active_marker"] = marker
        if wireless:
            if not bind_phone(receiver) or stop:
                return
            if not workload.prepare_phone(stop.sleep) or stop:
                return
            run["drive_from_ns"] = time.monotonic_ns()
            sample_host_usb()
            dx.pop("first_paint", None)  # the drive is the first paint after preparation
        _wait(lambda: "first_paint" in dx, FIRST_PAINT_WAIT_S, stop, receiver, display)
        span = int(args.duration * 1e9) + 5 * 10**8
        end = dx.get("first_paint", time.monotonic_ns()) + span
        usb_due = time.monotonic() + HOST_USB_PERIOD_S
        print(f"drive smoke: active, window {args.duration} s", file=sys.stderr, flush=True)
        while (not stop and time.monotonic_ns() < end
               and receiver.poll() is None and display.poll() is None):
            if "first_paint" in dx:  # a late first paint restarts the drive clock
                end = max(end, dx["first_paint"] + span)
            # Drag the map once a second and keep media playing for the whole
            # drive; coverage (AV_COVERAGE_MIN) still judges any gaps.
            outcome = workload.step(control, last_audio_ns=rx.get("last_audio"),
                                    since_ns=run["active_ns"], stopped=lambda: bool(stop),
                                    sleep=stop.sleep)
            if outcome == "stalled":
                run["failure"] = "display control input stalled"
                break
            if outcome == "closed":
                run["failure"] = "display control input closed"
                break
            if outcome == "phone_failed":
                break  # workload.phone_failure: BLOCKED below
            if wireless and time.monotonic() >= usb_due:
                sample_host_usb()
                usb_due = time.monotonic() + HOST_USB_PERIOD_S
            time.sleep(0.1)
        run["window_end_ns"] = time.monotonic_ns()
        run["receiver_alive_at_end"] = receiver.poll() is None
        run["display_alive_at_end"] = display.poll() is None

    try:
        drive()
    finally:
        # The single cleanup, reached by normal flow (signals only set the flag)
        # or by an exception: every registered child's group, then the phone.
        # Wireless restores the phone first: adb reaches it only while the
        # session (and so the AP link) is up. Nothing was sent before the bind.
        if wireless:
            run["cleanup_failures"] = (workload.restore_phone() if adb_target["serial"]
                                       else [])
            if adb_target["serial"]:
                base["link"]["after"] = transport.link_snapshot(phone_adb)
        for process in reversed(children):
            _stop(process)
        for logger in loggers:
            logger.join(timeout=5)
        stamped.close()
        if wireless:
            if adb_target["serial"]:
                _host_adb("disconnect", adb_target["serial"])
            host = base["link"]["host_usb"]
            host["after"] = transport.host_usb_evidence(args.serial)
            for moment, evidence in [("during", e) for e in host["during"]] + [("after", host["after"])]:
                refusal = transport.host_usb_refusal(evidence)
                if refusal:
                    run["cleanup_failures"].append(f"{refusal.split(':')[0]}_{moment}: {refusal}")
                    break
        else:
            # Media stopped and verified first, USB mode last (it re-enumerates USB).
            run["cleanup_failures"] = workload.restore_phone()
        base["cleanup_failures"] = run["cleanup_failures"]  # in every result, BLOCKED too
        for failure in run["cleanup_failures"]:
            print(f"drive smoke: cleanup failure: {failure}", file=sys.stderr)
        run["media_relaunches"] = workload.media_relaunches
        run["phone_failure"] = workload.phone_failure
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
    if loggers and loggers[0].logger_error:
        run["logger_error"] = loggers[0].logger_error
    run.update(receiver_exit=receiver.returncode if receiver else None,
               display_exit=display.returncode if display else None)
    (out / "run.json").write_text(json.dumps(run, indent=2) + "\n")
    if run.get("adb_failure"):
        # Wireless: adb never reached the expected phone over the AP link, so
        # the workload could not run on the projected phone. Fail closed.
        reason = run["adb_failure"].split(":")[0]
        print(f"drive smoke: {run['adb_failure']}", file=sys.stderr)
        for sig, handler in previous.items():
            signal.signal(sig, handler)
        return _blocked(out, base, reason)
    if workload.phone_failure is not None:
        # A required phone command (setup or media relaunch) failed: the harness
        # lost the phone, so the drive was not the one the gate asks for.
        # BLOCKED (nothing qualifying was measured), never a PASS or a FAIL.
        print(f"drive smoke: phone command failed: {workload.phone_failure}", file=sys.stderr)
        base["phone_failure"] = workload.phone_failure
        for sig, handler in previous.items():
            signal.signal(sig, handler)
        return _blocked(out, base, "phone_command_failed")
    # analyze() fails a PASS on a nonzero or missing receiver/display exit and on a latched
    # logger error, reading the run.json written just above.
    result = {**base, **analyze_dir(out), "finished_utc": datetime.now(timezone.utc).isoformat()}
    result["media_relaunches"] = run["media_relaunches"]
    result["cleanup_failures"] = run["cleanup_failures"]
    if run.get("failure"):
        result["failure"] = run["failure"]
        if result["verdict"] == "PASS":  # a runner-detected failure never passes
            result["verdict"] = "FAIL"
    store_after = sha256(store) if store.exists() else None
    result["trust_store_unchanged"] = store_before == store_after
    if not result["trust_store_unchanged"]:
        result["verdict"] = "FAIL"
    cleanup = moving_map.cleanup_gaps(run["cleanup_failures"])
    if cleanup and result["verdict"] == "PASS":
        # The phone was not shown restored (the player still running, or USB not
        # reset): never a PASS, so the soak gate refuses it. A FAIL stays a FAIL.
        result.update(verdict="BLOCKED", blocked_on=",".join(cleanup))
    (out / "result.json").write_text(json.dumps(result, indent=2) + "\n")
    _print_summary(result)
    for sig, handler in previous.items():
        signal.signal(sig, handler)
    return {"PASS": 0, "BLOCKED": 3}.get(result["verdict"], 1)


def _print_summary(result: dict) -> None:
    print(f"drive smoke: {result['verdict']}")
    for c in result.get("criteria", []):
        state = {True: "pass", False: "FAIL", None: "n/a"}[c["passed"]]
        extra = {k: v for k, v in c.items() if k in (
            "min_fps", "floor_fps", "longest_s", "pairs", "reason", "windows", "clauses",
            "coverage", "abs_p99_ms", "abs_max_ms", "worst_window_mean_ms")}
        print(f"  {state:4} {c['name']} {json.dumps(extra)}")


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    sub = parser.add_subparsers(dest="command", required=True)
    run = sub.add_parser("run", help="drive the smoke on the phone (live)")
    run.add_argument("--serial", required=True,
                     help="the phone's serial: its adb serial when wired; wireless: the hardware "
                          "serial (getprop ro.serialno) the phone must answer with over the AP link")
    run.add_argument("--transport", choices=transport.TRANSPORTS, default=transport.DEFAULT_TRANSPORT,
                     help="session transport (default wired); a result counts only for its own")
    run.add_argument("--phone-id", type=int,
                     help="wireless: the approved wireless phone id in the trust store")
    run.add_argument("--ap-interface",
                     help="wireless: the helper's AP interface (the one ap.conf names, e.g. wlan0); "
                          "required, since ap.conf is root-only and never read")
    run.add_argument("--ap-channel", type=int, choices=transport.AP_CHANNELS,
                     help="wireless: the AP channel ap.conf names (the receiver's default is 36)")
    run.add_argument("--startup-timeout-seconds", type=float, default=None,
                     help=f"wireless: extend the startup budget past its derived "
                          f"{int(WIRELESS_STARTUP_BUDGET_S)} s floor (never shorten; wired stays "
                          f"{int(READY_TIMEOUT_S)} s)")
    run.add_argument("--port", default=os.environ.get("AA_PERF_PORT", "1-2.1"),
                     help="USB port context the phone is on (default 1-2.1)")
    run.add_argument("--build-dir", default=os.environ.get("AA_BUILD_DIR"),
                     help="optimized build directory that produced the binaries")
    run.add_argument("--receiver", help="default: <build-dir>/aa-wired-receiver, or "
                     "aa-wireless-receiver with --transport wireless")
    run.add_argument("--display", help="default: <build-dir>/aa-dummy-display")
    run.add_argument("--store", default=str(Path.home() / ".local/state/android-auto-receiver/"
                                            "approved-phones.json"))
    run.add_argument("--duration", type=int, default=DEFAULT_DURATION_S)
    run.add_argument("--display-env", help="DISPLAY[,XAUTHORITY] of the owner's session")
    run.add_argument("--video-profiles", help="diagnostic override; such a run never satisfies the gate")
    run.add_argument("--geo-uri", default="geo:0,0?q=Atlanta")
    run.add_argument("--media-uri", required=True,
                     help="media to play on the phone for the whole drive (A/V coverage is gated)")
    run.add_argument("--media-type")
    run.add_argument("--media-component", required=True,
                     help="explicit PKG/ACTIVITY for --media-uri; required: the cleanup stops "
                          "and verifies exactly this player")
    run.add_argument("--out", required=True, help="new private directory for logs and result.json")
    analyze_p = sub.add_parser("analyze", help="re-analyze a run directory")
    analyze_p.add_argument("dir")
    check = sub.add_parser("check", help="soak gate: is there a fresh PASS for these bytes?")
    check.add_argument("--record", required=True)
    check.add_argument("--receiver-sha256", required=True)
    check.add_argument("--display-sha256", required=True)
    check.add_argument("--phone-serial", required=True,
                       help="the soak phone's serial, verified on its port; empty if it could not be")
    check.add_argument("--now", help="ISO-8601 time; tests and offline review only, never live")
    check.add_argument("--transport", choices=transport.TRANSPORTS, default=transport.DEFAULT_TRANSPORT,
                       help="the soak's transport; a smoke of another transport is refused")
    check.add_argument("--phone-id", type=int, help="wireless: the soak's approved phone id")
    check.add_argument("--ap-interface", help="wireless: the stage's AP interface")
    check.add_argument("--ap-channel", type=int, choices=transport.AP_CHANNELS,
                       help="wireless: the stage's AP channel (default 36)")
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
    try:
        startup_budget(args.transport, args.startup_timeout_seconds, duration_s=args.duration)
    except ValueError as error:
        parser.error(str(error))
    if args.transport == "wireless":
        if not args.phone_id or args.phone_id <= 0:
            parser.error("--transport wireless needs --phone-id (the approved wireless phone)")
        if not transport.valid_interface(args.ap_interface):
            parser.error("--transport wireless needs --ap-interface IFACE (the helper's AP "
                         "interface; its ap.conf is root-only and the harness never reads it)")
        if Path(args.store).name != "approved-phones.json":
            parser.error("--transport wireless: --store must name <dir>/approved-phones.json "
                         "(the wireless receiver takes the directory)")
    args.receiver = args.receiver or str(Path(args.build_dir) / transport.RECEIVER_BINARY[args.transport])
    args.display = args.display or str(Path(args.build_dir) / "aa-dummy-display")
    return _run_cli(args)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
