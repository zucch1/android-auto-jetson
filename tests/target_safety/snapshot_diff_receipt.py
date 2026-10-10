# SPDX-License-Identifier: GPL-3.0-or-later
# /// script
# requires-python = ">=3.11"
# dependencies = []
# ///
# Run: python3 -B tests/target_safety/snapshot_diff_receipt.py
"""Evidence receipt: file digests, verdict fields, deterministic serialization."""
from __future__ import annotations

import hashlib
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from snapshot_diff_fixtures import make_entry, make_manifest, run_cases, write_manifest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
from target_safety.kernel import Blocked
from target_safety.snapshot_diff import diff_manifests, evidence_receipt


def sha256_of(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def test_receipt_records_correct_file_digests(tmp_path: Path) -> None:
    """Given a completed equivalent diff over two retained manifests, When writing the
    evidence receipt, Then every recorded sha256 is the SHA-256 of that file's bytes and
    the verdict, window trace and timestamp are carried across."""
    before_path = write_manifest(tmp_path, make_manifest(task_id="window-7"), "before.json")
    after_path = write_manifest(tmp_path, make_manifest(task_id="window-7"), "after.json")
    artifact_path = tmp_path / "diff.json"
    result = diff_manifests(before_path, after_path, artifact_path)
    receipt_path = evidence_receipt("window-7", before_path, after_path, artifact_path,
                                    result, "trace://window-7", tmp_path / "receipt.json")
    assert receipt_path == tmp_path / "receipt.json"
    receipt = json.loads(receipt_path.read_text())
    assert receipt["receipt"] == "protected-path-window/1"
    assert receipt["task_window_id"] == "window-7"
    assert receipt["manifest_window_ids"] == {"before": "window-7", "after": "window-7"}
    assert receipt["window_trace"] == "trace://window-7"
    assert receipt["verdict"] == "equivalent" and receipt["exit_status"] == 0
    assert receipt["before_manifest"] == {"path": str(before_path),
                                          "sha256": sha256_of(before_path)}
    assert receipt["after_manifest"] == {"path": str(after_path),
                                         "sha256": sha256_of(after_path)}
    assert receipt["diff_artifact"] == {"path": str(artifact_path),
                                        "sha256": sha256_of(artifact_path)}
    recorded_ns = receipt["recorded_ns"]
    assert isinstance(recorded_ns, int) and recorded_ns > 0
    print("PASS evidence receipt records correct digests and verdict")


def test_receipt_carries_gate_failed_disposition(tmp_path: Path) -> None:
    """Given a gate_failed diff over an invalid after manifest, When writing the receipt,
    Then the recorded verdict and exit status are the gate outcome."""
    before_path = write_manifest(tmp_path, make_manifest(task_id="window-8"), "before.json")
    invalid = make_manifest(task_id="window-8", schema_version="ppic/2")
    after_path = write_manifest(tmp_path, invalid, "after.json")
    artifact_path = tmp_path / "diff.json"
    result = diff_manifests(before_path, after_path, artifact_path)
    receipt_path = evidence_receipt("window-8", before_path, after_path, artifact_path,
                                    result, "trace://window-8", tmp_path / "receipt.json")
    receipt = json.loads(receipt_path.read_text())
    assert receipt["verdict"] == "gate_failed" and receipt["exit_status"] == 2
    assert receipt["diff_artifact"]["sha256"] == sha256_of(artifact_path)
    print("PASS evidence receipt carries gate_failed dispositions")


def test_receipt_serialization_is_deterministic(tmp_path: Path) -> None:
    """Given a receipt written twice around the same diff and clock injection point,
    When serializing, Then the JSON key order is the sorted canonical order and the
    document is parse-stable across runs except for recorded_ns."""
    before_path = write_manifest(tmp_path, make_manifest(
        task_id="window-9", entries=[make_entry("a.txt")]), "before.json")
    after_path = write_manifest(tmp_path, make_manifest(
        task_id="window-9", entries=[make_entry("a.txt"), make_entry("b.txt")]),
        "after.json")
    artifact_path = tmp_path / "diff.json"
    result = diff_manifests(before_path, after_path, artifact_path)
    first = evidence_receipt("window-9", before_path, after_path, artifact_path,
                             result, "trace://window-9", tmp_path / "r1.json")
    second = evidence_receipt("window-9", before_path, after_path, artifact_path,
                              result, "trace://window-9", tmp_path / "r2.json")
    one, two = json.loads(first.read_text()), json.loads(second.read_text())
    assert list(one) == sorted(one), list(one)
    assert {k: v for k, v in one.items() if k != "recorded_ns"} == \
        {k: v for k, v in two.items() if k != "recorded_ns"}
    assert one["verdict"] == "different" and one["exit_status"] == 1
    print("PASS evidence receipt serialization is deterministic and sorted")


def test_receipt_refuses_stale_window_manifest(tmp_path: Path) -> None:
    """Given an after-manifest sealed under an EARLIER window id (the stale-reuse
    residual from review round 2), When writing the receipt for the current
    window, Then the receipt refuses with window_id_mismatch instead of
    certifying old evidence."""
    before_path = write_manifest(tmp_path, make_manifest(task_id="window-10"),
                                 "before.json")
    stale_after = write_manifest(tmp_path, make_manifest(task_id="window-9"),
                                 "after.json")
    artifact_path = tmp_path / "diff.json"
    result = diff_manifests(before_path, stale_after, artifact_path)
    try:
        evidence_receipt("window-10", before_path, stale_after, artifact_path,
                         result, "trace://window-10", tmp_path / "receipt.json")
    except Blocked as error:
        assert error.args[0] == "window_id_mismatch", error.args
        assert not (tmp_path / "receipt.json").exists()
        print("PASS evidence receipt refuses a stale-window manifest")
        return
    raise AssertionError("stale-window manifest must refuse the receipt")


def test_receipt_records_unverifiable_window_id(tmp_path: Path) -> None:
    """Given an unparseable after-manifest (already gate_failed), When writing the
    receipt, Then the receipt is retained with a null window id for that side
    rather than refusing, so failure evidence is never lost."""
    before_path = write_manifest(tmp_path, make_manifest(task_id="window-11"),
                                 "before.json")
    after_path = tmp_path / "after.json"
    after_path.write_bytes(b'{"schema_version": "ppic/1", "entries": [')  # truncated
    artifact_path = tmp_path / "diff.json"
    result = diff_manifests(before_path, after_path, artifact_path)
    assert result.verdict == "gate_failed"
    receipt_path = evidence_receipt("window-11", before_path, after_path, artifact_path,
                                    result, "trace://window-11", tmp_path / "receipt.json")
    receipt = json.loads(receipt_path.read_text())
    assert receipt["manifest_window_ids"] == {"before": "window-11", "after": None}
    assert receipt["verdict"] == "gate_failed"
    print("PASS evidence receipt records unverifiable window ids without refusing")


def main() -> int:
    return run_cases((
        test_receipt_records_correct_file_digests,
        test_receipt_carries_gate_failed_disposition,
        test_receipt_serialization_is_deterministic,
        test_receipt_refuses_stale_window_manifest,
        test_receipt_records_unverifiable_window_id,
    ), "snapshot-diff-receipt-")


if __name__ == "__main__":
    raise SystemExit(main())
