#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# /// script
# requires-python = ">=3.12"
# dependencies = []
# ///
# Run: python3 -B tests/perf/test_soak_chain.py
"""The in-repo soak chain: 1800 s then 7200 s, moving-map workload, same phone (#137)."""
from __future__ import annotations

import io
import json
import os
import sys
import tempfile
from contextlib import redirect_stderr, redirect_stdout
from pathlib import Path
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools/perf"))
import aa_soak_chain as chain  # noqa: E402

STUB = """#!/usr/bin/env bash
printf '%s\\n' "$*" >> "$STUB_LOG"
while [ $# -gt 0 ]; do
    case "$1" in --out) out="$2"; shift 2 ;; --duration) duration="$2"; shift 2 ;; *) shift ;; esac
done
rc_var="STUB_RC_${duration%s}"
rc="${!rc_var:-0}"
verdict=PASS; [ "$rc" = 1 ] && verdict=FAIL; [ "$rc" = 3 ] && verdict=BLOCKED
printf '{"verdict": "%s"}\\n' "$verdict" > "$out"
exit "$rc"
"""


def preflight_ok(_args, seconds, _stop) -> dict:
    return {"schema": "aa-host-preflight/1", "stage_s": seconds, "verdict": "OK", "reasons": [],
            "pipewire_err_counters": {"start": {"nodes": []}}}


def run(root: Path, *extra: str, env: dict[str, str] | None = None, builds=lambda: [],
        stub_text: str = STUB, host=preflight_ok) -> tuple[int, str]:
    stub = root / "stub-launcher"
    stub.write_text(stub_text)
    stub.chmod(0o755)
    argv = ["--out", str(root / "chain"), "--phone-serial", "TESTPHONE01",
            "--drive-smoke", str(root / "smoke.json"), "--media-uri", "file:///sdcard/Music/tone.opus",
            "--media-component", "org.example.player/.Play", "--quiet-seconds", "0",
            "--launcher", str(stub), *extra]
    output = io.StringIO()
    with patch.dict(os.environ, {"STUB_LOG": str(root / "calls.log"), **(env or {})}), \
            patch.object(chain, "builds", builds), patch.object(chain, "host_preflight", host), \
            patch.object(chain, "host_counters", lambda _args: {"nodes": []}), redirect_stdout(output):
        code = chain.main(argv)
    return code, output.getvalue()


def calls(root: Path) -> list[str]:
    path = root / "calls.log"
    return path.read_text().splitlines() if path.exists() else []


def test_both_stages_run_with_the_moving_map_workload_and_the_soak_phone() -> None:
    with tempfile.TemporaryDirectory(prefix="soak-chain-") as directory:
        root = Path(directory)
        code, output = run(root)
        assert code == 0 and "CHAIN COMPLETE" in output, output
        made = calls(root)
        assert [line.split("--duration ")[1].split()[0] for line in made] == ["1800s", "7200s"], made
        for line in made:
            for needle in ("--phone-serial TESTPHONE01", "--drive-smoke ", "--media-uri file:///sdcard",
                           "--media-component org.example.player/.Play", "--capture-out "):
                assert needle in line, (needle, line)
        assert not any("--duration 60s" in line for line in made), "the 60 s stage is retired"
        for seconds in chain.STAGES:
            for suffix in ("command.json", "console.log", "build-watch.jsonl", "result.json", "logs"):
                assert (root / "chain" / f"stage-{seconds}s-{suffix}").exists(), (seconds, suffix)
        assert chain.STAGES == (1800, 7200)


def test_a_failed_stage_stops_the_chain() -> None:
    with tempfile.TemporaryDirectory(prefix="soak-chain-") as directory:
        root = Path(directory)
        code, _ = run(root, env={"STUB_RC_1800": "1"})
        assert code == 1
        assert len(calls(root)) == 1
        marker = json.loads((root / "chain" / "stage-7200s-not-run.json").read_text())
        assert "stage-1800s" in marker["reason"], marker


