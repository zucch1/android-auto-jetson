# /// script
# requires-python = ">=3.11"
# dependencies = []
# ///
# Run: python3 -B tests/target_safety/snapshot_gate_review_fixes.py
"""Regression fixes for the independent adversarial snapshot-gate review (task 5).

One case per review finding, replaying the attack shapes from
/tmp/opencode/gate-attacks/ against the fixed producer/consumer. F9 note:
ctime-based write-then-revert detection is cross-tick only -- a same-tick
write+revert with restored mtime is an accepted residual per the review
(window serialization/quiescence is the mitigation), and F10/F12 note: the
manifest digest is self-consistency, not a MAC, and the target identity is
an unauthenticated caller string -- both accepted limitations.
"""
from __future__ import annotations

import base64
import json
import os
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from snapshot_diff_fixtures import (FIXED_NS, make_entry, make_manifest,
                                    reseal, run_cases, write_manifest)
from snapshot_fixtures import frozen_request

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
from target_safety.kernel import Blocked
from target_safety.snapshot import LinkException, snapshot
from target_safety.snapshot_diff import diff_manifests, validate_manifest


def _blocked(label: str, code: str, call) -> None:
    try:
        call()
    except Blocked as error:
        assert error.reason == code, f"{label}: {error.reason} != {code}"
        return
    raise AssertionError(f"{label}: accepted, expected {code}")


def test_f1_stale_after_manifest_never_certified(tmp_path: Path) -> None:
    """F1: a Blocked AFTER snapshot at a reused path leaves the empty claim marker
    (never the stale prior manifest), so the gate fails closed instead of certifying
    equivalent from stale evidence (attack C replay)."""
    tree = tmp_path / "tree"
    tree.mkdir()
    (tree / "keep.txt").write_text("hello\n")
    before, after = tmp_path / "before.json", tmp_path / "after.json"
    snapshot(frozen_request((tree,), task_id="w1"), before)
    snapshot(frozen_request((tree,), task_id="w1"), after)
    first = diff_manifests(before, after, tmp_path / "d1.json")
    assert first.verdict == "equivalent", first
    stale_bytes = after.read_bytes()
    snapshot(frozen_request((tree,), task_id="w2"), before)
    (tree / "keep.txt").write_text("CHANGED\n")
    (tree / "bad-link").symlink_to("/etc/passwd")
    _blocked("stale-after", "unapproved_external_link",
             lambda: snapshot(frozen_request((tree,), task_id="w2"), after))
    assert after.read_bytes() == b"", "Blocked run must leave only the empty claim marker"
    assert after.read_bytes() != stale_bytes
    result = diff_manifests(before, after, tmp_path / "d2.json")
    assert result.verdict == "gate_failed" and result.exit_status == 2, result
    assert "manifest_malformed" in result.reason_codes, result.reason_codes
    print("PASS F1 blocked after-snapshot leaves no stale manifest and the gate fails closed")


def test_f1_manifest_published_atomically(tmp_path: Path) -> None:
    """F1: the manifest appears only as a complete file and no temp file survives."""
    tree = tmp_path / "tree"
    tree.mkdir()
    (tree / "keep.txt").write_text("hello\n")
    out = tmp_path / "manifest.json"
    snapshot(frozen_request((tree,)), out)
    payload = json.loads(out.read_text())
    assert payload["trailer"]["complete"] is True
    assert out.read_bytes().endswith(b"\n")
    leftovers = [p.name for p in tmp_path.iterdir() if p.suffix == ".tmp"]
    assert leftovers == [], leftovers
    print("PASS F1 manifest is published atomically with no surviving temp file")


def test_f2_two_root_change_not_shadowed(tmp_path: Path) -> None:
    """F2: entries are keyed by (root, path); a root0-only change with same relative
    names in both roots is detected through the public gate (attack D6 replay)."""
    work = tmp_path / "two-root"
    root_prot, root_cg = work / "protected", work / "codegraph"
    for root in (root_prot, root_cg):
        root.mkdir(parents=True)
        (root / "shared.txt").write_text("v1\n")
    before = tmp_path / "before.json"
    snapshot(frozen_request((root_prot, root_cg)), before)
    (root_prot / "shared.txt").write_text("v2-CHANGED\n")
    after = tmp_path / "after.json"
    snapshot(frozen_request((root_prot, root_cg)), after)
    result = diff_manifests(before, after, tmp_path / "diff.json")
    assert result.verdict == "different" and result.exit_status == 1, result
    changed = [d for d in result.differences
               if d.get("kind") == "changed" and d.get("path") == ["u", "shared.txt"]]
    assert changed, result.differences
    print("PASS F2 root0-only change under a shared relative name is detected")


