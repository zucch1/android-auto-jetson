# /// script
# requires-python = ">=3.11"
# dependencies = []
# ///
# Run: python3 -B tests/target_safety/snapshot_diff_compare.py
"""Diff verdicts: equivalence, added/removed/changed, gates, git supplement, determinism."""
from __future__ import annotations

import hashlib
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from snapshot_diff_fixtures import (FIXED_NS, canonical_digest, make_entry, make_manifest,
                                    reseal, run_cases, write_manifest)

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
from target_safety.snapshot_diff import DiffResult, diff_manifests


def run_diff(tmp_path: Path, before: object, after: object,
             name: str = "diff.json") -> tuple[DiffResult, dict]:
    before_path = write_manifest(tmp_path, before, "before.json")
    after_path = write_manifest(tmp_path, after, "after.json")
    artifact_path = tmp_path / name
    result = diff_manifests(before_path, after_path, artifact_path)
    return result, json.loads(artifact_path.read_text())


def git_record(**overrides: object) -> dict[str, object]:
    record: dict[str, object] = {
        "root": 0,
        "path": ["u", "repo"],
        "result": "ok",
        "reason": None,
        "git_kind": "directory",
        "gitdir": ["u", "/tmp/opencode/protected/repo/.git"],
        "status": [],
        "tracked": [["u", "a.txt"]],
        "untracked": [],
        "commands": [["status", 0, "e" * 64]],
    }
    record.update(overrides)
    return record


def test_valid_pair_equivalent(tmp_path: Path) -> None:
    """Given two identical manifests, When diffing, Then the verdict is equivalent with
    exit status 0 and both trailer digests recorded."""
    manifest = make_manifest()
    result, artifact = run_diff(tmp_path, manifest, make_manifest())
    digest = canonical_digest(manifest)
    assert result.verdict == "equivalent" and result.exit_status == 0, result
    assert result.before_digest == result.after_digest == digest
    assert result.entries_compared == 1 and result.fields_compared == 17
    assert result.reason_codes == () and result.differences == ()
    assert artifact["schema_version"] == "ppic-diff/1"
    assert artifact["differences"] == [] and artifact["reason_codes"] == []
    print("PASS diff of an identical pair is equivalent with exit 0")


def test_added_entry(tmp_path: Path) -> None:
    """Given an entry only in the after manifest, When diffing, Then the verdict is
    different with exit 1, the added difference item, and the entry_count evidence
    drift record."""
    before = make_manifest(entries=[make_entry("a.txt")])
    after = make_manifest(entries=[make_entry("a.txt"), make_entry("b.txt")])
    result, artifact = run_diff(tmp_path, before, after)
    assert result.verdict == "different" and result.exit_status == 1, result
    assert result.differences == ({"field": "entry_count", "kind": "changed"},
                                  {"path": ["u", "b.txt"], "kind": "added"}), result.differences
    assert result.entries_compared == 1
    assert artifact["differences"] == [{"field": "entry_count", "kind": "changed"},
                                       {"path": ["u", "b.txt"], "kind": "added"}]
    print("PASS diff reports added entries as different with exit 1")


def test_removed_entry(tmp_path: Path) -> None:
    """Given an entry only in the before manifest, When diffing, Then the verdict is
    different with the removed difference item and the entry_count evidence drift."""
    before = make_manifest(entries=[make_entry("a.txt"), make_entry("b.txt")])
    after = make_manifest(entries=[make_entry("a.txt")])
    result, _ = run_diff(tmp_path, before, after)
    assert result.verdict == "different" and result.exit_status == 1, result
    assert result.differences == ({"field": "entry_count", "kind": "changed"},
                                  {"path": ["u", "b.txt"], "kind": "removed"}), result.differences
    print("PASS diff reports removed entries as different with exit 1")


def test_changed_content_sha256(tmp_path: Path) -> None:
    """Given entries whose sha256 differs, When diffing, Then one changed item carries the
    before/after digest values only for sha256."""
    before = make_manifest()
    after = make_manifest(entries=[make_entry(sha256="f" * 64)])
    result, _ = run_diff(tmp_path, before, after)
    assert result.verdict == "different" and result.exit_status == 1, result
    assert result.differences == (
        {"path": ["u", "f.txt"], "kind": "changed",
         "fields": {"sha256": {"before": "a" * 64, "after": "f" * 64}}},), result.differences
    print("PASS diff reports content digest changes with before/after values")


