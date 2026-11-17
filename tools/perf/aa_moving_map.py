#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# /// script
# requires-python = ">=3.12"
# dependencies = []
# ///
"""Moving-map workload shared by the drive smoke and the soak stages (#137).

One implementation of the phone and display choreography:

* ``prepare_phone``: reset the phone's USB mode, wake it, start the media and
  then Google Maps (``geo:`` intent), so Maps is in front;
* ``step``: once per ``STIMULUS_PERIOD_S``, drag the map across the display
  through its test control channel (``--test-only-control-stdin``), and keep
  the media playing: after ``MEDIA_RELAUNCH_SILENCE_S`` without an audio write
  the media intent is re-sent, then Maps (at most once per interval), counted
  in ``media_relaunches``;
* ``restore_phone``: close the intent gate (no media or Maps intent after
  it), force-stop the media player and verify it is gone (``pidof``, bounded
  retries), and only then reset the USB mode: ``svc usb setFunctions``
  re-enumerates USB and breaks adb for a moment, so nothing may follow it.
  Every step that cannot be verified is a named ``cleanup_failures`` entry.

Every required phone command is checked: a nonzero exit, a timeout or an
``am start`` that reports ``Error`` sets ``phone_failure`` and ends the
workload (no further intent or drag). The phone is harness, not the product,
so the soak reporter BLOCKS such a stage (``run_evidence:moving_map_phone``).

tools/wired-qa/aa_drive_smoke.py drives it from its run loop; the soak
collector (tools/perf/aa_projection_collect.py) runs it in a ``SoakStimulus``
thread for the whole stage. Every phone command goes through ``adb -s SERIAL``.
"""
from __future__ import annotations

import os
import re
import select
import subprocess
import threading
import time
from collections.abc import Callable
from pathlib import Path

STIMULUS_PERIOD_S = 1.0
# Drag from x=A to x=B at y=260 in DRAG_STEPS moves, DRAG_STEP_S apart, then
# back on the next stimulus: the phone keeps encoding a moving map.
DRAG_POINTS = (440, 700)
DRAG_Y = 260
DRAG_STEPS = 6
DRAG_STEP_S = 0.06
# A display that accepts no touch-control input for this long has stalled its
# GUI thread (tools/dummy-display drains stdin on a GUI-thread timer).
CONTROL_WRITE_TIMEOUT_S = 2.0
# After this long without an audio write the media intent is re-sent.
MEDIA_RELAUNCH_SILENCE_S = 5.0
# Evidence floor for a v3 soak stage: the stimulus must have started and
# completed at least this many drags per second of active window. Nominal is
# about 0.65 Hz (one drag per STIMULUS_PERIOD_S plus its own ~0.5 s); the floor
# leaves room for adb relaunches and still proves the map moved all stage.
MIN_DRAG_RATE_HZ = 0.25
# Continuity: no stretch of the active window may go longer than this without a
# completed drag. Nominal spacing is ~1.4 s (STIMULUS_PERIOD_S plus the drag's
# own ~0.4 s); a media relaunch adds two ``am start`` calls (about 1-3 s each)
# between drags, so a healthy run stays under ~8 s. 10 s keeps that margin and
# still catches a stimulus that stopped (closed pipe, dead thread) mid-stage,
# which the aggregate MIN_DRAG_RATE_HZ alone cannot.
MAX_DRAG_GAP_S = 10.0
# The media force-stop is verified with ``pidof``; on a failed command or a
# still-running player it is retried this many times, this far apart.
MEDIA_STOP_ATTEMPTS = 3
MEDIA_STOP_RETRY_S = 1.0
# Every adb command's bound (adb_runner).
ADB_TIMEOUT_S = 20.0
# The whole restore (media stop attempts and the USB switch) stays within this:
# a media-stop attempt (force-stop + pidof, after its pause) starts only while it
# and the USB switch still fit, so the USB switch always has its ADB_TIMEOUT_S.
# Out of budget, the media stop is recorded as not done (cleanup:media_not_stopped).
RESTORE_BUDGET_S = 90.0
MAPS_PACKAGE = "com.google.android.apps.maps"
DEFAULT_GEO_URI = "geo:0,0?q=Atlanta"
AUDIO_LINE = b'{"event":"audio_write_completed"'


def adb_runner(serial: str, timeout: float = ADB_TIMEOUT_S) -> Callable[..., subprocess.CompletedProcess]:
    """``adb -s SERIAL ...`` with a bound; a missing or hung adb never raises.

    A timeout or a missing adb comes back as a failed CompletedProcess (exit 1).
    """
    def run(*args: str) -> subprocess.CompletedProcess:
        try:
            return subprocess.run(["adb", "-s", serial, *args], capture_output=True, text=True,
                                  timeout=timeout)
        except (OSError, subprocess.TimeoutExpired) as error:
            return subprocess.CompletedProcess(["adb", *args], 1, "", str(error))
    return run