def test_a_build_before_a_stage_blocks_it() -> None:
    with tempfile.TemporaryDirectory(prefix="soak-chain-") as directory:
        root = Path(directory)
        seen = iter([[], ["1234 cc1plus"]])  # quiet gate passes, then a build appears
        code, output = run(root, builds=lambda: next(seen, ["1234 cc1plus"]))
        assert code == 3 and "BLOCKED" in output, output
        assert calls(root) == [], "a stage launched while a build was running"
        assert (root / "chain" / "stage-7200s-not-run.json").exists()


BUILD_STUB = STUB.replace('printf \'{"verdict"', '''[ -n "${STUB_BUILD_FLAG:-}" ] && [ "${duration%s}" = 1800 ] && touch "$STUB_BUILD_FLAG" && sleep 1
printf \'{"verdict"''', 1)


def test_a_build_during_a_stage_disqualifies_it() -> None:
    # Given a quiet host at the gate and before the stage, and a compiler that
    # starts while the 1800 s stage runs (the review's P2), for a stage whose
    # own result would PASS and one whose result FAILs.
    for rc, verdict, code in (("0", "PASS", 3), ("1", "FAIL", 1)):
        with tempfile.TemporaryDirectory(prefix="soak-chain-") as directory:
            root = Path(directory)
            flag = root / "build-started"
            with patch.object(chain, "WATCH_PERIOD_S", 0.2):
                code_seen, output = run(root, env={"STUB_BUILD_FLAG": str(flag), "STUB_RC_1800": rc},
                                        builds=lambda: ["4242 cc1plus"] if flag.exists() else [],
                                        stub_text=BUILD_STUB)
            out = root / "chain"
            # Then the stage is never a PASS: a would-be PASS is BLOCKED (3) and
            # its result moved aside; a FAIL stays a FAIL. The evidence is
            # recorded, and the chain stops before the 7200 s stage.
            assert code_seen == code, (verdict, code_seen, output)
            assert "DISQUALIFIED: build_during_stage" in output, output
            record = json.loads((out / "stage-1800s-interference.json").read_text())
            assert record["reason"] == "build_during_stage" and record["result_verdict"] == verdict
            assert record["observations"] and record["observations"][0]["builds"] == ["4242 cc1plus"]
            assert (out / "stage-1800s-result.json").exists() is (verdict != "PASS")
            assert (out / "stage-1800s-result.disqualified.json").exists() is (verdict == "PASS")
            assert len(calls(root)) == 1 and (out / "stage-7200s-not-run.json").exists()
            watch = [json.loads(line) for line in
                     (out / "stage-1800s-build-watch.jsonl").read_text().splitlines()]
            assert any(sample["builds"] for sample in watch), watch


def test_build_scan_sees_a_real_build_process_and_skips_its_own_ancestors() -> None:
    import subprocess
    import time
    with tempfile.TemporaryDirectory(prefix="soak-chain-scan-") as directory:
        ninja = Path(directory) / "ninja"
        ninja.symlink_to("/bin/sleep")
        process = subprocess.Popen([str(ninja), "30"])
        try:
            deadline = time.monotonic() + 5
            while time.monotonic() < deadline and f"{process.pid} ninja" not in chain.builds():
                time.sleep(0.05)
            assert f"{process.pid} ninja" in chain.builds()
            # Our own ancestors (e.g. ctest running this test) never count.
            import aa_projection_collect as collect
            own = collect.ancestors()
            assert os.getpid() in own
            assert not [entry for entry in chain.builds() if int(entry.split()[0]) in own]
        finally:
            process.kill()
            process.wait()


CLEANUP_LEADER = """#!/usr/bin/env bash
# A descendant in the leader's group that needs CLEANUP_S to clean up on TERM
# (or ignores TERM entirely); the leader itself exits at once on TERM.
python3 -c '
import os, signal, sys, time
marks, cleanup = sys.argv[1], float(sys.argv[2])
def on_term(*_):
    open(os.path.join(marks, "cleanup_began"), "w").close()
    if cleanup < 0:
        return
    time.sleep(cleanup)
    open(os.path.join(marks, "restored"), "w").close()
    sys.exit(0)
signal.signal(signal.SIGTERM, on_term)
open(os.path.join(marks, "child_ready"), "w").write(str(os.getpid()))
while True:
    time.sleep(0.1)
' "$MARKS" "$CLEANUP_S" &
trap 'exit 0' TERM
while :; do sleep 0.1; done
"""