def test_f3_two_root_no_change_equivalent(tmp_path: Path) -> None:
    """F3: the contract's two-root scope (protected + referent, same relative names)
    passes the gate on a no-change window (attack A replay)."""
    work = tmp_path / "two-root"
    root_prot, root_cg = work / "protected", work / "codegraph"
    for root in (root_prot, root_cg):
        (root / "sub").mkdir(parents=True)
        (root / "shared.txt").write_text("same-name-different-root\n")
        (root / "sub" / "only.txt").write_text(f"only-in-{root.name}\n")
    before, after = tmp_path / "before.json", tmp_path / "after.json"
    snapshot(frozen_request((root_prot, root_cg), task_id="w1"), before)
    snapshot(frozen_request((root_prot, root_cg), task_id="w2"), after)
    result = diff_manifests(before, after, tmp_path / "diff.json")
    assert result.verdict == "equivalent" and result.exit_status == 0, result
    assert result.differences == (), result.differences
    print("PASS F3 no-change two-root pair is equivalent (no entry-order misfire)")


def test_f3_entry_order_uses_root_then_raw_bytes(tmp_path: Path) -> None:
    """F3: order is the producer's (root, raw path bytes) key -- NOT the JSON spelling,
    which sorts 'b' tags before 'u' tags against byte order."""
    b_entry = make_entry()
    b_entry["path"] = ["b", base64.b64encode(b"\xff").decode("ascii")]
    u_entry = make_entry("a.txt")
    reseal_ok = make_manifest(entries=[u_entry, b_entry])
    validate_manifest(reseal_ok)
    bad_order = make_manifest(entries=[b_entry, u_entry])
    _blocked("tag-order", "manifest_entry_order", lambda: validate_manifest(bad_order))
    root0, root1 = make_entry(""), make_entry("")
    root1["root"] = 1
    validate_manifest(make_manifest(entries=[root0, root1],
                                    capabilities=[{"root": 0, "xattrs": "supported",
                                                   "acls": "supported",
                                                   "inode_flags": "supported", "detail": ""},
                                                  {"root": 1, "xattrs": "supported",
                                                   "acls": "supported",
                                                   "inode_flags": "supported", "detail": ""}]))
    print("PASS F3 entry order keys on (root, raw bytes) and accepts two-root ties")


def test_f4_provenance_mismatch_different(tmp_path: Path) -> None:
    """F4: manifests from different targets/boots (attack B1 replay) are never equivalent."""
    tree = tmp_path / "tree"
    tree.mkdir()
    (tree / "keep.txt").write_text("hello\n")
    before, after = tmp_path / "before.json", tmp_path / "after.json"
    snapshot(frozen_request((tree,), task_id="w1", target_identity="host-A",
                            boot_id="boot-1"), before)
    snapshot(frozen_request((tree,), task_id="w2", target_identity="host-B",
                            boot_id="boot-2"), after)
    result = diff_manifests(before, after, tmp_path / "diff.json")
    assert result.verdict == "different" and result.exit_status == 1, result
    fields = {item["field"] for item in result.differences if item.get("kind") == "changed"}
    assert {"target_identity", "boot_id"} <= fields, result.differences
    print("PASS F4 provenance mismatch is verdict-different, never equivalent")


def test_f4_link_exception_evidence_mismatch(tmp_path: Path) -> None:
    """F4: the declared link-exception set is must-match evidence (attack B2 evidence)."""
    before = make_manifest(link_exceptions=[
        {"path": ["u", "/t/l"], "expected_target": ["u", "/usr/bin/python3"]}])
    after = make_manifest()
    result = diff_manifests(write_manifest(tmp_path, before, "b.json"),
                            write_manifest(tmp_path, after, "a.json"),
                            tmp_path / "diff.json")
    assert result.verdict == "different", result
    assert {"field": "link_exceptions", "kind": "changed"} in result.differences
    print("PASS F4 link_exceptions evidence mismatch is verdict-different")


