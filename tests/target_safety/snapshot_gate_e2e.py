# /// script
# requires-python = ">=3.11"
# dependencies = []
# ///
# Run: python3 -B tests/target_safety/snapshot_gate_e2e.py
"""End-to-end fixture qualification of the assembled protected-path snapshot-diff gate.

Full-lifecycle scenarios per the plan contract v1 window: a task acting on the
target takes a BEFORE inventory, performs its window of operations, takes an
AFTER inventory, and diffs them through the consumer. Equivalence is the only
pass; ANY difference, or any missing/incomplete/invalid evidence, must yield
different(1) or gate_failed(2) -- never equivalent. Each scenario builds a real
fixture tree under /tmp/opencode, drives snapshot() -> diff_manifests ->
evidence_receipt, and asserts the verdict, exit status, and artifact content.
Standard library only, local-only (no SSH, no network).
"""
from __future__ import annotations

import json
import os
import sys
import tempfile
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parent))
from snapshot_fixtures import FIXED_NS, build_rich_tree, frozen_request, git_init
from snapshot_diff_fixtures import reseal
from snapshot_gate_fixtures import (changed_fields, diff_and_receipt, paths_of_kind,
                                    run_window, sha256_of)

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
from target_safety.kernel import Blocked
from target_safety.snapshot import LinkException, snapshot
from target_safety.snapshot_diff import diff_manifests, evidence_receipt


# --- HAPPY PATHS -----------------------------------------------------------

def test_no_window_equivalent(tmp_path: Path) -> None:
    """Given a rich tree with no window operation, When running the full gate, Then the
    verdict is equivalent with exit 0, a zero-diff artifact, and a receipt whose recorded
    digests match the retained before/after/artifact bytes."""
    tree = build_rich_tree(tmp_path)
    outcome = run_window(tmp_path, tree)
    assert outcome.verdict == "equivalent" and outcome.exit_status == 0, outcome
    assert outcome.artifact["differences"] == [] and outcome.artifact["reason_codes"] == []
    assert outcome.artifact["verdict"] == "equivalent"
    receipt = outcome.receipt
    assert receipt is not None, outcome.receipt_error
    assert receipt["receipt"] == "protected-path-window/1"
    assert receipt["verdict"] == "equivalent" and receipt["exit_status"] == 0
    assert receipt["before_manifest"]["sha256"] == sha256_of(outcome.before_path)
    assert receipt["after_manifest"]["sha256"] == sha256_of(outcome.after_path)
    assert receipt["diff_artifact"]["sha256"] == sha256_of(outcome.artifact_path)
    print("PASS snapshot gate no-window lifecycle is equivalent with a zero-diff receipt")


def test_out_of_scope_write_equivalent(tmp_path: Path) -> None:
    """Given a window that writes only OUTSIDE the protected root, When running the gate,
    Then the protected inventory is unchanged and the verdict is equivalent."""
    tree = build_rich_tree(tmp_path)
    outside = tmp_path / "outside"
    outside.mkdir()

    def window() -> None:
        (outside / "noise.txt").write_bytes(b"noise\n")
        (outside / "nested").mkdir()
        (outside / "nested" / "deep.bin").write_bytes(b"\x00\x01")

    outcome = run_window(tmp_path, tree, window=window)
    assert outcome.verdict == "equivalent" and outcome.exit_status == 0, outcome
    assert outcome.artifact["differences"] == []
    print("PASS snapshot gate out-of-scope window writes leave the gate equivalent")


# --- FAILURE MODES: must be different(1) or gate_failed(2) ------------------

def test_same_size_content_change_detected(tmp_path: Path) -> None:
    """Given a same-size content rewrite (mtime restored), When running the gate, Then the
    verdict is different and the difference lists the sha256 field, never the size."""
    tree = build_rich_tree(tmp_path)
    target = tree / "keep.txt"
    original_mtime = os.lstat(target).st_mtime_ns

    def window() -> None:
        target.write_bytes(b"XXXXX\n")   # same length as b"hello\n"
        os.utime(target, ns=(original_mtime, original_mtime))

    outcome = run_window(tmp_path, tree, window=window)
    assert outcome.verdict == "different" and outcome.exit_status == 1, outcome
    fields = changed_fields(outcome.artifact, b"keep.txt")
    assert "sha256" in fields, fields
    assert "size" not in fields, fields
    print("PASS snapshot gate detects same-size content change via the sha256 field")


