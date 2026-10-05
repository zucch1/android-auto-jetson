# /// script
# requires-python = ">=3.11"
# dependencies = []
# ///
# Run: python3 -B tests/target_safety/snapshot_diff_validate.py
"""Manifest validation: alternates normalization, every rejection code, first-failure order."""
from __future__ import annotations

import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from snapshot_diff_fixtures import (canonical_digest, make_entry, make_manifest,
                                    reseal, run_cases, write_manifest)

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
from target_safety.kernel import Blocked
from target_safety.snapshot_diff import load_manifest, validate_manifest


def expect_blocked(data: object, code: str, label: str) -> None:
    try:
        validate_manifest(data)
    except Blocked as error:
        assert error.reason == code, f"{label}: {error.reason} != {code}"
        return
    raise AssertionError(f"{label}: accepted, expected {code}")


def test_valid_producer_form_normalizes(tmp_path: Path) -> None:
    """Given a producer-shaped manifest, When validating, Then it is accepted and the
    returned copy normalizes task_id/start_ns/end_ns/trailer to the contract names."""
    manifest = make_manifest()
    result = validate_manifest(manifest)
    assert result["task_window_id"] == "task-1"
    assert result["started_ns"] == manifest["start_ns"]
    assert result["ended_ns"] == manifest["end_ns"]
    assert "task_id" not in result and "start_ns" not in result and "end_ns" not in result
    trailer = result["completion_trailer"]
    assert isinstance(trailer, dict) and trailer["complete"] is True
    assert "trailer" not in result
    assert manifest["task_id"] == "task-1", "input dict must not be mutated"
    print("PASS validate accepts producer form and normalizes key names")


def test_contract_form_alternates_accepted(tmp_path: Path) -> None:
    """Given a manifest using task_window_id/started_ns/ended_ns/completion_trailer,
    When validating, Then it is accepted and the trailer digest seals that body form."""
    entry = make_entry()
    body = {
        "boot_id": "boot-fixture",
        "bytes_hashed": 5,
        "capabilities": {"xattrs": "supported", "acls": "supported",
                        "inode_flags": "supported"},
        "entries": [entry],
        "entry_count": 1,
        "link_exceptions": [],
        "roots": [["u", "/tmp/opencode/protected"]],
        "schema_version": "ppic/1",
        "started_ns": 1,
        "ended_ns": 2,
        "target_identity": "jetson-fixture",
        "task_window_id": "window-9",
        "tool_digest": "b" * 64,
        "tool_version": "ppic-snapshot/1",
        "traversal_errors": [],
    }
    manifest = dict(body)
    manifest["completion_trailer"] = {"complete": True, "manifest_digest": canonical_digest(body)}
    result = validate_manifest(manifest)
    assert result["task_window_id"] == "window-9"
    assert result["completion_trailer"]["manifest_digest"] == canonical_digest(body)
    print("PASS validate accepts contract-form alternates and their digest rule")


def test_manifest_malformed_rejections(tmp_path: Path) -> None:
    """Given non-object JSON, non-list entries and wrong primitive types, When validating,
    Then each raises manifest_malformed before any other code."""
    expect_blocked([1, 2], "manifest_malformed", "top-level list")
    expect_blocked("text", "manifest_malformed", "top-level string")
    expect_blocked(make_manifest(entries="nope"), "manifest_malformed", "entries str")
    expect_blocked(make_manifest(entry_count="1"), "manifest_malformed", "entry_count str")
    expect_blocked(make_manifest(capabilities="x"), "manifest_malformed", "capabilities str")
    expect_blocked(make_manifest(completion_trailer=[]), "manifest_malformed", "trailer list")
    expect_blocked(make_manifest(entries=[5]), "manifest_malformed", "entry item int")
    expect_blocked(make_manifest(entries=[make_entry(mode="0644")]),
                   "manifest_malformed", "entry mode str")
    expect_blocked(make_manifest(entries=[make_entry(sha256=7)]),
                   "manifest_malformed", "entry sha256 int")
    bad_path = make_entry()
    bad_path["path"] = "f.txt"
    expect_blocked(make_manifest(entries=[bad_path]),
                   "manifest_malformed", "entry path str")
    print("PASS validate rejects malformed structures and primitive types")


