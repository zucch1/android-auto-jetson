#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# /// script
# requires-python = ">=3.12"
# dependencies = []
# ///
# Run: python3 -B tests/perf/test_moving_map.py
"""The shared moving-map workload, alone and inside the soak collector (#137)."""
from __future__ import annotations

import argparse
import os
import sys
import tempfile
import time
from pathlib import Path
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools/perf"))
import aa_moving_map as mm  # noqa: E402
import aa_projection_collect as collector  # noqa: E402

FAKE_RECEIVER = ROOT / "tests/perf/fixtures/wired_perf/fake_receiver.py"
REAL_SLEEP = time.sleep


class Adb:
    """Records adb calls (the argv after ``adb -s SERIAL``)."""

    def __init__(self) -> None:
        self.calls: list[tuple[str, ...]] = []

    def __call__(self, *args: str) -> None:
        self.calls.append(args)

    def intents(self, needle: str) -> int:
        return sum(1 for call in self.calls if needle in " ".join(call))


def workload(adb: Adb) -> mm.MovingMap:
    return mm.MovingMap(adb, media_uri="file:///sdcard/Music/fixture.opus", media_type="audio/ogg",
                        media_component="org.example.player/.Play")


def test_drag_lines_alternate_direction() -> None:
    first, second = mm.drag_lines(0), mm.drag_lines(1)
    assert first[0] == "touch down 440 260" and first[-1] == "touch up 700 260", first
    assert second[0] == "touch down 700 260" and second[-1] == "touch up 440 260", second
    assert len(first) == mm.DRAG_STEPS + 2


def test_media_is_required() -> None:
    try:
        mm.MovingMap(Adb(), media_uri="")
    except ValueError:
        return
    raise AssertionError("a moving-map workload without media was accepted")


def test_prepare_and_restore_the_phone() -> None:
    adb = Adb()
    work = workload(adb)
    assert work.prepare_phone(lambda seconds: True)
    order = [" ".join(call) for call in adb.calls]
    assert order[0] == "shell svc usb setFunctions mtp", order
    media = next(i for i, c in enumerate(order) if "file:///sdcard/Music/fixture.opus" in c)
    maps = next(i for i, c in enumerate(order) if mm.MAPS_PACKAGE in c)
    assert media < maps, "Maps must be started after the media so it is in front"
    assert "-t audio/ogg" in order[media] and "-n org.example.player/.Play" in order[media]
    # A stop during the setup sleeps ends it before Maps.
    stopped = Adb()
    assert not workload(stopped).prepare_phone(lambda seconds: False)
    assert stopped.intents(mm.MAPS_PACKAGE) == 0
    work.restore_phone()
    assert adb.calls[-2:] == [("shell", "svc", "usb", "setFunctions", "mtp"),
                              ("shell", "am", "force-stop", "org.example.player")]


def test_media_relaunch_after_silence_only() -> None:
    adb = Adb()
    work = workload(adb)
    now = time.monotonic_ns()
    silence = int(mm.MEDIA_RELAUNCH_SILENCE_S * 1e9)
    work._relaunched_ns = now - 2 * silence
    # Audio heard 1 s ago: nothing to do.
    assert not work.keep_media_playing(now, now - 1_000_000_000, now - 60 * 10**9)
    # No audio for longer than the silence: media then Maps, counted once.
    assert work.keep_media_playing(now, now - 2 * silence, now - 60 * 10**9)
    assert work.media_relaunches == 1
    last_two = [" ".join(call) for call in adb.calls[-2:]]
    assert "fixture.opus" in last_two[0] and mm.MAPS_PACKAGE in last_two[1], last_two
    # Not again within the interval, even though audio is still silent.
    assert not work.keep_media_playing(time.monotonic_ns(), now - 2 * silence, now - 60 * 10**9)
    assert work.media_relaunches == 1


def test_step_drags_once_per_period_and_reports_stalls() -> None:
    adb = Adb()
    work = workload(adb)
    read_fd, write_fd = os.pipe()
    os.set_blocking(write_fd, False)
    try:
        now = time.monotonic_ns()
        assert work.step(write_fd, last_audio_ns=now, since_ns=now, stopped=lambda: False,
                         sleep=lambda seconds: True) == "sent"
        assert work.step(write_fd, last_audio_ns=now, since_ns=now, stopped=lambda: False,
                         sleep=lambda seconds: True) == "idle"
        sent = os.read(read_fd, 65536).decode().splitlines()
        assert sent == mm.drag_lines(0) and work.drags == 1, sent
        # A full pipe nobody drains is a stall, not a hang.
        while True:
            try:
                os.write(write_fd, b"x" * 65536)
            except BlockingIOError:
                break
        assert mm.send_control(write_fd, b"touch up 1 1\n", lambda: False, timeout=0.3) == "stalled"
        assert mm.send_control(write_fd, b"touch up 1 1\n", lambda: True) == "stopped"
    finally:
        os.close(read_fd)
        os.close(write_fd)


