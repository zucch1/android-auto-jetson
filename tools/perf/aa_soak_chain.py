#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# /// script
# requires-python = ">=3.12"
# dependencies = []
# ///
"""The wired soak chain: the 1800 s stage, then the 7200 s stage (#137).

Replaces the ad-hoc chain scripts kept beside past runs. It waits until no
compiler has run for ``--quiet-seconds`` (default 300), then runs each stage
through ``tools/perf/aa-projection-perf.sh`` with the moving-map workload, the
same-phone drive-smoke gate and the run's own provenance. A stage that does
not PASS stops the chain; later stages get a ``stage-<N>s-not-run.json``. The
60 s stage is retired (the #137 gate out): the drive smoke runs before the
chain instead.

Everything for a stage lands in ``--out``: ``stage-<N>s-command.json``,
``-console.log``, ``-build-watch.jsonl`` (5 s process watch while the stage
runs), ``-result.json``, ``-capture.json`` and ``-logs/`` (exported as
``SOAK_STATE_DIR`` for a local ssh adapter that sets the collector's state
directory). Exit: the first non-PASS
stage's launcher code, else 0; 2 on usage errors.
"""
from __future__ import annotations

import argparse
import json
import os
import signal
import subprocess
import sys
import time
from pathlib import Path

STAGES = (1800, 7200)  # the 60 s stage is retired (#137)
BUILD_NAMES = ("cmake", "ninja", "make", "c++", "g++", "gcc", "cc1plus", "ctest", "nvcc", "ld")
WATCH_PERIOD_S = 5
# On a stop, the stage's process group gets TERM, then KILL after this grace:
# long enough for the collector (TERM -> exit 3) to stop its receiver/display
# and restore the phone before anything is killed.
STOP_GRACE_S = 60.0
KILL_WAIT_S = 10.0
LAUNCHER = Path(__file__).resolve().parent / "aa-projection-perf.sh"
STOP_SIGNALS = (signal.SIGTERM, signal.SIGINT)


class StopFlag:
    """SIGTERM/SIGINT handler that only records the first signal; it never raises.

    Every wait point polls it, so a stop can neither strand the running stage nor
    interrupt the one cleanup path (the #146 pattern).
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
            time.sleep(min(0.2, remaining))
        return False


def stop_group(process: subprocess.Popen) -> str:
    """TERM the stage's whole process group, KILL it after STOP_GRACE_S; reap."""
    for sig, wait in ((signal.SIGTERM, STOP_GRACE_S), (signal.SIGKILL, KILL_WAIT_S)):
        try:
            os.killpg(process.pid, sig)
        except (ProcessLookupError, PermissionError):
            pass
        try:
            process.wait(timeout=wait)
        except subprocess.TimeoutExpired:
            continue
        try:  # the leader exited: take whatever it left in its group too
            os.killpg(process.pid, signal.SIGKILL)
        except (ProcessLookupError, PermissionError):
            pass
        return "terminated" if sig == signal.SIGTERM else "killed"
    return "unreaped"


def builds() -> list[str]:
    """Running compiler/build processes (exact /proc comm names)."""
    found = []
    for path in Path("/proc").glob("[0-9]*/comm"):
        try:
            name = path.read_text().strip()
        except OSError:
            continue
        if name in BUILD_NAMES:
            found.append(f"{path.parent.name} {name}")
    return found


def wait_quiet(seconds: float, log: Path, sleep=time.sleep, clock=time.monotonic) -> bool:
    """Wait until no build process has been seen for ``seconds`` (logged).

    ``sleep`` returns False when the chain is stopped; then this returns False.
    """
    quiet_since = None
    with log.open("w") as out:
        while True:
            running = builds()
            now = clock()
            quiet_since = None if running else (now if quiet_since is None else quiet_since)
            quiet = 0.0 if quiet_since is None else now - quiet_since
            out.write(json.dumps({"time": time.time(), "builds": running, "quiet_seconds": quiet}) + "\n")
            out.flush()
            if quiet >= seconds:
                return True
            if sleep(min(10.0, max(1.0, seconds - quiet))) is False:
                return False


def stage_command(args: argparse.Namespace, seconds: int) -> list[str]:
    name = f"stage-{seconds}s"
    command = [str(args.launcher), "--mode", "wired", "--duration", f"{seconds}s",
               "--host", args.host, "--port", args.port, "--phone-serial", args.phone_serial,
               "--drive-smoke", args.drive_smoke, "--media-uri", args.media_uri,
               "--interval", str(args.interval),
               "--out", str(args.out / f"{name}-result.json"),
               "--capture-out", str(args.out / f"{name}-capture.json")]
    for flag, value in (("--media-type", args.media_type), ("--media-component", args.media_component),
                        ("--geo-uri", args.geo_uri), ("--receiver", args.receiver),
                        ("--display", args.display), ("--store", args.store),
                        ("--build-dir", args.build_dir), ("--thermal-policy", args.thermal_policy),
                        ("--provenance", args.provenance), ("--display-env", args.display_env)):
        if value:
            command += [flag, str(value)]
    return command