def test_metadata_only_changes_detected(tmp_path: Path) -> None:
    """Given metadata-only windows (chmod and an mtime touch), When running the gate, Then
    each is different and the changed fields name mode and mtime_ns respectively."""
    work_a = tmp_path / "chmod"
    work_a.mkdir()
    tree_a = build_rich_tree(work_a)
    outcome_a = run_window(work_a, tree_a,
                           window=lambda: os.chmod(tree_a / "keep.txt", 0o600))
    assert outcome_a.verdict == "different" and outcome_a.exit_status == 1, outcome_a
    assert "mode" in changed_fields(outcome_a.artifact, b"keep.txt")

    work_b = tmp_path / "touch"
    work_b.mkdir()
    tree_b = build_rich_tree(work_b)

    def touch() -> None:
        os.utime(tree_b / "keep.txt", ns=(FIXED_NS, FIXED_NS))

    outcome_b = run_window(work_b, tree_b, window=touch)
    assert outcome_b.verdict == "different" and outcome_b.exit_status == 1, outcome_b
    assert "mtime_ns" in changed_fields(outcome_b.artifact, b"keep.txt")
    print("PASS snapshot gate detects metadata-only chmod (mode) and mtime touch (mtime_ns)")


def test_write_then_revert_caught_by_ctime(tmp_path: Path) -> None:
    """Given a write-then-revert to the exact original bytes (mtime restored), When running
    the gate, Then the pair is still different and ctime_ns is the field that catches it.
    This is the amendment's headline write-and-revert detection. If ctime_ns were somehow
    unchanged on this filesystem, the assertion would fail and that would be a FINDING."""
    tree = build_rich_tree(tmp_path)
    target = tree / "keep.txt"
    original_mtime = os.lstat(target).st_mtime_ns

    def window() -> None:
        target.write_bytes(b"bye!!\n")    # transient write, same length
        target.write_bytes(b"hello\n")    # revert exact bytes
        os.utime(target, ns=(original_mtime, original_mtime))  # restore mtime

    outcome = run_window(tmp_path, tree, window=window)
    assert outcome.verdict == "different" and outcome.exit_status == 1, outcome
    fields = changed_fields(outcome.artifact, b"keep.txt")
    assert "sha256" not in fields, fields
    assert "size" not in fields, fields
    assert "mtime_ns" not in fields, fields
    assert "ctime_ns" in fields, (
        "FINDING: ctime_ns did NOT change across a write-then-revert on this filesystem; "
        f"changed fields were {fields}. ctime-based evasion narrowing is NOT effective here.")
    print("PASS snapshot gate catches write-then-revert solely via ctime_ns")


def test_add_remove_file_and_dir_detected(tmp_path: Path) -> None:
    """Given windows that add a file, remove a file, add an empty dir and remove a dir,
    When running the gate, Then each is different and the entry appears as added/removed."""
    cases = (
        ("add-file", lambda t: (t / "new.txt").write_bytes(b"new\n"), "added", b"new.txt"),
        ("rm-file", lambda t: os.remove(t / "keep.txt"), "removed", b"keep.txt"),
        ("add-dir", lambda t: (t / "newdir").mkdir(), "added", b"newdir"),
        ("rm-dir", lambda t: os.rmdir(t / "empty"), "removed", b"empty"),
    )
    for name, op, kind, relpath in cases:
        work = tmp_path / name
        work.mkdir()
        tree = build_rich_tree(work)
        outcome = run_window(work, tree, window=lambda t=tree, o=op: o(t))
        assert outcome.verdict == "different" and outcome.exit_status == 1, (name, outcome)
        assert relpath in paths_of_kind(outcome.artifact, kind), (name, relpath, outcome.artifact["differences"])
    print("PASS snapshot gate detects added/removed files and directories each as different")


def test_git_ignored_content_still_detected(tmp_path: Path) -> None:
    """Given a real repository with a .gitignore, When a NEW git-ignored file appears in the
    window, Then the physical inventory still detects it (added) even though the git
    supplement -- which ignores it -- is unchanged."""
    work = tmp_path
    tree = work / "tree"
    tree.mkdir()
    home = work / "home"
    home.mkdir()
    git_init(tree, home)
    (tree / ".gitignore").write_text("*.log\n")

    def window() -> None:
        (tree / "cache.log").write_bytes(b"ignored\n")

    outcome = run_window(work, tree, window=window)
    assert outcome.verdict == "different" and outcome.exit_status == 1, outcome
    assert b"cache.log" in paths_of_kind(outcome.artifact, "added"), outcome.artifact["differences"]
    assert outcome.artifact["git_supplement"]["verdict"] == "identical", (
        "git supplement should NOT flag an ignored file, yet it did: "
        f"{outcome.artifact['git_supplement']}")
    print("PASS snapshot gate detects a new git-ignored file the git supplement never sees")