def stop_leader_with_cleaning_child(cleanup_s: float, grace_s: float) -> tuple[str, float, set, bool]:
    import subprocess
    import time
    with tempfile.TemporaryDirectory(prefix="soak-chain-grace-") as directory:
        root = Path(directory)
        (root / "leader").write_text(CLEANUP_LEADER)
        (root / "leader").chmod(0o755)
        process = subprocess.Popen([str(root / "leader")], start_new_session=True,
                                   env={**os.environ, "MARKS": str(root), "CLEANUP_S": str(cleanup_s)})
        try:
            deadline = time.monotonic() + 20
            while not (root / "child_ready").exists() and time.monotonic() < deadline:
                time.sleep(0.05)
            child = int((root / "child_ready").read_text())
            started = time.monotonic()
            with patch.object(chain, "STOP_GRACE_S", grace_s):
                outcome = chain.stop_group(process)
            elapsed = time.monotonic() - started
            marks = {path.name for path in root.iterdir()}
            return outcome, elapsed, marks, group_left(process.pid) or os.path.exists(f"/proc/{child}")
        finally:
            if group_left(process.pid):
                os.killpg(process.pid, 9)
            process.wait()


def group_left(pgid: int) -> bool:
    try:
        os.killpg(pgid, 0)
    except ProcessLookupError:
        return False
    return True


def test_the_grace_holds_for_descendants_after_the_leader_exits() -> None:
    # Given a launcher that exits at once on TERM and a child still cleaning up
    # for 1 s (the review's reproduction, which was KILLed after 0.003 s).
    outcome, elapsed, marks, left = stop_leader_with_cleaning_child(cleanup_s=1.0, grace_s=10.0)
    # Then the child finished its cleanup inside the grace; nothing was killed.
    assert "restored" in marks, (outcome, elapsed, marks)
    assert outcome == "terminated" and 1.0 <= elapsed < 10.0 and not left, (outcome, elapsed)


def test_a_descendant_that_never_finishes_is_killed_when_the_grace_expires() -> None:
    outcome, elapsed, marks, left = stop_leader_with_cleaning_child(cleanup_s=-1, grace_s=1.5)
    assert "cleanup_began" in marks and "restored" not in marks, marks
    assert outcome == "killed" and 1.5 <= elapsed < 10.0 and not left, (outcome, elapsed)


REPORTING_STUB = """#!/usr/bin/env bash
# The stage's result comes from the real reporter on a prepared capture.
printf '%s\\n' "$*" >> "$STUB_LOG"
while [ $# -gt 0 ]; do
    case "$1" in --out) out="$2"; shift 2 ;; *) shift ;; esac
done
python3 -B "$REPORTER" --live --capture "$STUB_CAPTURE" --out "$out" --now 2026-11-17T00:00:00Z
"""


def test_a_stage_whose_player_was_not_stopped_blocks_the_chain() -> None:
    # Given an 1800 s stage that measured a PASS but whose restore could not
    # verify the media player stopped (the 2026-11-17 VLC case).
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    from test_v3_av_contract import moving_map, v3
    failure = "media_stop_unverified: org.example.player after 3 attempts: still running (pid 1)"
    reporter = str(ROOT / "tools/perf/aa_perf_report.py")
    with tempfile.TemporaryDirectory(prefix="soak-chain-cleanup-") as directory:
        root = Path(directory)
        (root / "stuck.json").write_text(json.dumps(
            v3(1800, workload=moving_map(1800, cleanup_failures=[failure]))))
        code, output = run(root, env={"REPORTER": reporter, "STUB_CAPTURE": str(root / "stuck.json")},
                           stub_text=REPORTING_STUB)
        result = json.loads((root / "chain" / "stage-1800s-result.json").read_text())
        # Then the stage is BLOCKED by name, and the 7200 s stage never starts.
        assert code == 3, output
        assert result["verdict"] == "BLOCKED" and "cleanup:media_not_stopped" in result["blocked_on"]
        assert len(calls(root)) == 1, calls(root)
        marker = json.loads((root / "chain" / "stage-7200s-not-run.json").read_text())
        assert "stage-1800s did not pass (rc 3)" in marker["reason"], marker
    # The same stage with a verified restore lets the chain continue.
    with tempfile.TemporaryDirectory(prefix="soak-chain-cleanup-") as directory:
        root = Path(directory)
        (root / "clean.json").write_text(json.dumps(v3(1800, workload=moving_map(1800))))
        code, output = run(root, env={"REPORTER": reporter, "STUB_CAPTURE": str(root / "clean.json")},
                           stub_text=REPORTING_STUB)
        assert code == 0 and len(calls(root)) == 2, output


