# /// script
# requires-python = ">=3.11"
# dependencies = []
# ///
# Run: python3 -B tests/target_safety/snapshot_gate_fixtures.py
"""Shared full-lifecycle gate harness for the snapshot e2e qualification suite.

Each gate run performs the plan-contract window: a BEFORE protected-path
snapshot, a simulated window of operations, an AFTER snapshot, then the
consumer diff + evidence receipt. The harness funnels every outcome --
equivalent, different, or gate_failed (including a producer Blocked during
the after-inventory and a missing/corrupt after-manifest) -- into one
GateOutcome so scenarios assert uniformly. Standard library only, no network.
"""
from __future__ import annotations

import hashlib
import json
import os
import sys
from collections.abc import Callable, Iterable
from dataclasses import dataclass
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
from target_safety.kernel import Blocked
from target_safety.snapshot import LinkException, encode_name, snapshot
from target_safety.snapshot_codec import decode_name
from target_safety.snapshot_diff import DiffResult, diff_manifests, evidence_receipt

sys.path.insert(0, str(Path(__file__).resolve().parent))
from snapshot_fixtures import frozen_request

FIXED_NS = 1_700_000_000_000_000_000


@dataclass(frozen=True, slots=True)
class GateOutcome:
    """One before/after gate disposition plus its retained artifacts."""

    verdict: str
    exit_status: int
    reason_codes: tuple[str, ...]
    differences: tuple[dict[str, object], ...]
    artifact: dict[str, object]
    receipt: dict[str, object] | None
    receipt_error: str | None
    producer_blocked: str | None
    before_path: Path
    after_path: Path
    artifact_path: Path
    receipt_path: Path


def sha256_of(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def changed_fields(artifact: dict[str, object], relpath: bytes) -> set[str]:
    """Changed field names recorded for one entry path in a diff artifact."""
    target = list(encode_name(relpath))
    for difference in artifact.get("differences", []):
        if difference.get("kind") == "changed" and difference.get("path") == target:
            return set(difference.get("fields", {}))
    return set()


def paths_of_kind(artifact: dict[str, object], kind: str) -> set[bytes]:
    """Decoded entry paths appearing as a given difference kind (added/removed)."""
    result: set[bytes] = set()
    for difference in artifact.get("differences", []):
        if difference.get("kind") == kind:
            result.add(decode_name(difference["path"]))
    return result


def diff_and_receipt(workdir: Path, before_path: Path, after_path: Path,
                     task_id: str, producer_blocked: str | None = None) -> GateOutcome:
    """Run the consumer diff + evidence receipt over two retained manifests.

    A missing/unreadable after-manifest still yields a gate_failed diff (the
    consumer refuses to load it); the receipt that cannot hash the absent file
    is reported as a harness-level receipt_error rather than raised.
    """
    artifact_path = workdir / "diff.json"
    result: DiffResult = diff_manifests(before_path, after_path, artifact_path)
    artifact: dict[str, object] = json.loads(artifact_path.read_text())
    receipt_path = workdir / "receipt.json"
    receipt: dict[str, object] | None = None
    receipt_error: str | None = None
    try:
        evidence_receipt(task_id, before_path, after_path, artifact_path, result,
                         f"trace://{task_id}", receipt_path)
        receipt = json.loads(receipt_path.read_text())
    except (OSError, ValueError) as error:
        receipt_error = f"{type(error).__name__}: {error}"
    return GateOutcome(
        verdict=result.verdict,
        exit_status=result.exit_status,
        reason_codes=tuple(result.reason_codes),
        differences=tuple(result.differences),
        artifact=artifact,
        receipt=receipt,
        receipt_error=receipt_error,
        producer_blocked=producer_blocked,
        before_path=before_path,
        after_path=after_path,
        artifact_path=artifact_path,
        receipt_path=receipt_path,
    )


def run_window(workdir: Path, tree: Path, *, window: Callable[[], None] | None = None,
               link_exceptions: Iterable[LinkException] = (),
               task_id: str = "window-e2e") -> GateOutcome:
    """Full window: BEFORE snapshot, window of operations, AFTER snapshot, gate.

    The AFTER snapshot may legitimately raise Blocked (a producer fail-closed
    result such as an unapproved external link or a detected race). That is a
    gate-failure path: it is captured as producer_blocked and the absent
    after-manifest is diffed, yielding gate_failed -- never equivalent.
    """
    exceptions = tuple(link_exceptions)
    before_path = workdir / "before.json"
    snapshot(frozen_request((tree,), link_exceptions=exceptions, task_id=task_id),
             before_path)
    if window is not None:
        window()
    after_path = workdir / "after.json"
    producer_blocked: str | None = None
    try:
        snapshot(frozen_request((tree,), link_exceptions=exceptions, task_id=task_id),
                 after_path)
    except Blocked as error:
        producer_blocked = error.reason
    return diff_and_receipt(workdir, before_path, after_path, task_id, producer_blocked)


def main() -> int:
    os.makedirs("/tmp/opencode", exist_ok=True)
    assert GateOutcome("equivalent", 0, (), (), {}, None, None, None,
                       Path("a"), Path("b"), Path("c"), Path("d")).verdict == "equivalent"
    assert changed_fields({"differences": [
        {"path": ["u", "f.txt"], "kind": "changed", "fields": {"sha256": {}, "ctime_ns": {}}},
    ]}, b"f.txt") == {"sha256", "ctime_ns"}
    assert paths_of_kind({"differences": [
        {"path": ["u", "x"], "kind": "added"},
        {"path": ["u", "y"], "kind": "removed"},
    ]}, "added") == {b"x"}
    print("PASS snapshot gate harness types and diff-field helpers")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