def test_changed_ctime_only(tmp_path: Path) -> None:
    """Given entries differing only in ctime_ns, When diffing, Then the ctime-only change
    is detected as changed, closing the write-and-revert evasion gap."""
    before = make_manifest()
    after = make_manifest(entries=[make_entry(ctime_ns=FIXED_NS + 500)])
    result, _ = run_diff(tmp_path, before, after)
    assert result.verdict == "different" and result.exit_status == 1, result
    assert result.differences == (
        {"path": ["u", "f.txt"], "kind": "changed",
         "fields": {"ctime_ns": {"before": FIXED_NS, "after": FIXED_NS + 500}}},), result.differences
    print("PASS diff detects ctime-only changes")


def test_changed_mtime_only(tmp_path: Path) -> None:
    """Given entries differing only in mtime_ns, When diffing, Then the mtime-only change
    is detected as changed."""
    before = make_manifest()
    after = make_manifest(entries=[make_entry(mtime_ns=FIXED_NS + 700)])
    result, _ = run_diff(tmp_path, before, after)
    assert result.verdict == "different" and result.exit_status == 1, result
    assert result.differences == (
        {"path": ["u", "f.txt"], "kind": "changed",
         "fields": {"mtime_ns": {"before": FIXED_NS, "after": FIXED_NS + 700}}},), result.differences
    print("PASS diff detects mtime-only changes")


def test_changed_mode_only(tmp_path: Path) -> None:
    """Given entries differing only in mode, When diffing, Then the mode-only change is
    detected as changed."""
    before = make_manifest()
    after = make_manifest(entries=[make_entry(mode=0o100755)])
    result, _ = run_diff(tmp_path, before, after)
    assert result.verdict == "different" and result.exit_status == 1, result
    assert result.differences == (
        {"path": ["u", "f.txt"], "kind": "changed",
         "fields": {"mode": {"before": 0o100644, "after": 0o100755}}},), result.differences
    print("PASS diff detects mode-only changes")


def test_capability_mismatch_gates(tmp_path: Path) -> None:
    """Given manifests disagreeing on a capability value, When diffing, Then the verdict is
    gate_failed with exit 2 and reason code capability_mismatch."""
    before = make_manifest()
    after = make_manifest(capabilities=[{"root": 0, "xattrs": "unsupported",
                                         "acls": "unsupported", "inode_flags": "unsupported",
                                         "detail": ""}])
    result, artifact = run_diff(tmp_path, before, after)
    assert result.verdict == "gate_failed" and result.exit_status == 2, result
    assert result.reason_codes == ("capability_mismatch",), result.reason_codes
    assert result.before_digest == canonical_digest(before)
    assert result.after_digest == canonical_digest(after)
    codes = artifact["reason_codes"]
    assert [item["code"] for item in codes] == ["capability_mismatch"]
    assert isinstance(codes[0]["detail"], str) and codes[0]["detail"]
    print("PASS diff gates on capability disagreement with exit 2")


def test_capability_gated_fields_skipped_when_unsupported(tmp_path: Path) -> None:
    """Given capabilities reporting xattrs unsupported on both sides and entries with
    differing xattrs, When diffing, Then the field is not compared and the verdict is
    equivalent."""
    unsupported = [{"root": 0, "xattrs": "unsupported", "acls": "unsupported",
                    "inode_flags": "unsupported", "detail": ""}]
    before = make_manifest(capabilities=unsupported)
    after = make_manifest(capabilities=unsupported,
                          entries=[make_entry(xattrs=[["u", "user.tag"], "e" * 64])])
    result, _ = run_diff(tmp_path, before, after)
    assert result.verdict == "equivalent" and result.exit_status == 0, result
    assert result.fields_compared == 14, result.fields_compared
    print("PASS diff skips capability-conditioned fields when unsupported")