def test_the_stop_grace_covers_the_launchers_remote_stop() -> None:
    # The launcher's stop on a signal is bounded at the stop script's worst case
    # (appearance 15 s, acknowledgement 30 s) plus its ssh margin; the chain
    # must not KILL the stage before that bound can expire.
    import re
    import subprocess
    worst = int(subprocess.run(["bash", str(ROOT / "tools/perf/aa_remote_stop.sh"), "--worst-case",
                                "30", "15"], capture_output=True, text=True, check=True).stdout)
    launcher = (ROOT / "tools/perf/aa-projection-perf.sh").read_text()
    margin = int(re.search(r"^REMOTE_STOP_SSH_MARGIN_S=(\d+)$", launcher, re.M).group(1))
    kill_grace = int(re.search(r"^SSH_KILL_GRACE_S=(\d+)$", launcher, re.M).group(1))
    assert re.search(r'^REMOTE_STOP_ACK_S="\$\{AA_REMOTE_STOP_ACK_S:-30\}"$', launcher, re.M)
    # Hard bound of that ssh: TERM at worst + margin, KILL kill_grace later.
    assert chain.STOP_GRACE_S > worst + margin + kill_grace, (chain.STOP_GRACE_S, worst, margin)


def test_a_blocked_host_preflight_runs_no_stage_and_says_why() -> None:
    # Given a host whose preflight is BLOCKED (#154): the stage is not run (exit 3),
    # the record and every reason are kept, and no later stage runs either.
    reasons = ["host_audio:pipewire-pulse[200]:pw-data-loop_not_realtime (cls=TS rtprio=-)"]

    def blocked(_args, seconds, _stop) -> dict:
        return {"schema": "aa-host-preflight/1", "stage_s": seconds, "verdict": "BLOCKED", "reasons": reasons}
    with tempfile.TemporaryDirectory(prefix="soak-chain-") as directory:
        root = Path(directory)
        code, output = run(root, host=blocked)
        out = root / "chain"
        assert code == 3 and "stage-1800s BLOCKED: host preflight: host_audio:pipewire-pulse" in output, output
        assert calls(root) == [], "a stage launched on a blocked host"
        assert json.loads((out / "stage-1800s-host-preflight.json").read_text())["reasons"] == reasons
        assert "pw-data-loop_not_realtime" in json.loads((out / "stage-1800s-not-run.json").read_text())["reason"]
        assert (out / "stage-7200s-not-run.json").exists()
        assert not (out / "stage-1800s-result.json").exists() and not (out / "stage-1800s-logs").exists()


def test_the_preflight_runs_before_each_stage_and_records_the_err_counters() -> None:
    seen: list[int] = []

    def host(args, seconds, stop) -> dict:
        seen.append(seconds)
        return preflight_ok(args, seconds, stop)
    with tempfile.TemporaryDirectory(prefix="soak-chain-") as directory:
        root = Path(directory)
        code, output = run(root, host=host)
        assert code == 0 and seen == [1800, 7200], (seen, output)
        for seconds in chain.STAGES:
            record = json.loads((root / "chain" / f"stage-{seconds}s-host-preflight.json").read_text())
            # start counters from the preflight, end counters added after the stage
            assert record["verdict"] == "OK", record
            assert set(record["pipewire_err_counters"]) == {"start", "during", "end"}, record
            assert record["pipewire_err_counters"]["end"]["phase"] == "after_stage"