def test_manifest_missing_field_rejections(tmp_path: Path) -> None:
    """Given a required header or entry field absent, When validating, Then each raises
    manifest_missing_field naming the absent field."""
    for field in ("schema_version", "task_id", "target_identity", "boot_id", "tool_digest",
                  "roots", "link_exceptions", "start_ns", "end_ns", "entry_count",
                  "bytes_hashed", "traversal_errors", "capabilities", "entries"):
        manifest = make_manifest()
        del manifest[field]
        expect_blocked(manifest, "manifest_missing_field", f"header {field}")
    manifest = make_manifest()
    del manifest["trailer"]
    expect_blocked(manifest, "manifest_missing_field", "header completion_trailer")
    entry = make_entry()
    del entry["sha256"]
    expect_blocked(make_manifest(entries=[entry]), "manifest_missing_field", "entry sha256")
    entry = make_entry()
    del entry["path"]
    expect_blocked(make_manifest(entries=[entry]), "manifest_missing_field", "entry path")
    print("PASS validate rejects absent required header and entry fields")


def test_manifest_schema_version_rejection(tmp_path: Path) -> None:
    """Given schema_version other than ppic/1, When validating, Then it raises
    manifest_schema_version even with an otherwise valid manifest."""
    expect_blocked(make_manifest(schema_version="ppic/2"),
                   "manifest_schema_version", "wrong schema")
    print("PASS validate rejects foreign schema_version")


def test_manifest_incomplete_rejection(tmp_path: Path) -> None:
    """Given a trailer whose complete flag is not True, When validating, Then it raises
    manifest_incomplete regardless of the digest."""
    manifest = make_manifest()
    manifest["trailer"] = {"complete": False, "manifest_digest": canonical_digest(manifest)}
    expect_blocked(manifest, "manifest_incomplete", "complete False")
    manifest = make_manifest()
    manifest["trailer"] = {"complete": 1, "manifest_digest": canonical_digest(manifest)}
    expect_blocked(manifest, "manifest_incomplete", "complete 1 is not True")
    print("PASS validate rejects incomplete completion trailers")


def test_manifest_entry_count_mismatch_rejection(tmp_path: Path) -> None:
    """Given entry_count different from len(entries), When validating, Then it raises
    manifest_entry_count_mismatch."""
    expect_blocked(make_manifest(entry_count=7), "manifest_entry_count_mismatch", "count 7")
    expect_blocked(make_manifest(entries=[make_entry("a.txt"), make_entry("b.txt")],
                                 entry_count=1),
                   "manifest_entry_count_mismatch", "count 1 of 2")
    print("PASS validate rejects entry_count mismatches")


def test_manifest_entry_order_rejection(tmp_path: Path) -> None:
    """Given entries not strictly increasing by encoded path, When validating, Then it
    raises manifest_entry_order; duplicates count as a violation."""
    expect_blocked(make_manifest(entries=[make_entry("b.txt"), make_entry("a.txt")]),
                   "manifest_entry_order", "descending")
    expect_blocked(make_manifest(entries=[make_entry("a.txt"), make_entry("a.txt")]),
                   "manifest_entry_order", "duplicate")
    expect_blocked(make_manifest(entries=[make_entry(""), make_entry("a.txt"),
                                          make_entry("Z.txt")]),
                   "manifest_entry_order", "out of order")
    print("PASS validate rejects entry order violations and duplicates")


def test_manifest_traversal_errors_rejection(tmp_path: Path) -> None:
    """Given a non-empty traversal_errors list, When validating, Then it raises
    manifest_traversal_errors even with a valid digest."""
    manifest = make_manifest(traversal_errors=[{"root": 0, "path": ["u", "x"],
                                                "reason": "unreadable", "detail": "EACCES"}])
    trailer = manifest["trailer"]
    assert isinstance(trailer, dict) and trailer["manifest_digest"] == canonical_digest(manifest)
    expect_blocked(manifest, "manifest_traversal_errors", "one error")
    print("PASS validate rejects manifests carrying traversal errors")