def test_capability_gated_fields_compared_when_supported(tmp_path: Path) -> None:
    """Given capabilities reporting xattrs supported and entries with differing xattrs,
    When diffing, Then the xattrs change is detected as changed."""
    before = make_manifest()
    after = make_manifest(entries=[make_entry(xattrs=[["u", "user.tag"], "e" * 64])])
    result, _ = run_diff(tmp_path, before, after)
    assert result.verdict == "different" and result.exit_status == 1, result
    assert result.differences == (
        {"path": ["u", "f.txt"], "kind": "changed",
         "fields": {"xattrs": {"before": [["u", "user.tag"], "d" * 64],
                               "after": [["u", "user.tag"], "e" * 64]}}},), result.differences
    print("PASS diff compares capability-conditioned fields when supported")


def test_absent_fields_treated_as_none(tmp_path: Path) -> None:
    """Given one entry lacking rdev and one entry with rdev None plus one side carrying an
    acls value, When diffing, Then absent means None for rdev (equivalent) and the acls
    difference is reported."""
    before_entry = make_entry()
    del before_entry["rdev"]
    before = make_manifest(entries=[before_entry])
    after_entry = make_entry()
    del after_entry["rdev"]
    after_entry["acls"] = [["u", "acl"], "e" * 64]
    after = make_manifest(entries=[after_entry])
    result, _ = run_diff(tmp_path, before, after)
    assert result.differences == (
        {"path": ["u", "f.txt"], "kind": "changed",
         "fields": {"acls": {"before": None, "after": [["u", "acl"], "e" * 64]}}},), result.differences
    plain = make_manifest(entries=[make_entry()])
    absent = make_manifest(entries=[{k: v for k, v in make_entry().items() if k != "rdev"}])
    result, _ = run_diff(tmp_path, plain, absent, "diff2.json")
    assert result.verdict == "equivalent", result
    print("PASS diff treats absent compared fields as None")


def test_timing_header_only_differences_equivalent(tmp_path: Path) -> None:
    """Given manifests differing only in timing/window-id headers, When diffing, Then the
    verdict is equivalent and the before/after values land in artifact diagnostics only."""
    before = make_manifest()
    after = make_manifest(task_id="task-2", start_ns=FIXED_NS + 5, end_ns=FIXED_NS + 6)
    result, artifact = run_diff(tmp_path, before, after)
    assert result.verdict == "equivalent" and result.exit_status == 0, result
    diagnostics = artifact["diagnostics"]
    assert diagnostics["before"]["task_window_id"] == "task-1"
    assert diagnostics["after"]["task_window_id"] == "task-2"
    assert diagnostics["before"]["started_ns"] == FIXED_NS
    assert diagnostics["after"]["started_ns"] == FIXED_NS + 5
    assert diagnostics["before"]["manifest_digest"] != diagnostics["after"]["manifest_digest"]
    assert result.differences == ()
    print("PASS diff keeps timing and window-id headers out of equivalence and into diagnostics")


def test_provenance_evidence_mismatch_changes_verdict(tmp_path: Path) -> None:
    """Given manifests differing in a cross-manifest provenance/evidence field, When
    diffing, Then the verdict is different and the mismatching field is recorded -- these
    fields are must-match evidence per the producer's equivalence(), never diagnostics."""
    overrides: tuple[tuple[str, object], ...] = (
        ("target_identity", "other-host"),
        ("boot_id", "boot-2"),
        ("tool_digest", "c" * 64),
        ("tool_version", "ppic-snapshot/2"),
        ("roots", [["u", "/tmp/opencode/elsewhere"]]),
        ("link_exceptions", [{"path": ["u", "/x"], "expected_target": ["u", "/y"]}]),
        ("bytes_hashed", 999),
    )
    for field, value in overrides:
        before = make_manifest()
        after = make_manifest(**{field: value})
        result, _ = run_diff(tmp_path, before, after, f"diff-{field}.json")
        assert result.verdict == "different" and result.exit_status == 1, (field, result)
        assert {"field": field, "kind": "changed"} in result.differences, (
            field, result.differences)
    count_before = make_manifest(entries=[make_entry("a.txt")])
    count_after = make_manifest(entries=[make_entry("a.txt"), make_entry("b.txt")])
    result, _ = run_diff(tmp_path, count_before, count_after, "diff-entry_count.json")
    assert result.verdict == "different", result
    assert {"field": "entry_count", "kind": "changed"} in result.differences, result.differences
    print("PASS diff grades every provenance/evidence field mismatch as different")


