# SPDX-License-Identifier: GPL-3.0-or-later
# /// script
# requires-python = ">=3.12"
# dependencies = []
# ///
# Run: python3 -B tests/target_safety/acquire_failures.py
"""Regressions for the pre-launch corrections: exit code, streaming bound,
quiet-boundary assessment, and after-snapshot on acquisition failure."""
from __future__ import annotations

import os
import sys
import tempfile
from unittest.mock import patch
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
import target_safety.acquire as acquire_module
from target_safety.acquire import (
    InputSpec,
    PackageInfo,
    TargetIdentity,
    WindowRequest,
    acquire,
    exit_code,
    run_window,
)
from target_safety.acquire_models import ProcessObservation, QUIET_NOTE, QuietBoundary
from target_safety.acquire_readers import assess_quiet
from target_safety.kernel import Blocked
from target_safety.snapshot_models import LinkException


def _identity() -> TargetIdentity:
    return TargetIdentity("6.8.12", "R39.2.1", "5.1", ())


def _pkg(_path: str) -> PackageInfo:
    return PackageInfo("libfixture", "1.0-1", "arm64", "src")


def _roots(base: Path) -> tuple[tuple[Path, ...], tuple[LinkException, ...]]:
    root0 = base / "proj"
    (root0 / ".venv/bin").mkdir(parents=True)
    (root0 / "a.txt").write_bytes(b"repo\n")
    os.symlink("/usr/bin/python3", root0 / ".venv/bin/python3")
    root1 = base / "cg"
    root1.mkdir()
    (root1 / "b.txt").write_bytes(b"cg\n")
    return (root0, root1), (LinkException(root0 / ".venv/bin/python3", "/usr/bin/python3"),)


def test_exit_code_mapping() -> None:
    assert exit_code("equivalent", None) == 0
    assert exit_code("different", None) == 1
    assert exit_code("equivalent", Blocked("x", "y")) == 70
    assert exit_code("gate_failed", Blocked("x", "y")) == 70
    print("PASS failed equivalence / failed acquisition exit nonzero")


def test_quiet_boundary_assesses_relevant_writers() -> None:
    clean = assess_quiet(ProcessObservation((("1", "init", "/"),), None),
                         ProcessObservation((("2", "bash", "/home"),), None),
                         (Path("/protected"),))
    assert clean.status == "quiescent" and clean.proven is True and clean.relevant_writers == ()

    busy = assess_quiet(ProcessObservation((("9", "editor", "/protected/repo"),), None),
                        ProcessObservation((), None), (Path("/protected"),))
    assert busy.status == "unproven-activity" and busy.proven is False
    assert busy.relevant_writers == ("9:editor",)
    assert busy.note is QUIET_NOTE
    print("PASS quiet boundary marks relevant-cwd writers unproven (no no-writers claim)")


def test_after_snapshot_on_acquire_failure() -> None:
    base = Path(tempfile.mkdtemp(prefix="af-after-", dir="/tmp/opencode"))
    roots, exceptions = _roots(base)
    tgt = base / "tgt"
    tgt.mkdir()
    (tgt / "f.bin").write_bytes(b"data\n")
    scratch = base / "scratch"
    scratch.mkdir()

    def failing_package_of(_path: str) -> PackageInfo:
        raise Blocked("dpkg_owner_unresolved", "simulated")

    request = WindowRequest(roots=roots, link_exceptions=exceptions,
                            input_set=(InputSpec(str(tgt / "f.bin")),), scratch=scratch,
                            task_window_id="win-fail", target_identity="jetson.local",
                            package_of=failing_package_of, identity=_identity)
    result = run_window(request)
    assert result.acquire_error is not None
    assert result.after_manifest.exists(), "after snapshot must run on acquisition failure"
    assert result.before_manifest.exists()
    assert result.diff_artifact.exists() and result.receipt.exists()
    assert exit_code(result.diff.verdict, result.acquire_error) == 70
    print("PASS after-snapshot + equivalence still run when acquisition fails")


def test_streaming_byte_bound_fails_closed() -> None:
    base = Path(tempfile.mkdtemp(prefix="af-bound-", dir="/tmp/opencode"))
    roots, exceptions = _roots(base)
    tgt = base / "tgt"
    tgt.mkdir()
    big = tgt / "big.bin"
    big.write_bytes(b"x" * 5000)
    scratch = base / "scratch"
    scratch.mkdir()
    original = acquire_module.COPY_BYTE_LIMIT
    acquire_module.COPY_BYTE_LIMIT = 1000
    try:
        request = WindowRequest(roots=roots, link_exceptions=exceptions,
                                input_set=(InputSpec(str(big)),), scratch=scratch,
                                task_window_id="win-bound", target_identity="jetson.local",
                                package_of=_pkg, identity=_identity)
        try:
            acquire(request)
        except Blocked as error:
            assert error.reason == "acquire_transfer_byte_limit", error
        else:
            raise AssertionError("streaming byte bound must fail closed mid-copy")
    finally:
        acquire_module.COPY_BYTE_LIMIT = original
    print("PASS transfer byte bound enforced while streaming (not after whole-file read)")


def main() -> int:
    test_exit_code_mapping()
    test_quiet_boundary_assesses_relevant_writers()
    test_after_snapshot_on_acquire_failure()
    test_streaming_byte_bound_fails_closed()
    return 0


if __name__ == "__main__":
    with patch.object(acquire_module, "observe_processes",
                      return_value=ProcessObservation((("1", "fixture-init", "/"),), None)):
        raise SystemExit(main())
