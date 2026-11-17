#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# /// script
# requires-python = ">=3.12"
# dependencies = []
# ///
# Run: python3 -B tests/perf/test_remote_stop.py
"""A stop reaches the collector on the far side of the ssh session (#137).

The fake pair: a stub ``ssh`` client that starts the "remote" command in its
own session (as sshd would: killing the client never signals it) and runs the
launcher's stop call, the real tools/perf/aa_remote_stop.sh, locally.
"""
from __future__ import annotations

import json
import os
import signal
import subprocess
import sys
import tempfile
import time
from datetime import datetime, timezone
from pathlib import Path
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools/perf"))
sys.path.insert(0, str(ROOT / "tools/wired-qa"))
import aa_drive_smoke as smoke  # noqa: E402
import aa_soak_chain as chain  # noqa: E402

LAUNCHER = ROOT / "tools/perf/aa-projection-perf.sh"
STOP = ROOT / "tools/perf/aa_remote_stop.sh"
COLLECTOR_DIR = ROOT / "tools/perf"
FAKE_RECEIVER = ROOT / "tests/perf/fixtures/wired_perf/fake_receiver.py"
PHONE = "TESTPHONE01"
A, B = "a" * 64, "b" * 64

SSH = r"""#!/usr/bin/env bash
cmd="${@: -1}"
printf '%s\n' "$cmd" >> "$SSH_LOG"
case "$cmd" in
    *"python3 -"*)
        run_file="$(sed -n "s/.*--run-file '\([^']*\)'.*/\1/p" <<<"$cmd")"
        cat >/dev/null  # the collector source
        # The remote side: its own session, as under sshd.
        setsid "$FAKE_REMOTE" "$run_file" </dev/null &
        wait "$!"
        exit $? ;;
    "bash -s -- "*) eval "$cmd"; exit $? ;;
    *sha256sum*aa-wired-receiver*) printf '%s  r\n' "$HASH_A"; exit 0 ;;
    *sha256sum*aa-dummy-display*) printf '%s  d\n' "$HASH_B"; exit 0 ;;
    *"adb devices -l"*) printf 'List of devices attached\n%s device usb:1-2.1 model:FAKE\n' "$PHONE"; exit 0 ;;
    *xrandr* | *drm*) printf 'connected\n'; exit 0 ;;
    *pactl* | *pw-cli*) printf '0\tfake\tPipeWire\n'; exit 0 ;;
esac
exit 0
"""
# The remote collector: records its run like the real one, and on TERM cleans
# up for 1 s (or hangs, FAKE_REMOTE_MODE=hung) before acknowledging.
REMOTE = r"""#!/usr/bin/env python3
import os, signal, sys, time
run_file, marks, mode = sys.argv[1], os.environ["FAKE_REMOTE_MARKS"], os.environ["FAKE_REMOTE_MODE"]
start = open(f"/proc/{os.getpid()}/stat").read().rsplit(")", 1)[1].split()[19]
try:
    os.setpgid(0, 0)  # as the real collector: lead its own group
except PermissionError:
    pass
if run_file:  # (a launcher without --run-file still starts the remote side)
    fd = os.open(run_file, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    with os.fdopen(fd, "w") as out:
        out.write(f"collector {os.getpid()} {start}\ngroup {os.getpgid(0)} {os.getsid(0)}\n"
                  "phone TESTPHONE01\nmedia_package org.example.player\n")
def mark(name):
    open(os.path.join(marks, name), "w").write(str(os.getpid()))
def on_term(*_):
    mark("cleanup_began")
    if mode == "hung":
        while True:
            time.sleep(1)
    time.sleep(1)
    mark("restored")
    if run_file:
        os.unlink(run_file)
    sys.exit(3)
signal.signal(signal.SIGTERM, on_term)
mark("started")
time.sleep(300)
"""
ADB = """#!/usr/bin/env bash
printf '%s\\n' "$*" >> "$FAKE_ADB_LOG"
exit 0
"""


def alive(pid: int) -> bool:
    try:
        state = Path(f"/proc/{pid}/stat").read_text().rsplit(")", 1)[1].split()[0]
    except (OSError, IndexError):
        return False
    return state != "Z"