def test_manifest_digest_mismatch_rejection(tmp_path: Path) -> None:
    """Given a trailer digest that does not match the recomputed producer-rule digest,
    When validating, Then it raises manifest_digest_mismatch."""
    manifest = make_manifest()
    manifest["trailer"] = {"complete": True, "manifest_digest": "0" * 64}
    expect_blocked(manifest, "manifest_digest_mismatch", "zero digest")
    manifest = make_manifest()
    reseal(manifest)
    manifest["bytes_hashed"] = 999
    expect_blocked(manifest, "manifest_digest_mismatch", "body changed after seal")
    print("PASS validate rejects trailer digests that do not recompute")


def test_first_failure_wins_ordering(tmp_path: Path) -> None:
    """Given a manifest violating several checks at once, When validating, Then the
    earliest check in the documented order reports its code alone."""
    manifest = make_manifest(schema_version="ppic/2", entry_count=99,
                             traversal_errors=[{"root": 0, "path": ["u", "x"],
                                                "reason": "r", "detail": "d"}])
    manifest["trailer"] = {"complete": True, "manifest_digest": "0" * 64}
    expect_blocked(manifest, "manifest_schema_version", "schema before count/errors/digest")
    manifest = make_manifest(entry_count=99,
                             traversal_errors=[{"root": 0, "path": ["u", "x"],
                                                "reason": "r", "detail": "d"}])
    manifest["trailer"] = {"complete": True, "manifest_digest": "0" * 64}
    expect_blocked(manifest, "manifest_entry_count_mismatch", "count before errors/digest")
    manifest = make_manifest(traversal_errors=[{"root": 0, "path": ["u", "x"],
                                                "reason": "r", "detail": "d"}])
    manifest["trailer"] = {"complete": True, "manifest_digest": "0" * 64}
    expect_blocked(manifest, "manifest_traversal_errors", "errors before digest")
    print("PASS validate reports the first failing check in documented order")


def test_load_manifest_reads_and_rejects_files(tmp_path: Path) -> None:
    """Given retained manifest files, When loading, Then valid content loads and unreadable
    or unparseable content raises manifest_malformed."""
    path = write_manifest(tmp_path, make_manifest())
    result = load_manifest(path)
    assert result["entry_count"] == 1
    garbage = tmp_path / "garbage.json"
    garbage.write_text("{not json")
    try:
        load_manifest(garbage)
    except Blocked as error:
        assert error.reason == "manifest_malformed", str(error)
    else:
        raise AssertionError("garbage JSON accepted")
    try:
        load_manifest(tmp_path / "absent.json")
    except Blocked as error:
        assert error.reason == "manifest_malformed", str(error)
    else:
        raise AssertionError("absent file accepted")
    rejected = write_manifest(tmp_path, make_manifest(schema_version="ppic/2"), "bad.json")
    try:
        load_manifest(rejected)
    except Blocked as error:
        assert error.reason == "manifest_schema_version", str(error)
    else:
        raise AssertionError("invalid manifest file accepted")
    print("PASS load_manifest reads retained files and rejects unreadable content")


def main() -> int:
    return run_cases((
        test_valid_producer_form_normalizes,
        test_contract_form_alternates_accepted,
        test_manifest_malformed_rejections,
        test_manifest_missing_field_rejections,
        test_manifest_schema_version_rejection,
        test_manifest_incomplete_rejection,
        test_manifest_entry_count_mismatch_rejection,
        test_manifest_entry_order_rejection,
        test_manifest_traversal_errors_rejection,
        test_manifest_digest_mismatch_rejection,
        test_first_failure_wins_ordering,
        test_load_manifest_reads_and_rejects_files,
    ), "snapshot-diff-validate-")


if __name__ == "__main__":
    raise SystemExit(main())
