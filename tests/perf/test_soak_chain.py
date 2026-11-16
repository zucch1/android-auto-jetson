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


if __name__ == "__main__":
    tests = [value for name, value in list(globals().items()) if name.startswith("test_")]
    for test in tests:
        test()
    print(f"PASS {len(tests)} soak chain tests")