def test_git_administration_change_detected(tmp_path: Path) -> None:
    """Given a window that mutates .git administration (appends to .git/config), When running
    the gate, Then the verdict is different and the .git/config content digest changed."""
    tree = build_rich_tree(tmp_path)

    def window() -> None:
        with open(tree / ".git" / "config", "ab") as stream:
            stream.write(b"\n[user]\n\tname = e2e\n")

    outcome = run_window(tmp_path, tree, window=window)
    assert outcome.verdict == "different" and outcome.exit_status == 1, outcome
    fields = changed_fields(outcome.artifact, b".git/config")
    assert "sha256" in fields, fields
    print("PASS snapshot gate detects a change inside .git administration")


def test_nested_repository_change_detected(tmp_path: Path) -> None:
    """Given a window that mutates nested-repository content, When running the gate, Then the
    verdict is different and the nested entry's content digest changed."""
    tree = build_rich_tree(tmp_path)

    def window() -> None:
        (tree / "nested-repo" / ".git" / "HEAD").write_text("ref: refs/heads/dev\n")

    outcome = run_window(tmp_path, tree, window=window)
    assert outcome.verdict == "different" and outcome.exit_status == 1, outcome
    fields = changed_fields(outcome.artifact, b"nested-repo/.git/HEAD")
    assert "sha256" in fields, fields
    print("PASS snapshot gate detects a change inside a nested repository")


def test_symlink_retarget_detected(tmp_path: Path) -> None:
    """Given an in-root symlink retarget (a) and an approved-exception link retargeted to the
    wrong text (b), When running the gate, Then (a) is different via the link-target digest
    and (b) blocks the producer (or differs) -- never silent equivalence."""
    # (a) in-root symlink retarget -> different
    work_a = tmp_path / "inlink"
    work_a.mkdir()
    tree_a = build_rich_tree(work_a)

    def retarget_in_root() -> None:
        os.remove(tree_a / "inner-link")
        os.symlink(".gitignore", tree_a / "inner-link")

    outcome_a = run_window(work_a, tree_a, window=retarget_in_root)
    assert outcome_a.verdict == "different" and outcome_a.exit_status == 1, outcome_a
    assert "sha256" in changed_fields(outcome_a.artifact, b"inner-link")

    # (b) approved-exception external link retargeted to wrong text -> producer Blocks
    work_b = tmp_path / "extlink"
    work_b.mkdir()
    tree_b = work_b / "tree"
    tree_b.mkdir()
    (tree_b / "keep.txt").write_bytes(b"hello\n")
    ext_target = "/tmp/opencode/ext-referent-e2e"
    (tree_b / "ext-link").symlink_to(ext_target)
    exception = LinkException(path=tree_b / "ext-link", expected_target=ext_target)

    def retarget_external() -> None:
        os.remove(tree_b / "ext-link")
        os.symlink("/tmp/opencode/other-referent", tree_b / "ext-link")

    outcome_b = run_window(work_b, tree_b, window=retarget_external,
                           link_exceptions=(exception,))
    assert outcome_b.verdict != "equivalent", outcome_b
    assert outcome_b.producer_blocked == "link_exception_text_mismatch" \
        or outcome_b.verdict == "different", outcome_b
    print("PASS snapshot gate blocks a wrong-text exception retarget and differs on in-root retarget")


def test_unapproved_external_link_blocks_closed(tmp_path: Path) -> None:
    """Given a window that adds a NEW unapproved external symlink, When running the gate, Then
    the producer Blocks the after-inventory and the window fails closed as gate_failed."""
    tree = build_rich_tree(tmp_path)

    def window() -> None:
        (tree / "evil").symlink_to("/etc/passwd")

    outcome = run_window(tmp_path, tree, window=window)
    assert outcome.producer_blocked == "unapproved_external_link", outcome
    assert outcome.verdict == "gate_failed" and outcome.exit_status == 2, outcome
    assert outcome.verdict != "equivalent"
    print("PASS snapshot gate fails closed on a new unapproved external symlink")


# --- FAILURE MODES: corrupt / missing / tampered evidence -------------------