def wait_for(predicate, seconds: float = 30.0) -> bool:
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        if predicate():
            return True
        time.sleep(0.05)
    return predicate()


def stubs(root: Path) -> Path:
    bin_dir = root / "bin"
    bin_dir.mkdir()
    for name, body in (("ssh", SSH), ("fake-remote", REMOTE), ("adb", ADB)):
        (bin_dir / name).write_text(body)
        (bin_dir / name).chmod(0o755)
    return bin_dir


def stop_launcher_mid_stage(mode: str, ack_seconds: str = "30") -> dict:
    with tempfile.TemporaryDirectory(prefix="remote-stop-") as directory:
        root = Path(directory)
        bin_dir = stubs(root)
        marks = root / "marks"
        marks.mkdir()
        smoke_record = root / "smoke.json"
        smoke_record.write_text(json.dumps({
            "schema": smoke.SCHEMA, "verdict": "PASS", "duration_s": 600, "video_profiles": None,
            "finished_utc": datetime.now(timezone.utc).isoformat(), "phone_serial": PHONE,
            "configuration": dict(smoke.DEFAULT_CONFIGURATION),
            "binaries": {"receiver": {"sha256": A}, "display": {"sha256": B}}}))
        env = {"PATH": f"{bin_dir}:/usr/bin:/bin", "AA_PERF_HOST": "fake.local", "AA_DISPLAY": ":0",
               "SSH_LOG": str(root / "ssh.log"), "FAKE_REMOTE": str(bin_dir / "fake-remote"),
               "FAKE_REMOTE_MARKS": str(marks), "FAKE_REMOTE_MODE": mode,
               "FAKE_ADB_LOG": str(root / "adb.log"), "HASH_A": A, "HASH_B": B, "PHONE": PHONE,
               "AA_REMOTE_STOP_ACK_S": ack_seconds}
        launcher = subprocess.Popen(
            ["bash", str(LAUNCHER), "--mode", "wired", "--duration", "1800s",
             "--out", str(root / "out.json"), "--media-uri", "file:///sdcard/Music/fixture.opus",
             "--media-component", "org.example.player/.Play", "--phone-serial", PHONE,
             "--drive-smoke", str(smoke_record)],
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, env=env,
            start_new_session=True)
        remote_pid = None
        try:
            assert wait_for(lambda: (marks / "started").exists()), "the remote collector never started"
            remote_pid = int((marks / "started").read_text())
            command = next(line for line in (root / "ssh.log").read_text().splitlines()
                           if "python3 -" in line)
            run_file = command.split("--run-file '")[1].split("'")[0] if "--run-file '" in command else None
            # When the chain stops the stage's local process group.
            started = time.monotonic()
            with patch.object(chain, "STOP_GRACE_S", 60.0):
                outcome = chain.stop_group(launcher)
            output = launcher.stdout.read()
            time.sleep(0.5)  # a remote side that survived the stop is still alive now
            return {"outcome": outcome, "rc": launcher.returncode, "output": output,
                    "elapsed": time.monotonic() - started,
                    "remote_alive": alive(remote_pid), "run_file": run_file,
                    "run_file_left": run_file is not None and Path(run_file).exists(),
                    "marks": sorted(path.name for path in marks.iterdir()),
                    "adb": (root / "adb.log").read_text().splitlines()
                    if (root / "adb.log").exists() else [],
                    "ssh": (root / "ssh.log").read_text().splitlines()}
        finally:
            if launcher.poll() is None:
                os.killpg(launcher.pid, signal.SIGKILL)
                launcher.wait()
            if remote_pid is not None and alive(remote_pid):
                os.kill(remote_pid, signal.SIGKILL)


def test_a_chain_stop_reaches_the_remote_collector_and_waits_for_its_cleanup() -> None:
    # Given a stage whose collector runs on the far side of an ssh session.
    seen = stop_launcher_mid_stage("clean")
    # Then the stop was sent over its own ssh, the remote collector ran its
    # whole cleanup (1 s) and acknowledged, before the stage group was gone.
    assert not seen["remote_alive"], ("the remote collector survived the stop", seen)
    assert seen["marks"] == ["cleanup_began", "restored", "started"], seen
    assert "remote-stop: acknowledged" in seen["output"], seen["output"]
    assert seen["run_file"] is not None and not seen["run_file_left"], seen
    assert seen["outcome"] == "terminated" and seen["rc"] == 143, seen
    assert seen["elapsed"] >= 1.0, seen
    assert any(line.startswith("bash -s -- '/tmp/aa-perf-run-") for line in seen["ssh"]), seen["ssh"]
    assert seen["adb"] == [], "the collector restored the phone itself; no fallback restore"


