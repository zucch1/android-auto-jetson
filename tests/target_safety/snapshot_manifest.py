# /// script
# requires-python = ">=3.11"
# dependencies = []
# ///
# Run: python3 -B tests/target_safety/snapshot_manifest.py
"""Manifest envelope: schema, digest, trailer, retention, determinism of two runs."""
from __future__ import annotations

import hashlib
import json
import os
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from snapshot_fixtures import (FIXED_NS, build_rich_tree, entry_map,
                               frozen_request, run_cases)

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
from target_safety.kernel import Blocked
from target_safety.snapshot import SCHEMA_VERSION, snapshot

sys.path.insert(0, str(Path(__file__).resolve().parent))
import target_safety.snapshot as snapshot_module


def _producer_digest() -> str:
    """Recompute the documented tool digest over every producer module source."""
    hasher = hashlib.sha256()
    producer_dir = Path(snapshot_module.__file__).resolve().parent
    for path in sorted(producer_dir.glob("snapshot*.py")):
        hasher.update(path.name.encode())
        hasher.update(b"\0")
        hasher.update(path.read_bytes())
        hasher.update(b"\0")
    return hasher.hexdigest()


def test_manifest_envelope_fields(tmp_path: Path) -> None:
    """Given a frozen request, When snapshotting, Then every contract envelope field is
    present: schema, ids, boot id, tool digest, roots, counts, timestamps, errors, trailer."""
    tree = build_rich_tree(tmp_path)
    manifest = snapshot(frozen_request((tree,), task_id="window-7",
                                       target_identity="jetson-1"),
                        tmp_path / "manifest.json")
    data = json.loads((tmp_path / "manifest.json").read_text())
    assert data["schema_version"] == SCHEMA_VERSION == "ppic/1"
    assert data["task_id"] == "window-7" and data["target_identity"] == "jetson-1"
    assert data["boot_id"] == "boot-fixture"
    assert data["tool_digest"] == _producer_digest()
    assert data["roots"] == [["u", str(tree)]]
    assert data["entry_count"] == len(manifest.entries)
    assert data["bytes_hashed"] == manifest.bytes_hashed
    assert data["start_ns"] == FIXED_NS and data["end_ns"] == FIXED_NS
    assert data["traversal_errors"] == []
    assert data["trailer"] == {"complete": True,
                               "manifest_digest": manifest.manifest_digest}
    print("PASS snapshot manifest envelope records every contract field")


def test_manifest_digest_covers_canonical_body(tmp_path: Path) -> None:
    """Given a written manifest, When recomputing the digest, Then the trailer digest is
    SHA-256 over the canonical serialization of everything except the trailer."""
    tree = tmp_path / "tree"
    tree.mkdir()
    (tree / "f.txt").write_bytes(b"data\n")
    manifest = snapshot(frozen_request((tree,)), tmp_path / "manifest.json")
    data = json.loads((tmp_path / "manifest.json").read_text())
    trailer = data.pop("trailer")
    assert trailer["complete"] is True
    canonical = json.dumps(data, sort_keys=True, separators=(",", ":"),
                           ensure_ascii=True).encode()
    assert trailer["manifest_digest"] == hashlib.sha256(canonical).hexdigest()
    assert trailer["manifest_digest"] == manifest.manifest_digest
    print("PASS snapshot manifest digest covers the canonical body exactly")


def test_manifest_written_to_caller_path_with_retention(tmp_path: Path) -> None:
    """Given a caller-specified local path outside the roots, When snapshotting, Then the
    complete manifest is retained there, not only an aggregate hash."""
    tree = tmp_path / "tree"
    tree.mkdir()
    (tree / "f.txt").write_bytes(b"data\n")
    out = tmp_path / "evidence" / "manifest.json"
    out.parent.mkdir()
    snapshot(frozen_request((tree,)), out)
    data = json.loads(out.read_text())
    assert len(data["entries"]) == 2
    assert {tuple(entry["path"]) for entry in data["entries"]} == {("u", ""), ("u", "f.txt")}
    assert all("sha256" in entry for entry in data["entries"])
    print("PASS snapshot retains the complete manifest at the caller-specified path")


def test_evidence_path_inside_roots_blocks(tmp_path: Path) -> None:
    """Given a manifest path inside the protected root, When snapshotting, Then the run
    raises Blocked('evidence_inside_roots') so evidence never lands in scope."""
    tree = tmp_path / "tree"
    tree.mkdir()
    (tree / "f.txt").write_bytes(b"data\n")
    try:
        snapshot(frozen_request((tree,)), tree / "manifest.json")
    except Blocked as error:
        assert error.reason == "evidence_inside_roots", str(error)
        print(f"PASS snapshot evidence path inside roots blocks: {error}")
        return
    raise AssertionError("in-root evidence path accepted")


def test_two_runs_byte_identical_manifests(tmp_path: Path) -> None:
    """Given a frozen fixture including non-UTF8 names, When snapshotting twice with the
    same injected clock and identity, Then the retained manifests are byte-identical."""
    tree = build_rich_tree(tmp_path)
    fd = os.open(os.path.join(os.fsencode(str(tree)), b"bad-\xff"), os.O_CREAT | os.O_WRONLY, 0o644)
    os.write(fd, b"x")
    os.close(fd)
    (tree / "void").mkdir()
    request = frozen_request((tree,), task_id="window-9", target_identity="jetson-2")
    first = tmp_path / "first.json"
    second = tmp_path / "second.json"
    snapshot(request, first)
    snapshot(request, second)
    assert first.read_bytes() == second.read_bytes()
    assert first.read_bytes().endswith(b"\n")
    print("PASS snapshot two runs over a frozen fixture are byte-identical")


def test_real_clock_runs_share_identical_evidence_body(tmp_path: Path) -> None:
    """Given two runs with the real clock, When comparing manifests, Then only the timing
    headers differ and the evidence body serialization is identical."""
    tree = tmp_path / "tree"
    tree.mkdir()
    (tree / "f.txt").write_bytes(b"data\n")
    import target_safety.snapshot as module
    first = snapshot(module.SnapshotRequest(roots=(tree,), task_id="a",
                                            target_identity="t", boot_id="b"),
                     tmp_path / "first.json")
    second = snapshot(module.SnapshotRequest(roots=(tree,), task_id="a",
                                             target_identity="t", boot_id="b"),
                      tmp_path / "second.json")
    for manifest in (first, second):
        body = manifest.body_jsonable()
        body["start_ns"] = body["end_ns"] = 0
        assert body == first.body_jsonable() | {"start_ns": 0, "end_ns": 0}
    assert first.entries == second.entries
    assert first.manifest_digest != second.manifest_digest
    print("PASS snapshot real-clock runs differ only in timing headers")


def main() -> int:
    return run_cases((
        test_manifest_envelope_fields,
        test_manifest_digest_covers_canonical_body,
        test_manifest_written_to_caller_path_with_retention,
        test_evidence_path_inside_roots_blocks,
        test_two_runs_byte_identical_manifests,
        test_real_clock_runs_share_identical_evidence_body,
    ), "snapshot-manifest-")


if __name__ == "__main__":
    raise SystemExit(main())
