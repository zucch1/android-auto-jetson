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
import subprocess
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

    def __init__(self, fail: str | None = None, returncode: int = 1, stdout: str = "") -> None:
        self.calls: list[tuple[str, ...]] = []
        self.fail, self.returncode, self.stdout = fail, returncode, stdout

    def __call__(self, *args: str) -> subprocess.CompletedProcess:
        self.calls.append(args)
        if self.fail is not None and self.fail in " ".join(args):
            return subprocess.CompletedProcess(args, self.returncode, self.stdout, "")
        if args[:2] == ("shell", "pidof"):  # nothing running: toybox exits 1, silent
            return subprocess.CompletedProcess(args, 1, "", "")
        return subprocess.CompletedProcess(args, 0, "Starting: Intent { ... }", "")

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
    assert work.restore_phone() == []
    # Media stopped and verified first; the USB function change last.
    assert adb.calls[-3:] == [("shell", "am", "force-stop", "org.example.player"),
                              ("shell", "pidof", "org.example.player"),
                              ("shell", "svc", "usb", "setFunctions", "mtp")]


FAKE_PHONE = ROOT / "tests/perf/fixtures/wired_perf/fake_phone_adb.sh"


def phone_restore(**knobs: str) -> tuple[list[str], list[str], bool]:
    """Set up and restore a fake phone through the real adb_runner.

    Returns (restore failures, adb argv log, whether the player still runs)."""
    with tempfile.TemporaryDirectory(prefix="moving-map-phone-") as directory:
        root = Path(directory)
        (root / "adb").symlink_to(FAKE_PHONE)
        log = root / "adb.log"
        env = {"PATH": f"{root}:{os.environ['PATH']}", "FAKE_ADB_LOG": str(log), **knobs}
        with patch.dict(os.environ, env):
            work = mm.MovingMap(mm.adb_runner("TESTPHONE01"), media_uri="file:///sdcard/Music/fixture.opus",
                                media_component=knobs.get("COMPONENT", "org.example.player/.Play"))
            assert work.prepare_phone(lambda seconds: REAL_SLEEP(0.4) or True)
            failures = work.restore_phone(sleep=lambda seconds: None)
            running = (root / "adb.log.phone" / "running.org.example.player").exists()
        return failures, log.read_text().splitlines(), running


def restore_order(adb: list[str]) -> list[str]:
    tail = adb[next(i for i, line in enumerate(adb) if "force-stop" in line or
                    ("setFunctions" in line and i > 0)):]
    return [" ".join(line.split()[3:5]) for line in tail]


def test_the_live_defect_usb_switch_first_loses_the_force_stop() -> None:
    # The 2026-11-17 A26 interruption: setFunctions then force-stop. The switch
    # re-enumerates USB, the force-stop gets "device not found", VLC lives on.
    with tempfile.TemporaryDirectory(prefix="moving-map-phone-") as directory:
        root = Path(directory)
        (root / "adb").symlink_to(FAKE_PHONE)
        with patch.dict(os.environ, {"PATH": f"{root}:{os.environ['PATH']}",
                                     "FAKE_ADB_LOG": str(root / "adb.log")}):
            adb = mm.adb_runner("TESTPHONE01")
            adb("shell", "am", "start", "-d", "x", "-n", "org.example.player/.Play")
            adb("shell", "svc", "usb", "setFunctions", "mtp")
            stop = adb("shell", "am", "force-stop", "org.example.player")
        assert stop.returncode == 1 and "not found" in stop.stderr, stop
        assert (root / "adb.log.phone" / "running.org.example.player").exists()


def test_restore_stops_and_verifies_media_before_the_usb_switch() -> None:
    failures, adb, running = phone_restore()
    assert failures == [] and not running, (failures, adb)
    assert restore_order(adb) == ["am force-stop", "pidof org.example.player",
                                  "svc usb"], adb
    assert adb[-1] == "-s TESTPHONE01 shell svc usb setFunctions mtp", adb


def test_restore_retries_a_player_that_respawns() -> None:
    failures, adb, running = phone_restore(FAKE_ADB_RESPAWN="org.example.player:2")
    assert failures == [] and not running, (failures, adb)
    assert restore_order(adb) == ["am force-stop", "pidof org.example.player"] * 3 + ["svc usb"], adb