def test_a_hung_remote_collector_is_killed_and_the_phone_restored_anyway() -> None:
    # Given a remote collector that hangs in its cleanup (no acknowledgement).
    seen = stop_launcher_mid_stage("hung", ack_seconds="2")
    # Then the fallback KILLs it and restores the phone directly.
    assert not seen["remote_alive"], ("the remote collector survived the stop", seen)
    assert "remote-stop: forced" in seen["output"], seen["output"]
    assert seen["run_file"] is not None and not seen["run_file_left"], seen
    assert "restored" not in seen["marks"] and "cleanup_began" in seen["marks"], seen
    assert seen["adb"] == [f"-s {PHONE} shell svc usb setFunctions mtp",
                           f"-s {PHONE} shell am force-stop org.example.player"], seen["adb"]
    assert seen["outcome"] == "terminated" and seen["rc"] == 143, seen


DISPLAY = """#!/usr/bin/env python3
import sys
for line in sys.stdin:
    pass
"""
XRANDR = """#!/bin/sh
printf 'HDMI-0 connected 1920x1080+0+0\\n'
"""


def test_the_real_collector_acknowledges_a_remote_stop_after_its_cleanup() -> None:
    # Given the real collector (fake receiver, display, adb and xrandr) running a
    # moving-map stage with a run file, as the launcher starts it on the target.
    with tempfile.TemporaryDirectory(prefix="remote-stop-collector-") as directory:
        root = Path(directory)
        bin_dir = root / "bin"
        bin_dir.mkdir()
        for name, body in (("adb", ADB), ("fake-display", DISPLAY), ("xrandr", XRANDR)):
            (bin_dir / name).write_text(body)
            (bin_dir / name).chmod(0o755)
        run_file = root / "run"
        env = {**os.environ, "PATH": f"{bin_dir}:{os.environ['PATH']}",
               "FAKE_ADB_LOG": str(root / "adb.log")}
        collector = subprocess.Popen(
            [sys.executable, "-B", str(COLLECTOR_DIR / "aa_projection_collect.py"),
             "--receiver", str(FAKE_RECEIVER), "--display", str(bin_dir / "fake-display"),
             "--port", "1-2.1", "--duration-seconds", "120", "--interval", "1",
             "--state-dir", str(root / "state"), "--allow-unoptimized-build",
             "--phone-serial", PHONE, "--workload", "moving-map",
             "--media-uri", "file:///sdcard/Music/fixture.opus",
             "--media-component", "org.example.player/.Play", "--run-file", str(run_file)],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, env=env,
            start_new_session=True)
        try:
            assert wait_for(lambda: run_file.exists() and "display " in run_file.read_text(), 60), \
                collector.stderr.read() if collector.poll() is not None else "no display yet"
            record = {line.split()[0]: line.split()[1:] for line in run_file.read_text().splitlines()}
            assert record["collector"][0] == str(collector.pid) and record["phone"] == [PHONE]
            assert record["group"][0] == str(collector.pid), record  # leads its own group
            assert record["media_package"] == ["org.example.player"]
            children = [int(record[role][0]) for role in ("receiver", "display")]
            assert all(alive(pid) for pid in children)
            # When the stop runs on the "target".
            stop = subprocess.run(["bash", str(STOP), str(run_file), "30", "5"],
                                  capture_output=True, text=True, timeout=90, env=env)
            # Then the collector stopped its receiver and display, restored the
            # phone, and only then removed its run file: acknowledged.
            assert stop.stdout.strip() == "remote-stop: acknowledged", (stop.stdout, stop.stderr)
            assert collector.wait(timeout=10) == 3
            assert not run_file.exists()
            assert not [pid for pid in children if alive(pid)], children
            adb = (root / "adb.log").read_text().splitlines()
            assert adb[-2:] == [f"-s {PHONE} shell svc usb setFunctions mtp",
                                f"-s {PHONE} shell am force-stop org.example.player"], adb
        finally:
            if collector.poll() is None:
                os.killpg(collector.pid, signal.SIGKILL)
                collector.wait()


