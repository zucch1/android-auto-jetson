# /// script
# requires-python = ">=3.12"
# dependencies = []
# ///
# Run: python3 -B tests/target_safety/acquire_route.py
"""Route qualification on fixtures: before/after equivalence, receipt, quiet boundary,
SHA/symlink capture, mid-window change detection, frozen-payload runnability."""
from __future__ import annotations

import hashlib
import json
import os
import subprocess
import sys
import tarfile
import tempfile
from unittest.mock import patch
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
from target_safety.acquire import (
    InputSpec,
    PackageInfo,
    TargetIdentity,
    WindowRequest,
    run_window,
)
from target_safety.acquire_driver import acquire_payload, invocation, load_budget, payload_digest
from target_safety.snapshot_models import LinkException
from target_safety.acquire_models import ProcessObservation
import target_safety.acquire as acquire_module

TARGET = Path(__file__).resolve().parents[2]


def _fixture(base: Path) -> tuple[tuple[Path, ...], tuple[LinkException, ...], tuple[InputSpec, ...]]:
    root0 = base / "proj"
    (root0 / ".local_env/aqt-venv/bin").mkdir(parents=True)
    (root0 / ".venv/bin").mkdir(parents=True)
    (root0 / "a.txt").write_bytes(b"repo file\n")
    os.symlink("/usr/bin/python3", root0 / ".local_env/aqt-venv/bin/python3")
    os.symlink("/usr/bin/python3", root0 / ".venv/bin/python3")
    root1 = base / "cg"
    root1.mkdir()
    (root1 / "cg.txt").write_bytes(b"cg\n")
    tgt = base / "tgt"
    (tgt / "usr/include").mkdir(parents=True)
    (tgt / "usr/lib").mkdir(parents=True)
    (tgt / "usr/include/h.h").write_bytes(b"hdr\n")
    (tgt / "usr/lib/libf.so.1").write_bytes(b"libbytes\n")
    os.symlink("libf.so.1", tgt / "usr/lib/libf.so")
    roots = (root0, root1)
    exceptions = (
        LinkException(root0 / ".local_env/aqt-venv/bin/python3", "/usr/bin/python3"),
        LinkException(root0 / ".venv/bin/python3", "/usr/bin/python3"),
    )
    inputs = (
        InputSpec(str(tgt / "usr/include/h.h")),
        InputSpec(str(tgt / "usr/lib/libf.so.1")),
        InputSpec(str(tgt / "usr/lib/libf.so")),
    )
    return roots, exceptions, inputs


def _pkg(_path: str) -> PackageInfo:
    return PackageInfo("libfixture", "1.0-1", "arm64", "fixture-src")


def _ident() -> TargetIdentity:
    return TargetIdentity("6.8.12", "R39.2.1", "5.1", (("libfixture", "1.0-1"),))


def test_route_equivalence_receipt_and_capture() -> None:
    base = Path(tempfile.mkdtemp(prefix="rt-happy-", dir="/tmp/opencode"))
    roots, exceptions, inputs = _fixture(base)
    scratch = base / "scratch"
    scratch.mkdir()
    request = WindowRequest(roots=roots, link_exceptions=exceptions, input_set=inputs,
                            scratch=scratch, task_window_id="win-1",
                            target_identity="jetson.local",
                            package_of=_pkg, identity=_ident)
    result = run_window(request)

    assert result.diff.verdict == "equivalent", result.diff
    receipt = json.loads(result.receipt.read_text())
    assert receipt["receipt"] == "protected-path-window/1"
    assert receipt["task_window_id"] == "win-1"
    assert receipt["verdict"] == "equivalent"

    record = json.loads(result.record_path.read_text())
    assert record["provenance"] == "observed"
    assert record["quiet_boundary"]["note"].endswith(
        "accepted limitation of endpoint-state verification.")
    assert "does not prove" in record["quiet_boundary"]["note"]

    by_path = {e["original_input_path"]: e for e in record["entries"]}
    header = next(e for e in record["entries"] if e["kind"] == "regular" and e["original_input_path"].endswith("h.h"))
    assert header["sha256"] == hashlib.sha256(b"hdr\n").hexdigest()
    assert header["size"] == len(b"hdr\n")
    link = next(e for e in record["entries"] if e["kind"] == "symlink")
    assert link["link_text"] == "libf.so.1"
    assert link["sha256"] == hashlib.sha256(b"libf.so.1").hexdigest()
    assert (result.snapshot_dir / link["copied_path"]).is_symlink()
    assert (result.snapshot_dir / header["copied_path"]).read_bytes() == b"hdr\n"
    print("PASS route zero-diff equivalence, window receipt, quiet boundary, SHA/symlink capture")