def test_the_second_stage_is_preflighted_on_its_own_clock() -> None:
    # A host that is fine for the 1800 s stage but not for the 7200 s one: the
    # first runs and passes, the second is BLOCKED without a launch.
    def host(args, seconds, stop) -> dict:
        record = preflight_ok(args, seconds, stop)
        if seconds == 7200:
            record.update(verdict="BLOCKED", reasons=["host_maintenance:apt-daily.timer_inside_stage_window"])
        return record
    with tempfile.TemporaryDirectory(prefix="soak-chain-") as directory:
        root = Path(directory)
        code, output = run(root, host=host)
        assert code == 3 and len(calls(root)) == 1 and "--duration 1800s" in calls(root)[0], output
        assert (root / "chain" / "stage-7200s-not-run.json").exists()
        assert (root / "chain" / "stage-1800s-result.json").exists()


def test_a_stop_during_the_maintenance_wait_starts_no_stage() -> None:
    import signal

    def host(args, seconds, stop) -> dict:
        stop.signum = signal.SIGTERM  # what the handler does
        return {"verdict": "BLOCKED", "reasons": ["host_maintenance:wait_interrupted"]}
    with tempfile.TemporaryDirectory(prefix="soak-chain-") as directory:
        root = Path(directory)
        code, _ = run(root, host=host)
        assert code == 128 + signal.SIGTERM and calls(root) == []
        assert (root / "chain" / "stage-1800s-not-run.json").exists()


def test_a_preflight_that_raises_blocks_and_still_writes_the_markers() -> None:
    def broken(_args, _seconds, _stop) -> dict:
        raise ValueError("day is out of range for month")
    with tempfile.TemporaryDirectory(prefix="soak-chain-") as directory:
        root = Path(directory)
        code, output = run(root, host=broken)
        out = root / "chain"
        assert code == 3 and calls(root) == [], output
        record = json.loads((out / "stage-1800s-host-preflight.json").read_text())
        assert record["verdict"] == "BLOCKED" and "host_preflight:error (ValueError" in record["reasons"][0]
        assert "host_preflight:error" in json.loads((out / "stage-1800s-not-run.json").read_text())["reason"]
        assert (out / "stage-7200s-not-run.json").exists()


def test_a_build_that_starts_during_the_preflight_blocks_the_launch() -> None:
    # The build check is repeated right after the (possibly long) preflight.
    state = {"preflighted": False}

    def host(args, seconds, stop) -> dict:
        state["preflighted"] = True
        return preflight_ok(args, seconds, stop)
    with tempfile.TemporaryDirectory(prefix="soak-chain-") as directory:
        root = Path(directory)
        code, output = run(root, host=host, builds=lambda: ["4242 ninja"] if state["preflighted"] else [])
        assert code == 3 and calls(root) == [], output
        assert "a build started during the host preflight" in output
        record = json.loads((root / "chain" / "stage-1800s-host-preflight.json").read_text())
        assert record["verdict"] == "BLOCKED" and record["reasons"] == ["interference:build_started_during_preflight"]
        assert (root / "chain" / "stage-1800s-not-run.json").exists()


def test_non_finite_preflight_bounds_are_usage_errors() -> None:
    for flag in ("--maintenance-wait-s", "--timer-margin-s"):
        for value in ("inf", "nan", "-1"):
            with tempfile.TemporaryDirectory(prefix="soak-chain-") as directory:
                root = Path(directory)
                try:
                    with redirect_stderr(io.StringIO()):
                        run(root, flag, value)
                except SystemExit as exit_:
                    assert exit_.code == 2, (flag, value)
                else:
                    raise AssertionError(f"{flag} {value} accepted")
                assert not (root / "chain").exists()