SLOW_ADB = """#!/usr/bin/env bash
# The media launch is in flight for 4 s, then lands (logged on completion,
# as the intent reaches the phone).
case "$*" in *fixture.opus*)
    if [ -n "${FAKE_ADB_SLOW:-}" ]; then echo "$$" > "$FAKE_ADB_SLOW"; sleep 4; fi ;;
esac
printf '%s\\n' "$*" >> "$FAKE_ADB_LOG"
exit 0
"""


def test_a_forced_stop_kills_an_in_flight_adb_command_before_restoring() -> None:
    # Sol's reproduction: the real collector, started as sshd would (under a
    # shell in its own session, so the collector must make its own group), is
    # frozen while its media-launch adb child is in flight.
    with tempfile.TemporaryDirectory(prefix="remote-stop-adb-") as directory:
        root = Path(directory)
        bin_dir = root / "bin"
        bin_dir.mkdir()
        for name, body in (("adb", SLOW_ADB), ("fake-display", DISPLAY), ("xrandr", XRANDR)):
            (bin_dir / name).write_text(body)
            (bin_dir / name).chmod(0o755)
        run_file, adb_marker, adb_log = root / "run", root / "adb-child", root / "adb.log"
        env = {**os.environ, "PATH": f"{bin_dir}:{os.environ['PATH']}", "FAKE_ADB_LOG": str(adb_log),
               "FAKE_ADB_SLOW": str(adb_marker)}
        collector_cmd = " ".join(f"'{part}'" for part in (
            sys.executable, "-B", str(COLLECTOR_DIR / "aa_projection_collect.py"),
            "--receiver", str(FAKE_RECEIVER), "--display", str(bin_dir / "fake-display"),
            "--port", "1-2.1", "--duration-seconds", "120", "--interval", "1",
            "--state-dir", str(root / "state"), "--allow-unoptimized-build",
            "--phone-serial", PHONE, "--workload", "moving-map",
            "--media-uri", "file:///sdcard/Music/fixture.opus",
            "--media-component", "org.example.player/.Play", "--run-file", str(run_file)))
        session = subprocess.Popen(["bash", "-c", f"{collector_cmd} >/dev/null 2>&1; exit 7"],
                                   env=env, start_new_session=True)
        collector_pid = adb_pid = None
        try:
            assert wait_for(lambda: adb_marker.exists() and adb_marker.read_text().strip(), 30)
            adb_pid = int(adb_marker.read_text())
            record = {line.split()[0]: line.split()[1:] for line in run_file.read_text().splitlines()}
            collector_pid = int(record["collector"][0])
            assert collector_pid != session.pid and record["group"][0] == str(collector_pid)
            assert record["group"][1] == str(session.pid)  # the shell's session, not its group
            os.kill(collector_pid, signal.SIGSTOP)  # the hung collector
            stop = subprocess.run(["bash", str(STOP), str(run_file), "2", "5"],
                                  capture_output=True, text=True, timeout=90, env=env)
            restored_at = len(adb_log.read_text().splitlines()) if adb_log.exists() else 0
            # Then the in-flight adb command died with the collector's group,
            # before the restore; the session's shell was not touched.
            assert stop.returncode == 0 and stop.stdout.strip() == "remote-stop: forced", stop
            assert not alive(adb_pid), "the in-flight adb command survived the forced stop"
            assert not run_file.exists()
            assert session.wait(timeout=10) == 7, "the ssh session's shell was killed"
            # And nothing reaches the phone after the restore, then or later.
            time.sleep(5)  # past the moment the adb child would have landed
            adb = adb_log.read_text().splitlines()
            assert adb[-2:] == [f"-s {PHONE} shell svc usb setFunctions mtp",
                                f"-s {PHONE} shell am force-stop org.example.player"], adb
            assert len(adb) == restored_at and not any("fixture.opus" in line for line in adb), adb
        finally:
            for pid in (collector_pid, adb_pid):
                if pid is not None and alive(pid):
                    os.kill(pid, signal.SIGKILL)
            if session.poll() is None:
                os.killpg(session.pid, signal.SIGKILL)
                session.wait()