def command_failure(result: object) -> str | None:
    """Why an adb command failed, or None when it succeeded.

    Anything but a CompletedProcess with exit 0 fails (fail closed), and so does
    output with a line starting ``Error`` (``am start`` can report an activity
    it did not start on stdout with exit 0).
    """
    if not isinstance(result, subprocess.CompletedProcess):
        return "no result"
    output = "\n".join(text for text in (result.stdout, result.stderr) if isinstance(text, str))
    error = next((line.strip() for line in output.splitlines() if line.strip().startswith("Error")),
                 None)
    if result.returncode != 0:
        detail = error or next((line.strip() for line in output.splitlines() if line.strip()), "")
        return f"exit {result.returncode}" + (f": {detail[:200]}" if detail else "")
    return error[:200] if error else None


def send_control(fd: int, data: bytes, stopped: Callable[[], bool],
                 timeout: float = CONTROL_WRITE_TIMEOUT_S) -> str:
    """Write to the display's non-blocking control pipe without ever blocking.

    Returns "sent", "stopped" (a stop was requested), "stalled" (the pipe
    stayed full for ``timeout``) or "closed" (the display's stdin is gone).
    """
    view = memoryview(data)
    deadline = time.monotonic() + timeout
    while view:
        if stopped():
            return "stopped"
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            return "stalled"
        try:
            _, writable, _ = select.select([], [fd], [], min(0.1, remaining))
            if writable:
                view = view[os.write(fd, view):]
        except BlockingIOError:
            continue
        except OSError:
            return "closed"
    return "sent"


def max_drag_gap_s(drags_ns: object, start_ns: int, end_ns: int) -> float | None:
    """Longest stretch of [start_ns, end_ns] without a completed drag, in seconds.

    The edges count (window start to the first drag, the last drag to the end).
    None when the evidence is not a nondecreasing list of integers.
    """
    if (not isinstance(drags_ns, list) or end_ns < start_ns
            or any(not isinstance(t, int) or isinstance(t, bool) for t in drags_ns)
            or any(b < a for a, b in zip(drags_ns, drags_ns[1:]))):
        return None
    points = [start_ns, *(t for t in drags_ns if start_ns < t < end_ns), end_ns]
    return max(b - a for a, b in zip(points, points[1:])) / 1e9


# Every restore failure maps to one named BLOCKED reason.
CLEANUP_REASONS = {
    "media_stop_unverified": "cleanup:media_not_stopped",
    "media_stop_unavailable": "cleanup:media_not_stopped",
    "usb_reset_failed": "cleanup:usb_not_restored",
    "phone_restore_not_reached": "cleanup:phone_restore_not_reached",
}


def cleanup_gaps(raw: object) -> list[str]:
    """The named BLOCKED reasons (cleanup:*) for a run's cleanup failures.

    ``raw`` is a soak capture's moving-map workload record, or a plain list
    (the drive smoke). A missing or malformed record is "cleanup:not_recorded".
    """
    if isinstance(raw, dict) and raw.get("kind") != "moving-map":
        return []
    failures = raw.get("cleanup_failures") if isinstance(raw, dict) else raw
    if not isinstance(failures, list) or not all(isinstance(item, str) for item in failures):
        return ["cleanup:not_recorded"]
    reasons = []
    for item in failures:
        name = item.split(":", 1)[0].strip()
        reason = CLEANUP_REASONS.get(name, "cleanup:" + (re.sub(r"[^a-z0-9_]+", "_", name.lower()) or "unknown"))
        if reason not in reasons:
            reasons.append(reason)
    return reasons


def media_package(component: str | None) -> str | None:
    """The package of an explicit PKG/ACTIVITY component, or None if it has none.

    Live moving-map runs require it: the restore force-stops and verifies
    exactly this package, so without it the player cannot be shown stopped.
    """
    if not isinstance(component, str) or component.count("/") != 1:
        return None
    package, activity = component.split("/")
    parts = package.split(".")
    if (len(parts) < 2 or not activity or not parts[0][:1].isalpha()
            or not all(part and all(c.isalnum() or c == "_" for c in part) for part in parts)):
        return None
    return package


def drag_lines(step: int) -> list[str]:
    """The touch-control lines of drag number ``step`` (alternating direction)."""
    a, b = DRAG_POINTS if step % 2 == 0 else DRAG_POINTS[::-1]
    lines = [f"touch down {a} {DRAG_Y}"]
    lines += [f"touch move {int(a + (b - a) * j / DRAG_STEPS)} {DRAG_Y}"
              for j in range(1, DRAG_STEPS + 1)]
    return lines + [f"touch up {b} {DRAG_Y}"]