def test_the_preflight_uses_the_launchers_ssh_even_for_localhost() -> None:
    # An ssh config may map localhost to another User: the preflight must ask that user.
    seen = []

    def local(argv, timeout=None):
        seen.append(argv)
        return chain.aa_host_preflight.Output(0, "1001\n")
    with patch.object(chain.aa_host_preflight.ssh_runner, "__defaults__", (local,)):
        assert chain.aa_host_preflight.soak_uid(chain.host_runner("localhost")) == "1001"
    assert seen == [["ssh", *chain.aa_host_preflight.SSH_OPTS, "localhost", "id -u"]], seen


SLOW_STUB = STUB.replace('printf \'%s\\n\' "$*" >> "$STUB_LOG"\n', 'printf \'%s\\n\' "$*" >> "$STUB_LOG"\nsleep 0.6\n')


def test_err_counters_are_sampled_while_the_receiver_stream_exists() -> None:
    samples = []

    def counters(_args) -> dict:
        samples.append(1)
        return {"time": 0, "receiver_streams": [{"id": 71, "name": "aa-wired-receiver", "err": 2}],
                "sinks": [], "nodes": []}
    assert SLOW_STUB != STUB
    with tempfile.TemporaryDirectory(prefix="soak-chain-") as directory:
        root = Path(directory)
        with patch.object(chain, "COUNTER_FIRST_S", 0.05), patch.object(chain, "COUNTER_PERIOD_S", 0.1), \
                patch.object(chain, "STAGES", (1800,)):
            stub = root / "stub-launcher"
            stub.write_text(SLOW_STUB)
            stub.chmod(0o755)
            argv = ["--out", str(root / "chain"), "--phone-serial", "TESTPHONE01",
                    "--drive-smoke", str(root / "smoke.json"), "--media-uri", "file:///x.opus",
                    "--media-component", "org.example.player/.Play", "--quiet-seconds", "0",
                    "--launcher", str(stub)]
            with patch.dict(os.environ, {"STUB_LOG": str(root / "calls.log")}), \
                    patch.object(chain, "builds", lambda: []), patch.object(chain, "host_preflight", preflight_ok), \
                    patch.object(chain, "host_counters", counters), redirect_stdout(io.StringIO()):
                assert chain.main(argv) == 0
        during = json.loads((root / "chain" / "stage-1800s-host-preflight.json").read_text())[
            "pipewire_err_counters"]["during"]
        assert during["samples"] >= 2 and during["receiver_stream_seen"], during
        assert during["latest_with_receiver"]["receiver_streams"][0]["err"] == 2
        assert during["first_with_receiver"]["phase"] == "during_stage"


def test_every_chain_gets_a_new_directory() -> None:
    with tempfile.TemporaryDirectory(prefix="soak-chain-") as directory:
        root = Path(directory)
        (root / "chain").mkdir()
        code, _ = run(root)
        assert code == 2 and calls(root) == []


def test_quiet_gate_waits_for_quiet_seconds() -> None:
    with tempfile.TemporaryDirectory(prefix="soak-chain-") as directory:
        clock = iter(range(0, 1000, 5))
        seen = iter([["1 ninja"], [], [], [], []])
        with patch.object(chain, "builds", lambda: next(seen, [])):
            chain.wait_quiet(10, Path(directory) / "idle.jsonl", sleep=lambda s: None,
                             clock=lambda: next(clock))
        lines = [json.loads(line) for line in (Path(directory) / "idle.jsonl").read_text().splitlines()]
        assert lines[0]["builds"] == ["1 ninja"] and lines[0]["quiet_seconds"] == 0
        assert lines[-1]["quiet_seconds"] >= 10, lines


INTERRUPT_LAUNCHER = """#!/usr/bin/env bash
[ -n "$IGNORE_TERM" ] && trap '' TERM
sleep 300 &
printf '%s %s\\n' "$$" "$!" > "$PIDS"
wait
"""
# The real chain in a child Python with only the host-wide build scan stubbed
# (a compiler elsewhere on this machine must not decide the test).
DRIVER = """import sys
sys.path.insert(0, sys.argv[1])
import aa_soak_chain as chain
chain.builds = lambda: []
chain.host_preflight = lambda a, s, stop: {"verdict": "OK", "reasons": []}
chain.host_counters = lambda a: {}
chain.COUNTER_FIRST_S = 3600.0
chain.STOP_GRACE_S = 1.0
sys.exit(chain.main(sys.argv[2:]))
"""