def test_audio_watch_sees_only_new_audio_lines() -> None:
    with tempfile.TemporaryDirectory(prefix="audio-watch-") as directory:
        log = Path(directory) / "receiver.log"
        log.write_text('{"event":"video_presented"}\n')
        watch = mm.AudioWatch(log)
        assert watch.poll() is None
        with log.open("a") as handle:
            handle.write('{"event":"audio_write_completed","scheduled_ns":1}\n{"event":"aud')
        first = watch.poll()
        assert first is not None
        with log.open("a") as handle:
            handle.write('io_chunks"}\n')  # completes a non-audio line
        assert watch.poll() == first


DISPLAY = """#!/usr/bin/env python3
import os, sys
with open(os.environ["FAKE_DISPLAY_INPUT"], "a") as out:
    for line in sys.stdin:
        out.write(line)
        out.flush()
"""
ADB = """#!/usr/bin/env bash
printf '%s\\n' "$*" >> "$FAKE_ADB_LOG"
exit 0
"""


def test_collector_runs_the_workload_for_the_stage() -> None:
    # Given the real collector with a fake receiver, a fake display recording its
    # control input and a fake adb on PATH.
    with tempfile.TemporaryDirectory(prefix="soak-moving-map-") as directory:
        root = Path(directory)
        bin_dir = root / "bin"
        bin_dir.mkdir()
        for name, body in (("adb", ADB), ("fake-display", DISPLAY)):
            (bin_dir / name).write_text(body)
            (bin_dir / name).chmod(0o755)
        env = {"PATH": f"{bin_dir}:{os.environ['PATH']}", "FAKE_ADB_LOG": str(root / "adb.log"),
               "FAKE_DISPLAY_INPUT": str(root / "display-input.log")}
        args = argparse.Namespace(
            receiver=str(FAKE_RECEIVER), display=str(bin_dir / "fake-display"), no_display=False,
            port="1-2.1", duration_seconds=4, interval=1, startup_timeout_seconds=5, store=None,
            video_socket=None, input_socket=None, state_dir=str(root / "state"),
            phone_serial="TESTPHONE01", workload="moving-map",
            media_uri="file:///sdcard/Music/fixture.opus", media_type=None, media_component=None,
            geo_uri=None, thermal_policy="strict-all-zones")
        with patch.dict(os.environ, env), \
                patch.object(collector, "_display_connected", return_value=True), \
                patch.object(mm, "MEDIA_RELAUNCH_SILENCE_S", 1.0), \
                patch.object(collector.time, "sleep", side_effect=lambda s: REAL_SLEEP(min(s, 0.5))):
            capture = collector.collect(args)
        adb = (root / "adb.log").read_text().splitlines()
        drags = (root / "display-input.log").read_text().splitlines()
    # Then the phone was set up and restored through adb -s, the map was dragged
    # through the display's control channel, media was kept playing (the fake
    # receiver logs no audio), and the capture is v3 with the workload record.
    assert all(line.startswith("-s TESTPHONE01 ") for line in adb), adb
    assert adb[0] == "-s TESTPHONE01 shell svc usb setFunctions mtp", adb
    assert adb[-1] == "-s TESTPHONE01 shell svc usb setFunctions mtp", adb
    assert any(mm.MAPS_PACKAGE in line for line in adb), adb
    assert drags and drags[:len(mm.drag_lines(0))] == mm.drag_lines(0), drags[:10]
    record = capture["workload"]
    assert record["kind"] == "moving-map" and record["stimulus_started"] is True, record
    assert record["drags"] >= 1 and record["media_relaunches"] >= 1, record
    assert capture["schema"] == "aa-projection-perf-capture/3"


def test_collector_refuses_a_workload_without_its_inputs() -> None:
    for argv in (["--workload", "moving-map", "--display", "d", "--media-uri", "m"],
                 ["--workload", "moving-map", "--display", "d", "--phone-serial", "S"],
                 ["--workload", "moving-map", "--media-uri", "m", "--phone-serial", "S"],
                 ["--workload", "moving-map", "--media-uri", "m", "--phone-serial", "S",
                  "--display", "d", "--no-display"]):
        code = collector._cli(["--receiver", "r", "--port", "1-2.1", "--duration-seconds", "1", *argv])
        assert code == 2, (argv, code)


if __name__ == "__main__":
    tests = [value for name, value in list(globals().items()) if name.startswith("test_")]
    for test in tests:
        test()
    print(f"PASS {len(tests)} moving-map workload tests")