class MovingMap:
    """Phone intents, map drags and media keep-alive for one run."""

    def __init__(self, adb: Callable[..., object], *, media_uri: str,
                 media_type: str | None = None, media_component: str | None = None,
                 geo_uri: str = DEFAULT_GEO_URI) -> None:
        if not media_uri:
            raise ValueError("the moving-map workload needs media for the whole run")
        self.adb = adb
        self.media = ["-d", media_uri] + (["-t", media_type] if media_type else [])
        self.media += ["-n", media_component] if media_component else []
        self.media_package = media_package(media_component)
        self.geo_uri = geo_uri
        self.media_relaunches = 0
        self.drags = 0
        # Monotonic time of every completed drag (continuity evidence).
        self.drag_ns: list[int] = []
        # The first required phone command that failed ("<step>: <why>"): the
        # workload is over; nothing more is sent but the restore.
        self.phone_failure: str | None = None
        # What restore_phone could not do or verify (empty: a clean restore).
        self.cleanup_failures: list[str] = []
        self._last_drag = None
        self._relaunched_ns = time.monotonic_ns()
        # Every phone command goes through _phone under this lock. restore_phone
        # takes it, waits for an in-flight command, closes the gate and restores:
        # nothing is ever sent to the phone after its restore.
        self._lock = threading.Lock()
        self._closed = False

    def _phone(self, step: str, *args: str) -> bool:
        """Run one required phone command; False if the gate is closed or it failed."""
        with self._lock:
            if self._closed or self.phone_failure is not None:
                return False
            failure = command_failure(self.adb(*args))
            if failure is not None:
                self.phone_failure = f"{step}: {failure}"
                return False
            return True

    def start_media(self) -> bool:
        return self._phone("media", "shell", "am", "start", "-a", "android.intent.action.VIEW",
                           *self.media)

    def start_maps(self) -> bool:
        return self._phone("maps", "shell", "am", "start", "-a", "android.intent.action.VIEW",
                           "-d", self.geo_uri, "-p", MAPS_PACKAGE)

    def prepare_phone(self, sleep: Callable[[float], bool]) -> bool:
        """USB reset, wake, media, then Maps in front.

        False if ``sleep`` was stopped or a command failed (``phone_failure``).
        """
        if not self._phone("usb_reset", "shell", "svc", "usb", "setFunctions", "mtp") or not sleep(3):
            return False
        if not self._phone("wake", "shell", "input", "keyevent", "KEYCODE_WAKEUP"):
            return False
        if not self.start_media() or not sleep(4):
            return False
        if not self.start_maps():
            return False
        self._relaunched_ns = time.monotonic_ns()
        return True

    def restore_phone(self, sleep: Callable[[float], object] = time.sleep,
                      clock: Callable[[], float] = time.monotonic) -> list[str]:
        """Close the intent gate (after any in-flight command), then restore. Idempotent.

        Order: the gate is closed first (no media or Maps intent can follow),
        then the media player is force-stopped and verified gone, and the USB
        function change comes last, because it re-enumerates USB and breaks
        adb for a moment. Returns (and keeps in ``cleanup_failures``) every
        step that failed or could not be verified; empty means a clean restore.
        """
        with self._lock:
            if self._closed:
                return list(self.cleanup_failures)
            self._closed = True
            failures = []
            if self.media_package:
                failure = self._stop_media(sleep, clock, clock() + RESTORE_BUDGET_S)
                if failure is not None:
                    failures.append(failure)
            else:
                failures.append("media_stop_unavailable: no media component, the player is unknown")
            usb = command_failure(self.adb("shell", "svc", "usb", "setFunctions", "mtp"))
            if usb is not None:
                failures.append(f"usb_reset_failed: {usb}")
            self.cleanup_failures = failures
            return list(failures)

    def _stop_media(self, sleep: Callable[[float], object], clock: Callable[[], float],
                    deadline: float) -> str | None:
        """Force-stop the media package until ``pidof`` shows no process.

        Bounded by MEDIA_STOP_ATTEMPTS and by ``deadline`` (RESTORE_BUDGET_S),
        always leaving the USB switch its ADB_TIMEOUT_S.
        """
        why = "not attempted"
        attempts = 0
        for attempt in range(MEDIA_STOP_ATTEMPTS):
            pause = MEDIA_STOP_RETRY_S if attempt else 0.0
            if deadline - clock() < pause + 3 * ADB_TIMEOUT_S:
                why = f"restore budget exhausted; last: {why}"
                break
            if pause:
                sleep(pause)
            attempts = attempt + 1
            stop = command_failure(self.adb("shell", "am", "force-stop", self.media_package))
            if stop is not None:
                why = f"force-stop {stop}"
                continue
            check = self.adb("shell", "pidof", self.media_package)
            if not isinstance(check, subprocess.CompletedProcess):
                why = "pidof: no result"
                continue
            output = (check.stdout or "").strip()
            if check.returncode == 1 and not output and not (check.stderr or "").strip():
                return None  # pidof found no process: the player is gone
            why = (f"still running (pid {output})" if check.returncode == 0 and output
                   else f"pidof: {command_failure(check) or 'unexpected output ' + output[:80]}")
        return (f"media_stop_unverified: {self.media_package} after {attempts} "
                f"attempts: {why}"[:300])

    def keep_media_playing(self, now_ns: int, last_audio_ns: int | None, since_ns: int,
                           stopped: Callable[[], bool] = lambda: False) -> bool:
        """Re-send media then Maps after MEDIA_RELAUNCH_SILENCE_S without audio."""
        silence = int(MEDIA_RELAUNCH_SILENCE_S * 10**9)
        heard = last_audio_ns if last_audio_ns is not None else since_ns
        if now_ns - heard > silence and now_ns - self._relaunched_ns > silence and not stopped():
            if not self.start_media() or stopped():
                return False  # failed, stopping or restored: never Maps after that
            if not self.start_maps():
                return False
            self._relaunched_ns = time.monotonic_ns()
            self.media_relaunches += 1
            return True
        return False

    def step(self, control_fd: int, *, last_audio_ns: int | None, since_ns: int,
             stopped: Callable[[], bool], sleep: Callable[[float], bool]) -> str:
        """One tick: media keep-alive, and a drag when one is due.

        Returns the control outcome ("sent", "stalled", "stopped", "closed"),
        "phone_failed" once a required phone command failed (the workload is
        over), or "idle" when no drag was due.
        """
        self.keep_media_playing(time.monotonic_ns(), last_audio_ns, since_ns, stopped)
        if self.phone_failure is not None:
            return "phone_failed"
        if stopped():
            return "stopped"
        if self._last_drag is not None and time.monotonic() - self._last_drag < STIMULUS_PERIOD_S:
            return "idle"
        outcome = "sent"
        for line in drag_lines(self.drags):
            outcome = send_control(control_fd, (line + "\n").encode(), stopped)
            if outcome != "sent" or not sleep(DRAG_STEP_S):
                break
        if outcome == "sent":
            self.drags += 1
            self.drag_ns.append(time.monotonic_ns())
        self._last_drag = time.monotonic()
        return outcome