def test_restore_records_a_media_stop_it_cannot_verify() -> None:
    # Given a player that never goes away: bounded attempts, a named failure,
    # and the USB switch still comes last.
    failures, adb, running = phone_restore(FAKE_ADB_STUCK="org.example.player")
    assert running and len(failures) == 1, failures
    assert failures[0].startswith("media_stop_unverified: org.example.player after 3 attempts: "
                                  "still running (pid 20941)"), failures
    assert restore_order(adb) == ["am force-stop", "pidof org.example.player"] * 3 + ["svc usb"], adb
    # Without a media component the player is unknown: recorded, not silent.
    work = mm.MovingMap(Adb(), media_uri="file:///sdcard/Music/fixture.opus")
    assert work.restore_phone() == ["media_stop_unavailable: no media component, the player is unknown"]
    # A failed USB switch is named too.
    adb_fails = Adb(fail="setFunctions")
    work = workload(adb_fails)
    assert work.restore_phone() == ["usb_reset_failed: exit 1"], work.cleanup_failures


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


def test_no_phone_intent_after_restore_even_mid_relaunch() -> None:
    # Given a stimulus whose media relaunch is blocked inside adb (the review's
    # reproduction), and a stop + restore arriving while it is blocked.
    import threading
    entered, release = threading.Event(), threading.Event()
    calls: list[str] = []

    def adb(*args: str) -> subprocess.CompletedProcess:
        line = " ".join(args)
        if "fixture.opus" in line and not release.is_set():
            entered.set()
            release.wait(10)
        calls.append(line)
        return subprocess.CompletedProcess(args, 0, "", "")

    work = workload(adb)
    read_fd, write_fd = os.pipe()
    os.set_blocking(write_fd, False)
    stimulus = mm.SoakStimulus(work, write_fd, "/nonexistent/receiver.log",
                               since_ns=time.monotonic_ns() - 60 * 10**9)
    work._relaunched_ns = time.monotonic_ns() - 60 * 10**9
    try:
        stimulus.start()
        assert entered.wait(10), "the relaunch never started"
        # When the collector stops the stimulus and restores the phone meanwhile.
        stopper = threading.Thread(target=stimulus.stop)
        stopper.start()
        restorer = threading.Thread(target=work.restore_phone)
        restorer.start()
        time.sleep(0.3)
        assert restorer.is_alive(), "restore ran while a phone command was in flight"
        release.set()
        restorer.join(10)
        stopper.join(60)
        assert not stimulus.is_alive()
        # Then the media intent finished first, the restore came next, and no
        # intent (no Maps relaunch) was sent after it, then or later.
        restore = calls.index("shell svc usb setFunctions mtp")
        assert all("am start" not in line for line in calls[restore:]), calls
        assert not any(mm.MAPS_PACKAGE in line for line in calls), calls
        assert not work.start_maps() and not work.start_media()
        assert calls[-3:] == ["shell am force-stop org.example.player",
                              "shell pidof org.example.player",
                              "shell svc usb setFunctions mtp"], calls
    finally:
        release.set()
        os.close(read_fd)
        os.close(write_fd)


def test_relaunch_honours_a_stop_between_its_intents() -> None:
    adb = Adb()
    work = workload(adb)
    work._relaunched_ns = 0
    flags = iter([False, True])  # not stopped before media, stopped before Maps
    assert not work.keep_media_playing(time.monotonic_ns(), None, 0, lambda: next(flags, True))
    assert adb.intents("fixture.opus") == 1 and adb.intents(mm.MAPS_PACKAGE) == 0


def test_command_failure_reads_exit_codes_timeouts_and_am_errors() -> None:
    done = lambda code, out="", err="": subprocess.CompletedProcess([], code, out, err)  # noqa: E731
    assert mm.command_failure(done(0, "Starting: Intent { act=android.intent.action.VIEW }")) is None
    assert mm.command_failure(done(0, "Warning: Activity not started, intent has been delivered "
                                      "to currently running top-most instance.")) is None
    assert mm.command_failure(done(1, "", "error: device offline")) == "exit 1: error: device offline"
    assert mm.command_failure(done(0, "Starting: Intent {}\nError: Activity not started, unable to "
                                      "resolve Intent")).startswith("Error: Activity not started")
    assert mm.command_failure(None) == "no result"
    # adb_runner turns a hung or missing adb into a failed result, never a raise.
    with tempfile.TemporaryDirectory(prefix="moving-map-adb-") as directory:
        hung = Path(directory) / "adb"
        hung.write_text("#!/bin/sh\nsleep 5\n")
        hung.chmod(0o755)
        with patch.dict(os.environ, {"PATH": f"{directory}:{os.environ['PATH']}"}):
            result = mm.adb_runner("TESTPHONE01", timeout=0.3)("shell", "true")
    assert mm.command_failure(result).startswith("exit 1"), result