def test_git_supplement_differing_changes_verdict(tmp_path: Path) -> None:
    """Given identical inventories with differing git status/tracked/untracked content,
    When diffing, Then the git drift is evidence (matching the producer's equivalence())
    and the verdict is different, with the detail still recorded in git_supplement."""
    before = make_manifest(git=[git_record()])
    after = make_manifest(git=[git_record(status=[["u", "?? new.txt"]],
                                          untracked=[["u", "new.txt"]])])
    result, artifact = run_diff(tmp_path, before, after)
    assert result.verdict == "different" and result.exit_status == 1, result
    assert {"field": "git", "kind": "changed"} in result.differences, result.differences
    supplement = artifact["git_supplement"]
    assert supplement["verdict"] == "differing", supplement
    assert supplement["differences"] == [
        {"root": 0, "path": ["u", "repo"], "kind": "changed",
         "fields": {"status": {"before": [], "after": [["u", "?? new.txt"]]},
                    "untracked": {"before": [], "after": [["u", "new.txt"]]} }}], supplement
    print("PASS diff grades git content drift as different and records the detail")


def test_git_supplement_identical_and_absent(tmp_path: Path) -> None:
    """Given identical git supplements or a supplement missing on one side, When diffing,
    Then the supplement verdict is identical or absent respectively."""
    result, artifact = run_diff(tmp_path, make_manifest(git=[git_record()]),
                                make_manifest(git=[git_record(commands=[])]))
    assert result.verdict == "equivalent", result
    assert artifact["git_supplement"] == {"verdict": "identical", "differences": []}
    without = make_manifest()
    del without["git"]
    reseal(without)
    result, artifact = run_diff(tmp_path, make_manifest(git=[git_record()]), without,
                                "diff2.json")
    assert result.verdict == "equivalent", result
    assert artifact["git_supplement"] == {"verdict": "absent"}
    print("PASS diff reports identical and absent git supplements")


def test_validation_gate_codes(tmp_path: Path) -> None:
    """Given a manifest failing each validation check on either side, When diffing, Then
    the verdict is gate_failed with exit 2, that side's code, and a null digest for it."""
    missing = make_manifest()
    del missing["tool_digest"]
    incomplete = make_manifest()
    incomplete["trailer"] = {"complete": False, "manifest_digest": "0" * 64}
    bad_digest = make_manifest()
    bad_digest["trailer"] = {"complete": True, "manifest_digest": "0" * 64}
    variants = (
        ("manifest_malformed", [1, 2]),
        ("manifest_missing_field", missing),
        ("manifest_schema_version", make_manifest(schema_version="ppic/2")),
        ("manifest_incomplete", incomplete),
        ("manifest_entry_count_mismatch", make_manifest(entry_count=7)),
        ("manifest_entry_order", make_manifest(entries=[make_entry("b.txt"),
                                                        make_entry("a.txt")])),
        ("manifest_traversal_errors", make_manifest(
            traversal_errors=[{"root": 0, "path": ["u", "x"], "reason": "r", "detail": "d"}])),
        ("manifest_digest_mismatch", bad_digest),
    )
    for code, variant in variants:
        good = make_manifest()
        result, artifact = run_diff(tmp_path, good, variant, f"diff-{code}.json")
        assert result.verdict == "gate_failed" and result.exit_status == 2, (code, result)
        assert result.reason_codes == (code,), (code, result.reason_codes)
        assert result.before_digest == canonical_digest(good)
        assert result.after_digest is None
        assert result.entries_compared == 0 and result.differences == ()
        assert [item["code"] for item in artifact["reason_codes"]] == [code]
    result, _ = run_diff(tmp_path, make_manifest(schema_version="ppic/2"),
                         make_manifest(entry_count=7), "diff-both.json")
    assert result.reason_codes == ("manifest_schema_version",
                                   "manifest_entry_count_mismatch"), result.reason_codes
    assert result.before_digest is None and result.after_digest is None
    print("PASS diff gates with exit 2 on every validation code, before code first")