def test_route_detects_mid_window_change() -> None:
    base = Path(tempfile.mkdtemp(prefix="rt-change-", dir="/tmp/opencode"))
    roots, exceptions, inputs = _fixture(base)
    scratch = base / "scratch"
    scratch.mkdir()
    target = roots[0] / "a.txt"

    def mutating_identity() -> TargetIdentity:
        target.write_bytes(b"mutated during window\n")
        return _ident()

    request = WindowRequest(roots=roots, link_exceptions=exceptions, input_set=inputs,
                            scratch=scratch, task_window_id="win-2",
                            target_identity="jetson.local",
                            package_of=_pkg, identity=mutating_identity)
    result = run_window(request)
    assert result.diff.verdict != "equivalent", result.diff
    assert result.diff.differences, result.diff
    changed = [d for d in result.diff.differences if d.get("path") == ["u", "a.txt"]]
    assert changed, result.diff.differences
    print("PASS route detects a protected-path change inside the window (never equivalence)")


def test_frozen_payload_runs_end_to_end() -> None:
    base = Path(tempfile.mkdtemp(prefix="rt-payload-", dir="/tmp/opencode"))
    roots, exceptions, inputs = _fixture(base)
    src = base / "src"
    src.mkdir()
    (base / "payload.tar").write_bytes(acquire_payload())
    with tarfile.open(base / "payload.tar") as tar:
        tar.extractall(src)
    scratch = base / "scratch"
    scratch.mkdir()
    pkg = {"name": "libfixture", "version": "1.0-1", "architecture": "arm64", "source": "src"}
    observation = base / "inputs.json"
    observation.write_text(json.dumps({
        "schema": "aa-acquire-observation/1",
        "identity": {"kernel": "6.8.12", "l4t": "R39.2.1", "jetpack": "5.1", "dpkg_summary": []},
        "packages": {i.path: pkg for i in inputs},
        "inputs": [{"path": i.path} for i in inputs],
    }))
    argv = ["--scratch", str(scratch), "--input-set", str(observation),
            *[x for r in roots for x in ("--root", str(r))],
            "--link-exception", f"{roots[0]}/.local_env/aqt-venv/bin/python3=/usr/bin/python3",
            "--link-exception", f"{roots[0]}/.venv/bin/python3=/usr/bin/python3",
            "--task-window-id", "win-payload", "--target-identity", "jetson.local"]
    harness = (
        "import sys; sys.path.insert(0, sys.argv[1]);"
        "import target_safety.acquire as A;"
        "from target_safety.acquire_models import ProcessObservation;"
        "A.observe_processes=lambda:ProcessObservation(((\"1\",\"fixture-init\",\"/\"),),None);"
        "raise SystemExit(A.main(sys.argv[2:]))"
    )
    run = subprocess.run([sys.executable, "-c", harness, str(src), *argv],
                         capture_output=True, text=True)
    assert run.returncode == 0, run.stderr
    summary = json.loads(run.stdout)
    assert summary["status"] == "ok" and summary["equivalent"] is True
    assert summary["quiet_status"] in ("quiescent", "unproven-activity")
    record = json.loads((scratch / "acquisition.json").read_text())
    assert record["provenance"] == "fixture"
    assert record["quiet_boundary"]["note"].startswith("Process/cwd observations")
    assert (scratch / "receipt.json").exists() and (scratch / "diff.json").exists()
    assert (scratch / "snapshot").is_dir()
    print("PASS frozen payload runs the full route end-to-end (equivalence + receipt + record)")


def test_frozen_payload_entry_runnable() -> None:
    base = Path(tempfile.mkdtemp(prefix="rt-entry-", dir="/tmp/opencode"))
    src = base / "src"
    src.mkdir()
    (base / "payload.tar").write_bytes(acquire_payload())
    with tarfile.open(base / "payload.tar") as tar:
        tar.extractall(src)
    env = dict(os.environ)
    env["PYTHONPATH"] = str(src)
    run = subprocess.run([sys.executable, "-B", "-m", "target_safety.acquire", "--help"],
                         capture_output=True, text=True, env=env)
    assert run.returncode == 0, run.stderr
    assert "acquire" in run.stdout
    print("PASS frozen payload entry (python3 -m target_safety.acquire) is runnable")


def test_budget_and_payload_digest() -> None:
    budget = load_budget()
    assert budget["total_budget_seconds"] == 2400
    assert budget["retry_policy"] == "none"
    assert budget["transports"] == 1 and budget["attempts"] == 1
    components = budget["components"]
    total = sum(components.values())
    assert total == budget["total_budget_seconds"], (total, budget["total_budget_seconds"])
    assert len(budget["roots"]) == 2 and len(budget["link_exceptions"]) == 2
    first = payload_digest()
    second = payload_digest()
    assert first == second, first
    assert first["payload_bytes"] > 0
    assert "python3" in invocation()
    print("PASS finite budget (2400s, no retries) and deterministic payload digest")


def main() -> int:
    test_route_equivalence_receipt_and_capture()
    test_route_detects_mid_window_change()
    test_frozen_payload_runs_end_to_end()
    test_frozen_payload_entry_runnable()
    test_budget_and_payload_digest()
    return 0


if __name__ == "__main__":
    with patch.object(acquire_module, "observe_processes",
                      return_value=ProcessObservation((("1", "fixture-init", "/"),), None)):
        raise SystemExit(main())