def test_failed_setup_command_stops_the_setup() -> None:
    # Given each required setup command failing in turn (nonzero exit or an
    # "Error" from am start), the setup stops there and names the step.
    for needle, step, later in (("setFunctions", "usb_reset", "KEYCODE_WAKEUP"),
                                ("KEYCODE_WAKEUP", "wake", "fixture.opus"),
                                ("fixture.opus", "media", mm.MAPS_PACKAGE),
                                (mm.MAPS_PACKAGE, "maps", None)):
        adb = Adb(fail=needle, returncode=0 if needle == mm.MAPS_PACKAGE else 255,
                  stdout="Error: Activity not started" if needle == mm.MAPS_PACKAGE else "")
        work = workload(adb)
        assert work.prepare_phone(lambda seconds: True) is False, needle
        assert work.phone_failure.startswith(step + ":"), work.phone_failure
        if later is not None:
            assert adb.intents(later) == 0, (needle, adb.calls)
        # Nothing but the restore after a failure; the restore still runs.
        assert not work.start_media() and not work.start_maps()
        calls = len(adb.calls)
        work.restore_phone()
        assert adb.calls[calls:] == [("shell", "am", "force-stop", "org.example.player"),
                                     ("shell", "pidof", "org.example.player"),
                                     ("shell", "svc", "usb", "setFunctions", "mtp")]


def test_failed_relaunch_ends_the_workload() -> None:
    # Given a stage whose media relaunch fails (exit 1, e.g. the device went offline).
    adb = Adb(fail="fixture.opus")
    work = workload(adb)
    work._relaunched_ns = 0
    read_fd, write_fd = os.pipe()
    os.set_blocking(write_fd, False)
    try:
        outcome = work.step(write_fd, last_audio_ns=None, since_ns=0, stopped=lambda: False,
                            sleep=lambda seconds: True)
        # Then the step reports it, no Maps intent and no drag follow, and the
        # relaunch is not counted.
        assert outcome == "phone_failed" and work.phone_failure.startswith("media: exit 1"), \
            (outcome, work.phone_failure)
        assert adb.intents(mm.MAPS_PACKAGE) == 0 and work.drags == 0 and work.media_relaunches == 0
        # The stimulus thread ends on it without a display failure (BLOCKED, not FAIL).
        stimulus = mm.SoakStimulus(work, write_fd, "/nonexistent/receiver.log", since_ns=0)
        stimulus.start()
        stimulus.join(10)
        assert not stimulus.is_alive() and stimulus.failure is None
    finally:
        os.close(read_fd)
        os.close(write_fd)


def test_closed_control_pipe_is_a_terminal_failure() -> None:
    # Given a stimulus that has dragged, then its display's stdin goes away.
    work = workload(Adb())
    read_fd, write_fd = os.pipe()
    os.set_blocking(write_fd, False)
    stimulus = mm.SoakStimulus(work, write_fd, "/nonexistent/receiver.log",
                               since_ns=time.monotonic_ns())
    try:
        stimulus.start()
        deadline = time.monotonic() + 10
        while work.drags < 1 and time.monotonic() < deadline:
            REAL_SLEEP(0.05)
        assert work.drags >= 1
        os.close(read_fd)
        stimulus.join(10)
        # Then the stimulus ends with a named display failure (the collector's
        # terminal error), never silently.
        assert not stimulus.is_alive()
        assert stimulus.failure == "display_control_closed", stimulus.failure
    finally:
        stimulus.halt.set()
        os.close(write_fd)


def test_max_drag_gap_counts_the_window_edges() -> None:
    s = 10**9
    assert mm.max_drag_gap_s([2 * s, 3 * s, 9 * s], 0, 10 * s) == 6.0
    assert mm.max_drag_gap_s([], 0, 10 * s) == 10.0
    assert mm.max_drag_gap_s([-5 * s, 1 * s, 20 * s], 0, 10 * s) == 9.0
    for bad in (None, [2, 1], [1.0], [True], "x"):
        assert mm.max_drag_gap_s(bad, 0, 10 * s) is None, bad


DISPLAY = """#!/usr/bin/env python3
import os, sys
with open(os.environ["FAKE_DISPLAY_INPUT"], "a") as out:
    for line in sys.stdin:
        out.write(line)
        out.flush()
"""
ADB = (ROOT / "tests/perf/fixtures/wired_perf/fake_phone_adb.sh").read_text()