def test_two_runs_byte_identical_artifacts(tmp_path: Path) -> None:
    """Given the same manifest pair, When diffing twice, Then the retained artifacts are
    byte-identical."""
    before = make_manifest(git=[git_record()])
    after = make_manifest(entries=[make_entry("a.txt"), make_entry("b.txt")],
                          git=[git_record(untracked=[["u", "new.txt"]])])
    first = tmp_path / "first.json"
    second = tmp_path / "second.json"
    diff_manifests(write_manifest(tmp_path, before, "before.json"),
                   write_manifest(tmp_path, after, "after.json"), first)
    diff_manifests(write_manifest(tmp_path, before, "before.json"),
                   write_manifest(tmp_path, after, "after.json"), second)
    assert first.read_bytes() == second.read_bytes()
    assert first.read_bytes().endswith(b"\n")
    print("PASS diff artifacts of two identical runs are byte-identical")


def test_real_producer_twice_equivalent_and_digest_rule(tmp_path: Path) -> None:
    """Given a tiny real tree snapshotted twice by the real producer, When diffing, Then
    the verdict is equivalent and the independent recompute matches the producer trailer
    digest exactly (the consumer's digest rule is the producer's rule)."""
    from target_safety.snapshot import SnapshotRequest, snapshot
    tree = tmp_path / "tree"
    (tree / "sub").mkdir(parents=True)
    (tree / "a.txt").write_bytes(b"alpha\n")
    (tree / "sub" / "b.txt").write_bytes(b"beta\n")
    before_path = tmp_path / "before.json"
    after_path = tmp_path / "after.json"
    snapshot(SnapshotRequest(roots=(tree,), task_id="window-a", target_identity="local",
                             boot_id="boot-int"), before_path)
    snapshot(SnapshotRequest(roots=(tree,), task_id="window-b", target_identity="local",
                             boot_id="boot-int"), after_path)
    raw_before = json.loads(before_path.read_text())
    raw_after = json.loads(after_path.read_text())

    def recompute(raw: dict) -> str:
        body = {key: value for key, value in raw.items()
                if key not in ("trailer", "completion_trailer")}
        return hashlib.sha256(json.dumps(
            body, sort_keys=True, separators=(",", ":"), ensure_ascii=True,
        ).encode("utf-8")).hexdigest()

    recomputed_before, recomputed_after = recompute(raw_before), recompute(raw_after)
    for raw, recomputed in ((raw_before, recomputed_before), (raw_after, recomputed_after)):
        trailer = raw["trailer"]
        assert isinstance(trailer, dict)
        assert trailer["manifest_digest"] == recomputed, "recompute rule != producer rule"
    artifact_path = tmp_path / "diff.json"
    result = diff_manifests(before_path, after_path, artifact_path)
    assert result.verdict == "equivalent" and result.exit_status == 0, result
    assert result.entries_compared == 4
    assert result.before_digest == recomputed_before
    assert result.after_digest == recomputed_after
    artifact = json.loads(artifact_path.read_text())
    assert artifact["diagnostics"]["before"]["task_window_id"] == "window-a"
    assert artifact["diagnostics"]["after"]["task_window_id"] == "window-b"
    assert artifact["diagnostics"]["before"]["started_ns"] != \
        artifact["diagnostics"]["after"]["started_ns"]
    print("PASS real-producer manifests diff equivalent and recompute matches trailer")


def main() -> int:
    return run_cases((
        test_valid_pair_equivalent,
        test_added_entry,
        test_removed_entry,
        test_changed_content_sha256,
        test_changed_ctime_only,
        test_changed_mtime_only,
        test_changed_mode_only,
        test_capability_mismatch_gates,
        test_capability_gated_fields_skipped_when_unsupported,
        test_capability_gated_fields_compared_when_supported,
        test_absent_fields_treated_as_none,
        test_timing_header_only_differences_equivalent,
        test_provenance_evidence_mismatch_changes_verdict,
        test_git_supplement_differing_changes_verdict,
        test_git_supplement_identical_and_absent,
        test_validation_gate_codes,
        test_two_runs_byte_identical_artifacts,
        test_real_producer_twice_equivalent_and_digest_rule,
    ), "snapshot-diff-compare-")


if __name__ == "__main__":
    raise SystemExit(main())