def test_f5_symlinked_evidence_path_rejected(tmp_path: Path) -> None:
    """F5: an evidence path reached through a symlink farm into the protected tree is
    rejected before any write (attack H replay)."""
    tree = tmp_path / "tree"
    tree.mkdir()
    (tree / "keep.txt").write_text("hello\n")
    secret = tree / ".secret-evidence"
    secret.mkdir()
    outside = tmp_path / "evidence-area"
    outside.mkdir()
    farm = outside / "farm"
    farm.symlink_to(secret)
    before = outside / "before.json"
    snapshot(frozen_request((tree,), task_id="w1"), before)
    _blocked("symlink-farm", "evidence_inside_roots",
             lambda: snapshot(frozen_request((tree,), task_id="w1"), farm / "after.json"))
    assert list(secret.iterdir()) == [], list(secret.iterdir())
    print("PASS F5 symlinked evidence path inside the root is rejected with no leak")


def test_f6_alias_key_conflicts_rejected(tmp_path: Path) -> None:
    """F6: two spellings of one alias pair with disagreeing values are malformed
    (attack D3/D4 replay); identical spellings are accepted."""
    manifest = make_manifest()
    conflict = make_manifest()
    conflict["task_window_id"] = "window-IMPOSTOR"
    reseal(conflict)
    _blocked("task-alias", "manifest_malformed", lambda: validate_manifest(conflict))
    trailer_conflict = make_manifest()
    trailer_conflict["completion_trailer"] = {"complete": True, "manifest_digest": "f" * 64}
    reseal(trailer_conflict)
    _blocked("trailer-alias", "manifest_malformed",
             lambda: validate_manifest(trailer_conflict))
    agree = make_manifest()
    agree["task_window_id"] = agree["task_id"]
    reseal(agree)
    validate_manifest(agree)
    validate_manifest(manifest)
    print("PASS F6 conflicting alias spellings are manifest_malformed; agreeing ones pass")


def test_f7_entry_root_required(tmp_path: Path) -> None:
    """F7: entry root is a required int; root-stripped manifests (attack E6 replay) fail
    validation and capability-conditioned fields are never silently skipped."""
    stripped = make_manifest()
    for entry in stripped["entries"]:
        entry.pop("root", None)
    reseal(stripped)
    _blocked("root-stripped", "manifest_missing_field", lambda: validate_manifest(stripped))
    bad_type = make_manifest()
    bad_type["entries"][0]["root"] = "0"
    reseal(bad_type)
    _blocked("root-str", "manifest_malformed", lambda: validate_manifest(bad_type))
    before = make_manifest(entries=[make_entry(xattrs=[["u", "user.k"], "d" * 64])])
    after = make_manifest(entries=[make_entry(xattrs=[["u", "user.k"], "e" * 64])])
    result = diff_manifests(write_manifest(tmp_path, before, "b.json"),
                            write_manifest(tmp_path, after, "a.json"),
                            tmp_path / "diff.json")
    changed = [item for item in result.differences if item.get("kind") == "changed"]
    assert changed and "xattrs" in changed[0]["fields"], result.differences
    print("PASS F7 entry root is required and xattr drift stays compared")


def test_f8_duplicate_link_exception_rejected(tmp_path: Path) -> None:
    """F8: the same path declared twice is rejected at request validation (attack B2
    replay) so the exactly-once rule cannot be defeated."""
    tree = tmp_path / "tree"
    tree.mkdir()
    link = tree / "the-link"
    link.symlink_to("/usr/bin/python3")
    exc = LinkException(link, "/usr/bin/python3")
    _blocked("duplicate-exception", "link_exception_duplicate",
             lambda: snapshot(frozen_request((tree,), link_exceptions=(exc, exc)),
                              tmp_path / "manifest.json"))
    print("PASS F8 duplicate declared link exception is rejected at request validation")