class AudioWatch:
    """Monotonic time of the latest audio write appended to a receiver log."""

    def __init__(self, path: str | os.PathLike[str]) -> None:
        self.path = Path(path)
        self.offset = 0
        self.last_ns: int | None = None
        self._partial = b""

    def poll(self) -> int | None:
        try:
            with open(self.path, "rb") as handle:
                handle.seek(self.offset)
                chunk = handle.read()
        except OSError:
            return self.last_ns
        self.offset += len(chunk)
        lines = (self._partial + chunk).split(b"\n")
        self._partial = lines.pop()
        if any(line.startswith(AUDIO_LINE) for line in lines):
            self.last_ns = time.monotonic_ns()
        return self.last_ns


class SoakStimulus(threading.Thread):
    """Runs a MovingMap against a managed display for a whole soak stage."""

    def __init__(self, workload: MovingMap, control_fd: int, receiver_log: str | os.PathLike[str],
                 since_ns: int) -> None:
        super().__init__(daemon=True, name="soak-stimulus")
        self.workload, self.control_fd, self.since_ns = workload, control_fd, since_ns
        self.audio = AudioWatch(receiver_log)
        self.halt = threading.Event()
        self.failure: str | None = None

    def stop(self) -> None:
        """Stop and join; bounded by the adb timeout of the commands one step can make.

        The collector restores the phone after this; MovingMap's intent gate
        keeps that ordering even if the join had to give up.
        """
        self.halt.set()
        self.join(timeout=2 * 20.0 + CONTROL_WRITE_TIMEOUT_S + 5)

    def _sleep(self, seconds: float) -> bool:
        return not self.halt.wait(seconds)

    def run(self) -> None:
        while not self.halt.is_set():
            outcome = self.workload.step(self.control_fd, last_audio_ns=self.audio.poll(),
                                         since_ns=self.since_ns, stopped=self.halt.is_set,
                                         sleep=self._sleep)
            # The display's control input is part of the product under test: a
            # stalled or unexpectedly closed pipe is a terminal FAIL. A failed
            # phone command ends the workload too; MovingMap.phone_failure
            # carries it (BLOCKED: the harness, not the product).
            if outcome == "stalled":
                self.failure = "display_control_stalled"
                return
            if outcome == "closed":
                self.failure = "display_control_closed"
                return
            if outcome == "phone_failed":
                return
            self.halt.wait(0.1)