def alive(pid: int) -> bool:
    try:
        state = Path(f"/proc/{pid}/stat").read_text().rsplit(")", 1)[1].split()[0]
    except (OSError, IndexError):
        return False
    return state != "Z"


def start_chain(root: Path, *extra: str, ignore_term: bool = False):
    import signal  # noqa: F401  (used by callers through the module)
    import subprocess
    stub = root / "launcher"
    stub.write_text(INTERRUPT_LAUNCHER)
    stub.chmod(0o755)
    env = {**os.environ, "PIDS": str(root / "pids")}
    if ignore_term:
        env["IGNORE_TERM"] = "1"
    return subprocess.Popen(
        [sys.executable, "-B", "-c", DRIVER, str(ROOT / "tools/perf"), "--out", str(root / "chain"),
         "--phone-serial", "TESTPHONE01", "--drive-smoke", str(root / "smoke.json"),
         "--media-uri", "file:///sdcard/Music/tone.opus",
         "--media-component", "org.example.player/.Play", "--launcher", str(stub), *extra],
        env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)


def interrupt_mid_stage(ignore_term: bool) -> None:
    import signal
    import time
    with tempfile.TemporaryDirectory(prefix="soak-chain-stop-") as directory:
        root = Path(directory)
        process = start_chain(root, "--quiet-seconds", "0", ignore_term=ignore_term)
        pids: list[int] = []
        try:
            deadline = time.monotonic() + 30
            while not (root / "pids").exists() and time.monotonic() < deadline:
                time.sleep(0.1)
            time.sleep(0.3)
            pids = [int(x) for x in (root / "pids").read_text().split()]
            assert all(alive(pid) for pid in pids), pids
            # When the chain is stopped in the middle of its first stage.
            process.send_signal(signal.SIGTERM)
            output, _ = process.communicate(timeout=60)
            deadline = time.monotonic() + 5
            while any(alive(pid) for pid in pids) and time.monotonic() < deadline:
                time.sleep(0.1)
            # Then the launcher and its child are gone and both stages are marked.
            assert process.returncode == 128 + signal.SIGTERM, output
            assert not [pid for pid in pids if alive(pid)], (pids, output)
            assert "INTERRUPTED" in output, output
            first = json.loads((root / "chain" / "stage-1800s-not-run.json").read_text())
            assert "interrupted" in first["reason"], first
            assert (root / "chain" / "stage-7200s-not-run.json").exists()
        finally:
            if process.poll() is None:
                process.kill()
            for pid in pids:
                try:
                    os.kill(pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass


def test_stopping_the_chain_stops_its_stage_tree() -> None:
    interrupt_mid_stage(ignore_term=False)


def test_a_stage_that_ignores_term_is_killed_after_the_grace() -> None:
    interrupt_mid_stage(ignore_term=True)


def test_a_stop_during_the_idle_gate_starts_no_stage() -> None:
    import signal
    import time
    with tempfile.TemporaryDirectory(prefix="soak-chain-stop-") as directory:
        root = Path(directory)
        process = start_chain(root, "--quiet-seconds", "3600")
        try:
            deadline = time.monotonic() + 30
            while not (root / "chain" / "pre-soak-idle.jsonl").exists() and time.monotonic() < deadline:
                time.sleep(0.1)
            time.sleep(0.5)
            began = time.monotonic()
            process.send_signal(signal.SIGINT)
            output, _ = process.communicate(timeout=30)
            assert time.monotonic() - began < 5, "the idle gate did not observe the stop"
            assert process.returncode == 128 + signal.SIGINT, output
            assert not (root / "pids").exists(), "a stage started after the stop"
            for seconds in chain.STAGES:
                assert (root / "chain" / f"stage-{seconds}s-not-run.json").exists()
        finally:
            if process.poll() is None:
                process.kill()


if __name__ == "__main__":
    tests = [value for name, value in list(globals().items()) if name.startswith("test_")]
    for test in tests:
        test()
    print(f"PASS {len(tests)} soak chain tests")