def test_truncated_after_manifest_gate_fails(tmp_path: Path) -> None:
    """Given an after-manifest cut mid-JSON, When running the diff, Then the verdict is
    gate_failed because the manifest is malformed."""
    tree = build_rich_tree(tmp_path)
    before_path = tmp_path / "before.json"
    snapshot(frozen_request((tree,), task_id="window-e2e"), before_path)
    scratch = tmp_path / "scratch.json"
    snapshot(frozen_request((tree,), task_id="window-e2e"), scratch)
    after_path = tmp_path / "after.json"
    blob = scratch.read_bytes()
    after_path.write_bytes(blob[: len(blob) // 2])
    outcome = diff_and_receipt(tmp_path, before_path, after_path, "window-e2e")
    assert outcome.verdict == "gate_failed" and outcome.exit_status == 2, outcome
    assert "manifest_malformed" in outcome.reason_codes, outcome.reason_codes
    print("PASS snapshot gate fails closed on a truncated mid-JSON after-manifest")


def test_missing_after_manifest_never_equivalent(tmp_path: Path) -> None:
    """Given no after-manifest at all, When running the diff, Then the outcome is gate_failed
    (and the receipt is blocked at the harness level) -- never equivalent."""
    tree = build_rich_tree(tmp_path)
    before_path = tmp_path / "before.json"
    snapshot(frozen_request((tree,), task_id="window-e2e"), before_path)
    after_path = tmp_path / "after.json"  # deliberately absent
    outcome = diff_and_receipt(tmp_path, before_path, after_path, "window-e2e")
    assert outcome.verdict != "equivalent", outcome
    assert outcome.verdict == "gate_failed" and outcome.exit_status == 2, outcome
    assert outcome.receipt_error is not None, "receipt should be blocked for a missing after-manifest"
    print("PASS snapshot gate never treats a missing after-manifest as equivalent")


def test_incomplete_trailer_gate_fails(tmp_path: Path) -> None:
    """Given an after-manifest whose completion trailer says complete:false, When running the
    diff, Then the verdict is gate_failed via manifest_incomplete."""
    tree = build_rich_tree(tmp_path)
    before_path = tmp_path / "before.json"
    snapshot(frozen_request((tree,), task_id="window-e2e"), before_path)
    scratch = tmp_path / "scratch.json"
    snapshot(frozen_request((tree,), task_id="window-e2e"), scratch)
    data = json.loads(scratch.read_text())
    data["trailer"]["complete"] = False
    after_path = tmp_path / "after.json"
    after_path.write_text(json.dumps(data, sort_keys=True, separators=(",", ":"),
                                     ensure_ascii=True) + "\n")
    outcome = diff_and_receipt(tmp_path, before_path, after_path, "window-e2e")
    assert outcome.verdict == "gate_failed" and outcome.exit_status == 2, outcome
    assert "manifest_incomplete" in outcome.reason_codes, outcome.reason_codes
    print("PASS snapshot gate fails closed on a complete:false completion trailer")


def test_tampered_digest_gate_fails(tmp_path: Path) -> None:
    """Given an otherwise-complete after-manifest with one entry's sha256 flipped (trailer not
    resealed), When running the diff, Then the verdict is gate_failed via
    manifest_digest_mismatch."""
    tree = build_rich_tree(tmp_path)
    before_path = tmp_path / "before.json"
    snapshot(frozen_request((tree,), task_id="window-e2e"), before_path)
    scratch = tmp_path / "scratch.json"
    snapshot(frozen_request((tree,), task_id="window-e2e"), scratch)
    data = json.loads(scratch.read_text())
    victim = next(entry for entry in data["entries"] if entry.get("sha256"))
    old = victim["sha256"]
    victim["sha256"] = ("0" if old[0] != "0" else "1") + old[1:]
    after_path = tmp_path / "after.json"
    after_path.write_text(json.dumps(data, sort_keys=True, separators=(",", ":"),
                                     ensure_ascii=True) + "\n")
    outcome = diff_and_receipt(tmp_path, before_path, after_path, "window-e2e")
    assert outcome.verdict == "gate_failed" and outcome.exit_status == 2, outcome
    assert "manifest_digest_mismatch" in outcome.reason_codes, outcome.reason_codes
    print("PASS snapshot gate fails closed on a tampered entry sha256 (digest mismatch)")


def test_capability_downgrade_gate_fails(tmp_path: Path) -> None:
    """Given an after-manifest hand-crafted with capabilities downgraded (supported ->
    unsupported) and resealed, When running the diff, Then the verdict is gate_failed via
    capability_mismatch."""
    tree = build_rich_tree(tmp_path)
    before_path = tmp_path / "before.json"
    snapshot(frozen_request((tree,), task_id="window-e2e"), before_path)
    scratch = tmp_path / "scratch.json"
    snapshot(frozen_request((tree,), task_id="window-e2e"), scratch)
    data = json.loads(scratch.read_text())
    for record in data["capabilities"]:
        record["xattrs"] = "unsupported"
        record["acls"] = "unsupported"
        record["inode_flags"] = "unsupported"
    reseal(data)
    after_path = tmp_path / "after.json"
    after_path.write_text(json.dumps(data, sort_keys=True, separators=(",", ":"),
                                     ensure_ascii=True) + "\n")
    outcome = diff_and_receipt(tmp_path, before_path, after_path, "window-e2e")
    assert outcome.verdict == "gate_failed" and outcome.exit_status == 2, outcome
    assert "capability_mismatch" in outcome.reason_codes, outcome.reason_codes
    print("PASS snapshot gate fails closed on capability asymmetry (capability_mismatch)")


# --- FAILURE MODES: races, unreadable evidence -------------------------------

def test_identity_race_blocks_clean_manifest(tmp_path: Path) -> None:
    """Given a file whose content changes between the producer's identity-before and
    identity-after read (the after-inventory's final recheck), When snapshotting the AFTER
    inventory, Then the producer Blocks (snapshot_changed) and never emits a clean manifest.
    The producer's own os.open/os.fstat capture seam is used to stage the in-window change."""
    tree = build_rich_tree(tmp_path)
    victim = tree / "keep.txt"          # b"hello\n", 6 bytes
    before_path = tmp_path / "before.json"
    snapshot(frozen_request((tree,), task_id="window-e2e"), before_path)
    after_path = tmp_path / "after.json"
    real_open = os.open
    real_fstat = os.fstat
    state = {"armed": False}

    def arming_open(path, flags, *args, **kwargs):
        fd = real_open(path, flags, *args, **kwargs)
        if os.fsencode(str(path)) == os.fsencode(str(victim)):
            state["armed"] = True
        return fd

    def modifying_fstat(fd):
        info = real_fstat(fd)
        if state["armed"]:
            state["armed"] = False
            with open(victim, "rb+") as stream:  # same-size content change -> ctime bump
                stream.write(b"XXXXX\n")
                stream.flush()
        return info

    producer_blocked = None
    with patch("target_safety.snapshot.os.open", side_effect=arming_open), \
            patch("target_safety.snapshot.os.fstat", side_effect=modifying_fstat):
        try:
            snapshot(frozen_request((tree,), task_id="window-e2e"), after_path)
        except Blocked as error:  # producer fail-closed on the identity-after recheck
            producer_blocked = error.reason
    assert producer_blocked == "snapshot_changed", producer_blocked
    assert not after_path.exists() or after_path.read_bytes() == b"", \
        "a clean after-manifest must never be emitted on a race (empty claim marker at most)"
    outcome = diff_and_receipt(tmp_path, before_path, after_path, "window-e2e", producer_blocked)
    assert outcome.verdict == "gate_failed" and outcome.exit_status == 2, outcome
    print("PASS snapshot gate blocks on an identity-after race and never emits a clean manifest")


def test_unreadable_entry_gate_fails(tmp_path: Path) -> None:
    """Given a file chmod 000 mid-traversal (non-root), When running the gate, Then the after
    manifest records traversal errors and the consumer refuses with gate_failed."""
    if os.geteuid() == 0:
        print("SKIP snapshot gate unreadable-entry scenario (root bypasses mode bits)")
        return
    work = tmp_path
    tree = work / "tree"
    tree.mkdir()
    locked = tree / "locked.txt"
    locked.write_bytes(b"secret\n")

    def window() -> None:
        os.chmod(locked, 0)

    try:
        outcome = run_window(work, tree, window=window)
    finally:
        os.chmod(locked, 0o644)
    assert outcome.verdict == "gate_failed" and outcome.exit_status == 2, outcome
    assert "manifest_traversal_errors" in outcome.reason_codes, outcome.reason_codes
    print("PASS snapshot gate fails closed on an unreadable entry (traversal_errors)")


# --- atime exclusion and determinism -----------------------------------------

def test_atime_only_change_equivalent(tmp_path: Path) -> None:
    """Given a window that only READS a file (updating atime), When running the gate, Then the
    verdict is equivalent because atime is excluded from equivalence by design."""
    tree = build_rich_tree(tmp_path)

    def window() -> None:
        with open(tree / "keep.txt", "rb") as stream:
            stream.read()

    outcome = run_window(tmp_path, tree, window=window)
    assert outcome.verdict == "equivalent" and outcome.exit_status == 0, outcome
    assert outcome.artifact["differences"] == []
    print("PASS snapshot gate treats an atime-only window as equivalent")


def test_diff_and_receipt_deterministic(tmp_path: Path) -> None:
    """Given an identical before/after pair diffed twice, When comparing artifacts and
    receipts, Then the artifacts are byte-identical and the receipts are identical except the
    real-clock recorded_ns (which is a parseable positive integer)."""
    tree = build_rich_tree(tmp_path)
    before_path = tmp_path / "before.json"
    snapshot(frozen_request((tree,)), before_path)
    (tree / "new.txt").write_bytes(b"new\n")
    after_path = tmp_path / "after.json"
    snapshot(frozen_request((tree,)), after_path)

    artifact1 = tmp_path / "diff1.json"
    artifact2 = tmp_path / "diff2.json"
    result1 = diff_manifests(before_path, after_path, artifact1)
    diff_manifests(before_path, after_path, artifact2)
    assert artifact1.read_bytes() == artifact2.read_bytes(), "diff artifacts must be byte-identical"

    receipt1_path = tmp_path / "receipt1.json"
    receipt2_path = tmp_path / "receipt2.json"
    evidence_receipt("task-1", before_path, after_path, artifact1, result1, "trace://det", receipt1_path)
    evidence_receipt("task-1", before_path, after_path, artifact1, result1, "trace://det", receipt2_path)
    receipt1 = json.loads(receipt1_path.read_text())
    receipt2 = json.loads(receipt2_path.read_text())
    recorded1 = receipt1.pop("recorded_ns")
    recorded2 = receipt2.pop("recorded_ns")
    assert receipt1 == receipt2, "receipts must be identical except recorded_ns"
    assert isinstance(recorded1, int) and isinstance(recorded2, int)
    assert recorded1 > 0 and recorded2 > 0
    print("PASS snapshot gate diff and receipt are deterministic across identical runs")


SCENARIOS = (
    ("1", "no-window equivalent", test_no_window_equivalent),
    ("2", "out-of-scope write equivalent", test_out_of_scope_write_equivalent),
    ("3", "same-size content change", test_same_size_content_change_detected),
    ("4", "metadata-only chmod/mtime", test_metadata_only_changes_detected),
    ("5", "write-then-revert ctime", test_write_then_revert_caught_by_ctime),
    ("6", "add/remove file and dir", test_add_remove_file_and_dir_detected),
    ("7", "git-ignored content detected", test_git_ignored_content_still_detected),
    ("8", ".git administration change", test_git_administration_change_detected),
    ("9", "nested repository change", test_nested_repository_change_detected),
    ("10", "symlink retarget", test_symlink_retarget_detected),
    ("11", "unapproved external link", test_unapproved_external_link_blocks_closed),
    ("12", "truncated after-manifest", test_truncated_after_manifest_gate_fails),
    ("13", "missing after-manifest", test_missing_after_manifest_never_equivalent),
    ("14", "complete:false trailer", test_incomplete_trailer_gate_fails),
    ("15", "tampered entry digest", test_tampered_digest_gate_fails),
    ("16", "identity-after race", test_identity_race_blocks_clean_manifest),
    ("17", "unreadable entry", test_unreadable_entry_gate_fails),
    ("18", "capability asymmetry", test_capability_downgrade_gate_fails),
    ("19", "atime-only equivalent", test_atime_only_change_equivalent),
    ("20", "determinism", test_diff_and_receipt_deterministic),
)


def main() -> int:
    os.makedirs("/tmp/opencode", exist_ok=True)
    passed = 0
    failed = 0
    for number, title, case in SCENARIOS:
        tmp_path = Path(tempfile.mkdtemp(prefix=f"snapshot-gate-{number}-", dir="/tmp/opencode"))
        try:
            case(tmp_path)
        except Exception as error:  # noqa: BLE001 - report and continue to next scenario
            print(f"FAIL scenario-{number} {title}: {type(error).__name__}: {error}")
            failed += 1
        else:
            passed += 1
    print(f"SUMMARY snapshot gate e2e: {passed} passed, {failed} failed, {len(SCENARIOS)} total")
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
