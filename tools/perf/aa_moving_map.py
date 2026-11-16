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
* ``restore_phone``: reset the USB mode and stop the media player.

tools/wired-qa/aa_drive_smoke.py drives it from its run loop; the soak
collector (tools/perf/aa_projection_collect.py) runs it in a ``SoakStimulus``
thread for the whole stage. Every phone command goes through ``adb -s SERIAL``.
"""
from __future__ import annotations

import os
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
MAPS_PACKAGE = "com.google.android.apps.maps"
DEFAULT_GEO_URI = "geo:0,0?q=Atlanta"
AUDIO_LINE = b'{"event":"audio_write_completed"'


def adb_runner(serial: str, timeout: float = 20.0) -> Callable[..., subprocess.CompletedProcess]:
    """``adb -s SERIAL ...`` with a bound; a missing or hung adb never raises."""
    def run(*args: str) -> subprocess.CompletedProcess:
        try:
            return subprocess.run(["adb", "-s", serial, *args], capture_output=True, text=True,
                                  timeout=timeout)
        except (OSError, subprocess.TimeoutExpired) as error:
            return subprocess.CompletedProcess(["adb", *args], 1, "", str(error))
    return run


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
        self.media_package = media_component.split("/")[0] if media_component else None
        self.geo_uri = geo_uri
        self.media_relaunches = 0
        self.drags = 0
        self._last_drag = None
        self._relaunched_ns = time.monotonic_ns()

    def start_media(self) -> None:
        self.adb("shell", "am", "start", "-a", "android.intent.action.VIEW", *self.media)

    def start_maps(self) -> None:
        self.adb("shell", "am", "start", "-a", "android.intent.action.VIEW",
                 "-d", self.geo_uri, "-p", MAPS_PACKAGE)

    def prepare_phone(self, sleep: Callable[[float], bool]) -> bool:
        """USB reset, wake, media, then Maps in front. False if ``sleep`` was stopped."""
        self.adb("shell", "svc", "usb", "setFunctions", "mtp")
        if not sleep(3):
            return False
        self.adb("shell", "input", "keyevent", "KEYCODE_WAKEUP")
        self.start_media()
        if not sleep(4):
            return False
        self.start_maps()
        self._relaunched_ns = time.monotonic_ns()
        return True

    def restore_phone(self) -> None:
        self.adb("shell", "svc", "usb", "setFunctions", "mtp")
        if self.media_package:
            self.adb("shell", "am", "force-stop", self.media_package)

    def keep_media_playing(self, now_ns: int, last_audio_ns: int | None, since_ns: int) -> bool:
        """Re-send media then Maps after MEDIA_RELAUNCH_SILENCE_S without audio."""
        silence = int(MEDIA_RELAUNCH_SILENCE_S * 10**9)
        heard = last_audio_ns if last_audio_ns is not None else since_ns
        if now_ns - heard > silence and now_ns - self._relaunched_ns > silence:
            self.start_media()
            self.start_maps()
            self._relaunched_ns = time.monotonic_ns()
            self.media_relaunches += 1
            return True
        return False

    def step(self, control_fd: int, *, last_audio_ns: int | None, since_ns: int,
             stopped: Callable[[], bool], sleep: Callable[[float], bool]) -> str:
        """One tick: media keep-alive, and a drag when one is due.

        Returns the control outcome ("sent", "stalled", "stopped", "closed"),
        or "idle" when no drag was due.
        """
        self.keep_media_playing(time.monotonic_ns(), last_audio_ns, since_ns)
        if self._last_drag is not None and time.monotonic() - self._last_drag < STIMULUS_PERIOD_S:
            return "idle"
        outcome = "sent"
        for line in drag_lines(self.drags):
            outcome = send_control(control_fd, (line + "\n").encode(), stopped)
            if outcome != "sent" or not sleep(DRAG_STEP_S):
                break
        if outcome == "sent":
            self.drags += 1
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
        self.halt.set()
        self.join(timeout=10)

    def _sleep(self, seconds: float) -> bool:
        return not self.halt.wait(seconds)

    def run(self) -> None:
        while not self.halt.is_set():
            outcome = self.workload.step(self.control_fd, last_audio_ns=self.audio.poll(),
                                         since_ns=self.since_ns, stopped=self.halt.is_set,
                                         sleep=self._sleep)
            if outcome == "stalled":
                self.failure = "display_control_stalled"
                return
            if outcome == "closed":
                return
            self.halt.wait(0.1)
