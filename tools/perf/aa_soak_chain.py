#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# /// script
# requires-python = ">=3.12"
# dependencies = []
# ///
"""The soak chain: the 1800 s stage, then the 7200 s stage (#137).

``--transport wired|wireless`` (default wired) is passed to every stage as the
launcher's ``--mode``; a wireless chain needs ``--phone-id``, ``--store``,
``--phone-serial`` as the phone's hardware serial (bound over the AP link by
get-serialno), the cable unplugged and a wireless drive smoke for the same
phone id (tools/perf/aa_transport.py). The stages, gates and
interference rules are the same for both transports.

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
directory).

A build seen by the watch while the stage runs disqualifies it (interference,
not a product FAIL): the chain writes ``stage-<N>s-interference.json`` with
every observation, a PASS result is moved to ``-result.disqualified.json`` so
no qualifying file remains, a would-be PASS counts as BLOCKED (3; a FAIL
stays a FAIL) and the chain stops. The stage is not cut short: the collector on the target records the
same builds itself (``run_status.concurrent_builds``) and the reporter BLOCKS
the stage with ``interference:concurrent_build``; killing it would lose that
named result and its evidence. Exit: the first non-PASS stage's launcher code
(3 for a disqualified one), else 0; 128+signal when stopped; 2 on usage errors.

A stop TERMs the stage's process group and gives the whole group
STOP_GRACE_S, even after the launcher itself exits; the launcher stops the
collector on the target over its own ssh (tools/perf/aa_remote_stop.sh).
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

from aa_projection_collect import ancestors, build_processes

STAGES = (1800, 7200)  # the 60 s stage is retired (#137)
WATCH_PERIOD_S = 5
# On a stop, the stage's process group gets TERM, then KILL after this grace,
# which the whole group gets, whether or not its leader has already exited. The
# launcher's TERM handler stops the collector on the target over its own ssh,
# bounded at aa_remote_stop.sh --worst-case 30 15 (117 s) plus its 15 s
# connect margin: 132 s. The grace covers that with margin (a test checks it).
STOP_GRACE_S = 180.0
KILL_WAIT_S = 10.0
GROUP_POLL_S = 0.1
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


def group_alive(pgid: int) -> bool:
    """Whether any process is left in the group (a zombie leader counts until reaped)."""
    try:
        os.killpg(pgid, 0)
    except ProcessLookupError:
        return False
    except PermissionError:
        return True
    return True


def _wait_group(process: subprocess.Popen, seconds: float) -> bool:
    """Reap the leader and wait until the whole group is empty; False on timeout."""
    deadline = time.monotonic() + seconds
    while True:
        process.poll()  # reap the leader as soon as it exits
        if not group_alive(process.pid):
            return True
        if time.monotonic() >= deadline:
            return False
        time.sleep(GROUP_POLL_S)


def stop_group(process: subprocess.Popen) -> str:
    """TERM the stage's whole process group; KILL only what is left after STOP_GRACE_S.

    The grace is for the group: a leader that exits at once does not cut short
    a descendant that is still cleaning up. Returns "terminated" (the group
    emptied within the grace), "killed" (KILL was needed) or "unreaped".
    """
    try:
        os.killpg(process.pid, signal.SIGTERM)
    except (ProcessLookupError, PermissionError):
        pass
    if _wait_group(process, STOP_GRACE_S):
        return "terminated"
    try:
        os.killpg(process.pid, signal.SIGKILL)
    except (ProcessLookupError, PermissionError):
        pass
    return "killed" if _wait_group(process, KILL_WAIT_S) else "unreaped"


def builds() -> list[str]:
    """Running compiler/build processes (BUILD_NAMES, exact /proc comm names),
    other than the chain and its own ancestors."""
    return build_processes(ancestors())


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
    command = [str(args.launcher), "--mode", args.transport, "--duration", f"{seconds}s",
               "--host", args.host, "--port", args.port, "--phone-serial", args.phone_serial,
               "--drive-smoke", args.drive_smoke, "--media-uri", args.media_uri,
               "--interval", str(args.interval),
               "--out", str(args.out / f"{name}-result.json"),
               "--capture-out", str(args.out / f"{name}-capture.json")]
    for flag, value in (("--media-type", args.media_type), ("--media-component", args.media_component),
                        ("--geo-uri", args.geo_uri), ("--receiver", args.receiver),
                        ("--display", args.display), ("--store", args.store),
                        ("--build-dir", args.build_dir), ("--thermal-policy", args.thermal_policy),
                        ("--provenance", args.provenance), ("--display-env", args.display_env),
                        ("--phone-id", args.phone_id)):
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
        seen: list[dict] = []
        while process.poll() is None and not stop:
            running = builds()
            watch.write(json.dumps({"time": time.time(), "builds": running}) + "\n")
            watch.flush()
            if running:
                seen.append({"time": time.time(), "builds": running})
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
    if seen:
        return _disqualify(args, name, seen, result, verdict, process.returncode)
    return process.returncode


def _disqualify(args: argparse.Namespace, name: str, seen: list[dict], result: Path,
                verdict: object, code: int) -> int:
    """A build ran during the stage: record it; the stage is never a PASS.

    A would-be PASS becomes BLOCKED (3). A FAIL stays a FAIL (1), as in the
    reporter, where interference never hides a measured budget miss; its
    interference record says the miss was measured on a contended host.
    """
    moved = None
    if verdict == "PASS":
        moved = result.with_name(f"{name}-result.disqualified.json")
        result.rename(moved)
    (args.out / f"{name}-interference.json").write_text(json.dumps({
        "reason": "build_during_stage", "verdict": "BLOCKED" if code == 0 else verdict,
        "launcher_rc": code,
        "result_verdict": verdict, "result_moved_to": moved.name if moved else None,
        "observations": seen}, indent=2) + "\n")
    print(f"{name} DISQUALIFIED: build_during_stage ({len(seen)} watch sample(s) saw a build)",
          flush=True)
    return 3 if code == 0 else code


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--out", required=True, type=Path, help="new directory for the chain's files")
    parser.add_argument("--phone-serial", required=True, help="the soak phone (same as its drive smoke)")
    parser.add_argument("--drive-smoke", required=True, help="that phone's passing drive-smoke result")
    parser.add_argument("--media-uri", required=True, help="media played for every whole stage")
    parser.add_argument("--media-type")
    parser.add_argument("--media-component", required=True,
                        help="PKG/ACTIVITY playing --media-uri; required: each stage's cleanup "
                             "stops and verifies this player before the next stage")
    parser.add_argument("--geo-uri")
    parser.add_argument("--port", default="1-2.1")
    parser.add_argument("--transport", choices=("wired", "wireless"), default="wired",
                        help="session transport of every stage (default wired)")
    parser.add_argument("--phone-id", type=int, help="wireless: the approved wireless phone id")
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
    if args.transport == "wireless" and (not args.phone_id or args.phone_id <= 0 or not args.store):
        print("usage error: --transport wireless needs --phone-id and --store", file=sys.stderr)
        return 2
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