GROUP_COLLECTOR = r"""#!/usr/bin/env python3
import os, signal, subprocess, sys, time
run_file = sys.argv[1]
os.setpgid(0, 0)
signal.signal(signal.SIGTERM, signal.SIG_IGN)  # hung: never acknowledges
child = subprocess.Popen(["sleep", "60"])  # an in-flight command in its group
start = open(f"/proc/{os.getpid()}/stat").read().rsplit(")", 1)[1].split()[19]
with open(run_file, "w") as out:
    out.write(f"collector {os.getpid()} {start}\ngroup {os.getpgid(0)} {os.getsid(0)}\n"
              f"phone TESTPHONE01\nchild {child.pid}\n")
time.sleep(60)
"""


def test_a_survivor_of_the_kill_is_reported_as_a_failure() -> None:
    # Given a hung collector group whose processes survive the KILL (modelled
    # by sending no signal), the stop must not report success.
    with tempfile.TemporaryDirectory(prefix="remote-stop-survivor-") as directory:
        root = Path(directory)
        bin_dir = root / "bin"
        bin_dir.mkdir()
        (bin_dir / "adb").write_text(ADB)
        (bin_dir / "adb").chmod(0o755)
        (root / "collector").write_text(GROUP_COLLECTOR)
        (root / "collector").chmod(0o755)
        run_file = root / "run"
        env = {**os.environ, "PATH": f"{bin_dir}:{os.environ['PATH']}",
               "FAKE_ADB_LOG": str(root / "adb.log")}
        collector = subprocess.Popen([str(root / "collector"), str(run_file)], env=env)
        try:
            assert wait_for(lambda: run_file.exists() and "child" in run_file.read_text())
            child = int(run_file.read_text().split("child ")[1].split()[0])
            # Model survival with a copy whose fallback sends no signal; the
            # production script's signal is fixed (see the test below).
            production = STOP.read_text()
            assert production.count("\nkill_signal=KILL\n") == 1
            no_signal = root / "aa_remote_stop_no_signal.sh"
            no_signal.write_text(production.replace("\nkill_signal=KILL\n", "\nkill_signal=0\n"))
            stop = subprocess.run(["bash", str(no_signal), str(run_file), "1", "1"], capture_output=True,
                                  text=True, timeout=60, env=env)
            assert stop.returncode == 1, stop
            assert stop.stdout.startswith("remote-stop: failed: survivors"), stop.stdout
            assert str(child) in stop.stdout and str(collector.pid) in stop.stdout, stop.stdout
        finally:
            try:
                os.killpg(collector.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            collector.wait()


def test_production_stop_signal_cannot_be_overridden_from_the_environment() -> None:
    # The fallback must always KILL: no inherited variable may weaken it.
    production = STOP.read_text()
    assert "\nkill_signal=KILL\n" in production
    assert "AA_REMOTE_STOP_SIGNAL" not in production
    assert "kill_signal=\"$" not in production and "kill_signal=${" not in production


def test_stop_script_never_signals_a_reused_pid_and_reports_no_run() -> None:
    with tempfile.TemporaryDirectory(prefix="remote-stop-script-") as directory:
        root = Path(directory)
        # No run file at all: nothing to stop.
        stop = subprocess.run(["bash", str(STOP), str(root / "missing"), "1", "1"],
                              capture_output=True, text=True, timeout=30)
        assert stop.stdout.strip() == "remote-stop: no-run", stop
        # A recorded PID whose start time does not match is another process now.
        bystander = subprocess.Popen(["sleep", "30"])
        try:
            (root / "run").write_text(f"collector {bystander.pid} 1\nreceiver {bystander.pid} 1\n")
            stop = subprocess.run(["bash", str(STOP), str(root / "run"), "1", "1"],
                                  capture_output=True, text=True, timeout=30)
            assert stop.stdout.strip() == "remote-stop: forced", stop
            assert bystander.poll() is None, "a reused PID was signalled"
            assert not (root / "run").exists()
        finally:
            bystander.kill()
            bystander.wait()


if __name__ == "__main__":
    tests = [value for name, value in list(globals().items()) if name.startswith("test_")]
    for test in tests:
        test()
    print(f"PASS {len(tests)} remote stop tests")