def run_collector(root: Path, **env_extra: str) -> dict:
    """The real collector with a fake receiver, a fake display recording its
    control input and a fake adb on PATH."""
    bin_dir = root / "bin"
    bin_dir.mkdir()
    for name, body in (("adb", ADB), ("fake-display", DISPLAY)):
        (bin_dir / name).write_text(body)
        (bin_dir / name).chmod(0o755)
    env = {"PATH": f"{bin_dir}:{os.environ['PATH']}", "FAKE_ADB_LOG": str(root / "adb.log"),
           "FAKE_DISPLAY_INPUT": str(root / "display-input.log"), **env_extra}
    args = argparse.Namespace(
        receiver=str(FAKE_RECEIVER), display=str(bin_dir / "fake-display"), no_display=False,
        port="1-2.1", duration_seconds=4, interval=1, startup_timeout_seconds=5, store=None,
        video_socket=None, input_socket=None, state_dir=str(root / "state"),
        phone_serial="TESTPHONE01", workload="moving-map",
        media_uri="file:///sdcard/Music/fixture.opus", media_type=None,
        media_component="org.example.player/.Play",
        geo_uri=None, thermal_policy="strict-all-zones")
    with patch.dict(os.environ, env), \
            patch.object(collector, "_display_connected", return_value=True), \
            patch.object(mm, "MEDIA_RELAUNCH_SILENCE_S", 1.0), \
            patch.object(collector.time, "sleep", side_effect=lambda s: REAL_SLEEP(min(s, 0.5))):
        return collector.collect(args)


def test_collector_refuses_the_stage_when_phone_setup_fails() -> None:
    # Given a phone whose Maps launch fails during the setup (the review's P1).
    with tempfile.TemporaryDirectory(prefix="soak-moving-map-") as directory:
        root = Path(directory)
        try:
            run_collector(root, FAKE_ADB_FAIL=mm.MAPS_PACKAGE)
        except collector.PhoneSetupError as error:
            reason = str(error)
        else:
            raise AssertionError("a failed Maps launch started the stage")
        adb = (root / "adb.log").read_text().splitlines()
        # Then nothing was launched (no receiver state, no display input), the
        # failure names the step, and the phone was still restored.
        assert reason.startswith("maps: exit 1: Error: Activity not started"), reason
        assert not (root / "display-input.log").exists()
        assert not (root / "state" / "receiver.log").exists()
        assert adb[-1] == "-s TESTPHONE01 shell svc usb setFunctions mtp", adb


def test_collector_records_a_failed_relaunch_and_stops_the_workload() -> None:
    # Given a stage whose second media intent (the first relaunch) fails.
    with tempfile.TemporaryDirectory(prefix="soak-moving-map-") as directory:
        root = Path(directory)
        capture = run_collector(root, FAKE_ADB_FAIL="fixture.opus:2")
        adb = (root / "adb.log").read_text().splitlines()
    # Then the capture records the failure, no intent followed the failed one
    # but the restore, and the reporter will BLOCK it (run_evidence:moving_map_phone).
    record = capture["workload"]
    assert record["phone_failure"].startswith("media: exit 1"), record
    failed = [i for i, line in enumerate(adb) if "fixture.opus" in line][1]
    assert all("am start" not in line for line in adb[failed + 1:]), adb
    assert record["media_relaunches"] == 0, record
    import aa_perf_report as reporter
    assert reporter.workload_gap(record, None) == "moving_map_phone"


def test_collector_runs_the_workload_for_the_stage() -> None:
    with tempfile.TemporaryDirectory(prefix="soak-moving-map-") as directory:
        root = Path(directory)
        capture = run_collector(root)
        adb = (root / "adb.log").read_text().splitlines()
        drags = (root / "display-input.log").read_text().splitlines()
    # Then the phone was set up and restored through adb -s, the map was dragged
    # through the display's control channel, media was kept playing (the fake
    # receiver logs no audio), and the capture is v3 with the workload record.
    assert all(line.startswith("-s TESTPHONE01 ") for line in adb), adb
    assert adb[0] == "-s TESTPHONE01 shell svc usb setFunctions mtp", adb
    # The restore: media force-stopped and verified gone, then the USB switch.
    assert adb[-3:] == ["-s TESTPHONE01 shell am force-stop org.example.player",
                        "-s TESTPHONE01 shell pidof org.example.player",
                        "-s TESTPHONE01 shell svc usb setFunctions mtp"], adb
    assert any(mm.MAPS_PACKAGE in line for line in adb), adb
    assert drags and drags[:len(mm.drag_lines(0))] == mm.drag_lines(0), drags[:10]
    record = capture["workload"]
    assert record["kind"] == "moving-map" and record["stimulus_started"] is True, record
    assert record["drags"] >= 1 and record["media_relaunches"] >= 1, record
    assert record["phone_failure"] is None and len(record["drag_t_ns"]) == record["drags"], record
    assert record["drag_t_ns"] == sorted(record["drag_t_ns"]) and record["drag_t_ns"][0] > 0
    assert capture["concurrent_builds"] == [], capture["concurrent_builds"]
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
