# SPDX-License-Identifier: GPL-3.0-or-later
# /// script
# requires-python = ">=3.11"
# dependencies = []
# ///
# Run: python3 -B tests/target_safety/snapshot_changes.py
"""Change detection: metadata-only touches, content changes, atime exclusion."""
from __future__ import annotations

import dataclasses
import hashlib
import os
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from snapshot_fixtures import entry_map, frozen_request, run_cases

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
from target_safety.snapshot import equivalence, snapshot


def _changed_fields(before, after, relpath: bytes) -> list[str]:
    prefix = f"entries~:0:{os.fsdecode(relpath)}:"
    for difference in equivalence(before, after).differences:
        if difference.startswith(prefix):
            return difference.rsplit(":", 1)[1].split(",")
    return []


def test_touch_mtime_only_change_detected(tmp_path: Path) -> None:
    """Given two snapshots around an mtime-only touch, When diffing, Then the change is
    detected and only timing metadata fields differ, never the content digest."""
    tree = tmp_path / "tree"
    tree.mkdir()
    target = tree / "f.txt"
    target.write_bytes(b"data\n")
    before = snapshot(frozen_request((tree,)), tmp_path / "before.json")
    os.utime(target, ns=(1_700_000_000_000_000_000, 1_700_000_000_000_000_000))
    after = snapshot(frozen_request((tree,)), tmp_path / "after.json")
    diff = equivalence(before, after)
    assert diff.equivalent is False and diff.reason == "body_differs"
    changed = _changed_fields(before, after, b"f.txt")
    assert "mtime_ns" in changed and "ctime_ns" in changed, changed
    assert "sha256" not in changed and "size" not in changed, changed
    print("PASS snapshot detects metadata-only mtime touch without digest noise")


def test_chmod_only_change_detected(tmp_path: Path) -> None:
    """Given two snapshots around a chmod, When diffing, Then the mode change is detected
    and no content digest changes."""
    tree = tmp_path / "tree"
    tree.mkdir()
    target = tree / "f.txt"
    target.write_bytes(b"data\n")
    before = snapshot(frozen_request((tree,)), tmp_path / "before.json")
    os.chmod(target, 0o600)
    after = snapshot(frozen_request((tree,)), tmp_path / "after.json")
    diff = equivalence(before, after)
    assert diff.equivalent is False and diff.reason == "body_differs"
    changed = _changed_fields(before, after, b"f.txt")
    assert "mode" in changed and "sha256" not in changed, changed
    print("PASS snapshot detects chmod-only mode change")


def test_chown_like_field_change_detected(tmp_path: Path) -> None:
    """Given unprivileged execution where chown is a no-op, When a recorded ownership
    field differs between manifests, Then equivalence still refuses the pair."""
    tree = tmp_path / "tree"
    tree.mkdir()
    (tree / "f.txt").write_bytes(b"data\n")
    before = snapshot(frozen_request((tree,)), tmp_path / "before.json")
    entry = entry_map(before)[b"f.txt"]
    assert entry.uid == os.geteuid() and entry.gid == os.getegid()
    altered = dataclasses.replace(entry, uid=entry.uid + 1)
    after = dataclasses.replace(
        before,
        entries=tuple(altered if item.path == b"f.txt" else item for item in before.entries),
    )
    diff = equivalence(before, after)
    assert diff.equivalent is False and diff.reason == "body_differs"
    assert any("uid" in difference for difference in diff.differences), diff.differences
    print("PASS snapshot detects chown-like ownership field change where unprivileged")


def test_content_change_detected_with_same_size(tmp_path: Path) -> None:
    """Given a rewrite that keeps size and restores mtime, When diffing, Then the
    SHA-256 change is detected, proving no metadata-implies-unchanged-digest shortcut."""
    tree = tmp_path / "tree"
    tree.mkdir()
    target = tree / "f.txt"
    target.write_bytes(b"aaaa\n")
    info = os.lstat(target)
    before = snapshot(frozen_request((tree,)), tmp_path / "before.json")
    target.write_bytes(b"bbbb\n")
    os.utime(target, ns=(info.st_mtime_ns, info.st_mtime_ns))
    after = snapshot(frozen_request((tree,)), tmp_path / "after.json")
    assert entry_map(before)[b"f.txt"].size == entry_map(after)[b"f.txt"].size
    assert entry_map(after)[b"f.txt"].sha256 == hashlib.sha256(b"bbbb\n").hexdigest()
    diff = equivalence(before, after)
    assert diff.equivalent is False and diff.reason == "body_differs"
    changed = _changed_fields(before, after, b"f.txt")
    assert "sha256" in changed and "size" not in changed, changed
    print("PASS snapshot detects same-size content change via SHA-256 alone")


def test_atime_excluded_from_equivalence(tmp_path: Path) -> None:
    """Given traversal itself updating atime, When diffing two snapshots of the same
    content, Then manifests stay equivalent because atime is never compared."""
    tree = tmp_path / "tree"
    tree.mkdir()
    target = tree / "f.txt"
    target.write_bytes(b"data\n")
    before = snapshot(frozen_request((tree,)), tmp_path / "before.json")
    with open(target, "rb") as stream:
        stream.read()
    after = snapshot(frozen_request((tree,)), tmp_path / "after.json")
    diff = equivalence(before, after)
    assert diff.equivalent is True and diff.differences == ()
    assert "atime" not in entry_map(before)[b"f.txt"].jsonable()
    print("PASS snapshot excludes atime so traversal reads never break equivalence")


def test_unchanged_tree_is_equivalent(tmp_path: Path) -> None:
    """Given a frozen tree, When diffing two snapshots, Then the canonical diff is zero."""
    tree = tmp_path / "tree"
    tree.mkdir()
    (tree / "f.txt").write_bytes(b"data\n")
    before = snapshot(frozen_request((tree,)), tmp_path / "before.json")
    after = snapshot(frozen_request((tree,)), tmp_path / "after.json")
    diff = equivalence(before, after)
    assert diff.equivalent is True and diff.reason == "identical" and diff.differences == ()
    assert diff.before_digest == diff.after_digest
    print("PASS snapshot unchanged tree yields a machine-readable zero-diff")


def main() -> int:
    return run_cases((
        test_touch_mtime_only_change_detected,
        test_chmod_only_change_detected,
        test_chown_like_field_change_detected,
        test_content_change_detected_with_same_size,
        test_atime_excluded_from_equivalence,
        test_unchanged_tree_is_equivalent,
    ), "snapshot-changes-")


if __name__ == "__main__":
    raise SystemExit(main())
