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
from contextlib import redirect_stdout
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


def run(root: Path, *extra: str, env: dict[str, str] | None = None, builds=lambda: []) -> tuple[int, str]:
    stub = root / "stub-launcher"
    stub.write_text(STUB)
    stub.chmod(0o755)
    argv = ["--out", str(root / "chain"), "--phone-serial", "TESTPHONE01",
            "--drive-smoke", str(root / "smoke.json"), "--media-uri", "file:///sdcard/Music/tone.opus",
            "--media-component", "org.example.player/.Play", "--quiet-seconds", "0",
            "--launcher", str(stub), *extra]
    output = io.StringIO()
    with patch.dict(os.environ, {"STUB_LOG": str(root / "calls.log"), **(env or {})}), \
            patch.object(chain, "builds", builds), redirect_stdout(output):
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
         "--media-uri", "file:///sdcard/Music/tone.opus", "--launcher", str(stub), *extra],
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