def run_stage(args: argparse.Namespace, seconds: int, stop: StopFlag,
              children: list[subprocess.Popen]) -> int:
    name = f"stage-{seconds}s"
    command = stage_command(args, seconds)
    (args.out / f"{name}-command.json").write_text(json.dumps(command, indent=2) + "\n")
    state = args.out / f"{name}-logs"
    state.mkdir(mode=0o700)
    env = {**os.environ, "SOAK_STATE_DIR": str(state)}
    print(f"{name} START {time.ctime()}", flush=True)
    with (args.out / f"{name}-console.log").open("w") as console, \
            (args.out / f"{name}-build-watch.jsonl").open("w") as watch:
        # Its own process group (the launcher and everything under it), registered
        # at once: the handler never raises, so nothing can come between.
        process = subprocess.Popen(command, stdout=console, stderr=subprocess.STDOUT, env=env,
                                   start_new_session=True)
        children.append(process)
        while process.poll() is None and not stop:
            watch.write(json.dumps({"time": time.time(), "builds": builds()}) + "\n")
            watch.flush()
            deadline = time.monotonic() + WATCH_PERIOD_S
            while process.poll() is None and not stop and time.monotonic() < deadline:
                try:
                    process.wait(timeout=0.2)
                except subprocess.TimeoutExpired:
                    pass
        if stop:
            outcome = stop_group(process)
            print(f"{name} INTERRUPTED by {signal.Signals(stop.signum).name}: stage {outcome}",
                  flush=True)
            return 128 + stop.signum
    result = args.out / f"{name}-result.json"
    verdict = json.loads(result.read_text()).get("verdict") if result.exists() else None
    print(f"{name} END rc={process.returncode} verdict={verdict}", flush=True)
    return process.returncode


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--out", required=True, type=Path, help="new directory for the chain's files")
    parser.add_argument("--phone-serial", required=True, help="the soak phone (same as its drive smoke)")
    parser.add_argument("--drive-smoke", required=True, help="that phone's passing drive-smoke result")
    parser.add_argument("--media-uri", required=True, help="media played for every whole stage")
    parser.add_argument("--media-type")
    parser.add_argument("--media-component")
    parser.add_argument("--geo-uri")
    parser.add_argument("--port", default="1-2.1")
    parser.add_argument("--host", default="localhost")
    parser.add_argument("--receiver")
    parser.add_argument("--display")
    parser.add_argument("--store")
    parser.add_argument("--build-dir", default=os.environ.get("AA_BUILD_DIR"))
    parser.add_argument("--thermal-policy")
    parser.add_argument("--provenance")
    parser.add_argument("--display-env")
    parser.add_argument("--interval", type=int, default=5)
    parser.add_argument("--quiet-seconds", type=float, default=300)
    parser.add_argument("--launcher", type=Path, default=LAUNCHER, help=argparse.SUPPRESS)
    args = parser.parse_args(argv)
    try:
        args.out.mkdir(mode=0o700, parents=True, exist_ok=False)
    except FileExistsError:
        print(f"usage error: {args.out} exists; every chain gets a new directory", file=sys.stderr)
        return 2
    stop = StopFlag()
    previous = {sig: signal.signal(sig, stop) for sig in STOP_SIGNALS}
    children: list[subprocess.Popen] = []
    try:
        return _chain(args, stop, children)
    finally:
        # The single cleanup: a stage still running (an exception, or a stop
        # between checks) has its whole group stopped before the chain exits.
        for process in children:
            if process.poll() is None:
                stop_group(process)
        for sig, handler in previous.items():
            signal.signal(sig, handler)


def _not_run(args: argparse.Namespace, stages: tuple[int, ...], reason: str) -> None:
    for skipped in stages:
        (args.out / f"stage-{skipped}s-not-run.json").write_text(
            json.dumps({"reason": reason}, indent=2) + "\n")


def _chain(args: argparse.Namespace, stop: StopFlag, children: list[subprocess.Popen]) -> int:
    if not wait_quiet(args.quiet_seconds, args.out / "pre-soak-idle.jsonl", sleep=stop.sleep) or stop:
        _not_run(args, STAGES, "chain interrupted before the first stage")
        return 128 + (stop.signum or signal.SIGTERM)
    print("PRE-SOAK IDLE PASS", flush=True)
    for index, seconds in enumerate(STAGES):
        if stop:
            code = 128 + stop.signum
        elif builds():
            code = 3
            print(f"stage-{seconds}s BLOCKED: a build started before the stage", flush=True)
        else:
            code = run_stage(args, seconds, stop, children)
        if code != 0:
            interrupted = code > 128 and stop
            if interrupted:
                _not_run(args, (seconds,), f"stage-{seconds}s interrupted "
                         f"({signal.Signals(stop.signum).name}); its stage was stopped")
            _not_run(args, STAGES[index + 1:], f"stage-{seconds}s did not pass (rc {code})")
            return code
    print("CHAIN COMPLETE", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