def test_f8_link_classification_fields_compared(tmp_path: Path) -> None:
    """F8: link_target and external_target are compared entry fields, so a symlink
    reclassification is caught even when sha256/ctime agree."""
    before = make_manifest(entries=[make_entry(link_target=["u", "/usr/bin/python3"])])
    after = make_manifest(entries=[make_entry(link_target=["u", "/bin/sh"])])
    result = diff_manifests(write_manifest(tmp_path, before, "b.json"),
                            write_manifest(tmp_path, after, "a.json"),
                            tmp_path / "diff.json")
    fields = set()
    for item in result.differences:
        if item.get("kind") == "changed":
            fields |= set(item["fields"])
    assert "link_target" in fields, result.differences
    flip_before = make_manifest(entries=[make_entry(external_target=False)])
    flip_after = make_manifest(entries=[make_entry(external_target=True)])
    result = diff_manifests(write_manifest(tmp_path, flip_before, "fb.json"),
                            write_manifest(tmp_path, flip_after, "fa.json"),
                            tmp_path / "diff2.json")
    fields = set()
    for item in result.differences:
        if item.get("kind") == "changed":
            fields |= set(item["fields"])
    assert "external_target" in fields, result.differences
    print("PASS F8 link_target and external_target drift is caught by the comparison")


def test_f9_ctime_cross_tick_only_documented(tmp_path: Path) -> None:
    """F9: ctime narrows write-and-revert evasion to cross-tick detection only; a pair
    indistinguishable within one ctime tick is the accepted residual (see module
    docstring: window serialization/quiescence is the mitigation)."""
    cross_tick = make_manifest(entries=[make_entry(ctime_ns=FIXED_NS)])
    reverted = make_manifest(entries=[make_entry(ctime_ns=FIXED_NS + 1)])
    result = diff_manifests(write_manifest(tmp_path, cross_tick, "b.json"),
                            write_manifest(tmp_path, reverted, "a.json"),
                            tmp_path / "diff.json")
    assert result.verdict == "different", result
    fields = set(result.differences[0]["fields"])
    assert fields == {"ctime_ns"}, fields
    same_tick = make_manifest()
    result = diff_manifests(write_manifest(tmp_path, same_tick, "sb.json"),
                            write_manifest(tmp_path, make_manifest(), "sa.json"),
                            tmp_path / "diff2.json")
    assert result.verdict == "equivalent", result
    print("PASS F9 cross-tick write-then-revert detected via ctime_ns; same-tick "
          "revert is the documented accepted residual")


def test_f10_digest_self_consistency_documented(tmp_path: Path) -> None:
    """F10 (documented limitation): the trailer digest is self-consistency, not a MAC --
    a tampered manifest with a recomputed digest validates (in-threat-model residual)."""
    tampered = make_manifest()
    tampered["entries"][0]["sha256"] = "0" * 64
    reseal(tampered)
    validate_manifest(tampered)
    print("PASS F10 digest is self-consistency only (documented, not tamper evidence)")


def test_f11_noncanonical_name_encoding_rejected(tmp_path: Path) -> None:
    """F11: a 'b'-tagged payload holding valid UTF-8 is manifest_malformed (attack D5
    replay); the canonical 'b' spelling of non-UTF-8 bytes is accepted."""
    ambiguous = make_manifest()
    ambiguous["entries"][0]["path"] = ["b", base64.b64encode(b"keep.txt").decode("ascii")]
    reseal(ambiguous)
    _blocked("utf8-b-tag", "manifest_malformed", lambda: validate_manifest(ambiguous))
    canonical = make_manifest()
    canonical["entries"][0]["path"] = ["b", base64.b64encode(b"bad-\xff").decode("ascii")]
    canonical["entries"][0]["type"] = "file"
    reseal(canonical)
    validate_manifest(canonical)
    print("PASS F11 non-canonical 'b'-tagged UTF-8 name is manifest_malformed")


def main() -> int:
    return run_cases((
        test_f1_stale_after_manifest_never_certified,
        test_f1_manifest_published_atomically,
        test_f2_two_root_change_not_shadowed,
        test_f3_two_root_no_change_equivalent,
        test_f3_entry_order_uses_root_then_raw_bytes,
        test_f4_provenance_mismatch_different,
        test_f4_link_exception_evidence_mismatch,
        test_f5_symlinked_evidence_path_rejected,
        test_f6_alias_key_conflicts_rejected,
        test_f7_entry_root_required,
        test_f8_duplicate_link_exception_rejected,
        test_f8_link_classification_fields_compared,
        test_f9_ctime_cross_tick_only_documented,
        test_f10_digest_self_consistency_documented,
        test_f11_noncanonical_name_encoding_rejected,
    ), "snapshot-gate-review-")


if __name__ == "__main__":
    raise SystemExit(main())
