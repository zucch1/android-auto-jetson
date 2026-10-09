# /// script
# requires-python = ">=3.11"
# dependencies = []
# ///
# Run: python3 -B tests/target_safety/snapshot_races.py
"""Race and evidence-gap handling: truncation, vanish, unreadable entries never equivalence."""
from __future__ import annotations

import os
import sys
import time
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parent))
from snapshot_fixtures import entry_map, frozen_request, run_cases

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
from target_safety.kernel import Blocked
from target_safety.snapshot import equivalence, snapshot


def test_missing_file_mid_traversal_blocks(tmp_path: Path) -> None:
    """Given a file that vanishes between directory listing and read, When snapshotting,
    Then Blocked('snapshot_entry_vanished') is raised and no manifest is written."""
    tree = tmp_path / "tree"
    tree.mkdir()
    victim = tree / "victim.txt"
    victim.write_bytes(b"content\n")
    out = tmp_path / "manifest.json"
    real_open = os.open

    def vanishing_open(path, flags, *args, **kwargs):
        if os.fsencode(str(path)) == os.fsencode(str(victim)):
            raise FileNotFoundError(2, "No such file or directory", str(path))
        return real_open(path, flags, *args, **kwargs)

    with patch("target_safety.snapshot.os.open", side_effect=vanishing_open):
        try:
            snapshot(frozen_request((tree,)), out)
        except Blocked as error:
            assert error.reason == "snapshot_entry_vanished", str(error)
            assert not out.exists() or out.read_bytes() == b"", \
                "a Blocked run must leave no manifest (empty claim marker at most)"
            print(f"PASS snapshot missing file mid-traversal blocks: {error}")
            return
    raise AssertionError("vanished file accepted")


def test_truncated_file_mid_traversal_blocks(tmp_path: Path) -> None:
    """Given a file truncated between size observation and read completion, When
    snapshotting, Then Blocked('snapshot_truncated_read') is raised."""
    tree = tmp_path / "tree"
    tree.mkdir()
    victim = tree / "victim.txt"
    victim.write_bytes(b"0123456789")
    out = tmp_path / "manifest.json"
    real_open = os.open
    state = {"armed": False}

    def truncating_open(path, flags, *args, **kwargs):
        fd = real_open(path, flags, *args, **kwargs)
        if os.fsencode(str(path)) == os.fsencode(str(victim)):
            state["armed"] = True
        return fd

    real_fstat = os.fstat

    def lying_fstat(fd):
        info = real_fstat(fd)
        if state["armed"]:
            state["armed"] = False
            os.truncate(victim, 0)
        return info

    with patch("target_safety.snapshot.os.open", side_effect=truncating_open), \
            patch("target_safety.snapshot.os.fstat", side_effect=lying_fstat):
        try:
            snapshot(frozen_request((tree,)), out)
        except Blocked as error:
            assert error.reason == "snapshot_truncated_read", str(error)
            assert not out.exists() or out.read_bytes() == b"", \
                "a Blocked run must leave no manifest (empty claim marker at most)"
            print(f"PASS snapshot truncated file mid-traversal blocks: {error}")
            return
    raise AssertionError("truncated file accepted")


def test_unreadable_entry_recorded_not_equivalence(tmp_path: Path) -> None:
    """Given an unreadable entry, When snapshotting, Then the gap is recorded as a
    traversal error, the entry stays inventoried, and no pair is ever equivalence."""
    if os.geteuid() == 0:
        print("SKIP snapshot unreadable-entry gate (root bypasses mode bits)")
        return
    tree = tmp_path / "tree"
    tree.mkdir()
    locked = tree / "locked.txt"
    locked.write_bytes(b"secret\n")
    clean = snapshot(frozen_request((tree,)), tmp_path / "clean.json")
    os.chmod(locked, 0)
    try:
        dirty = snapshot(frozen_request((tree,)), tmp_path / "dirty.json")
    finally:
        os.chmod(locked, 0o644)
    reasons = {error.reason for error in dirty.traversal_errors}
    assert "unreadable_file" in reasons, reasons
    entry = entry_map(dirty)[b"locked.txt"]
    assert entry.sha256 is None and entry.type == "file"
    assert equivalence(clean, dirty).equivalent is False
    assert equivalence(dirty, dirty).reason == "traversal_errors_present"
    print("PASS snapshot unreadable entry is recorded and never equivalence")


def test_unreadable_subtree_recorded_not_equivalence(tmp_path: Path) -> None:
    """Given an unreadable directory, When snapshotting, Then the skipped subtree is a
    recorded traversal error and equivalence is refused."""
    if os.geteuid() == 0:
        print("SKIP snapshot unreadable-subtree gate (root bypasses mode bits)")
        return
    tree = tmp_path / "tree"
    (tree / "hidden").mkdir(parents=True)
    (tree / "hidden" / "inner.txt").write_bytes(b"inner\n")
    (tree / "visible.txt").write_bytes(b"ok\n")
    clean = snapshot(frozen_request((tree,)), tmp_path / "clean.json")
    os.chmod(tree / "hidden", 0)
    try:
        dirty = snapshot(frozen_request((tree,)), tmp_path / "dirty.json")
    finally:
        os.chmod(tree / "hidden", 0o755)
    reasons = {error.reason for error in dirty.traversal_errors}
    assert "unreadable_directory" in reasons, reasons
    assert entry_map(dirty)[b"visible.txt"].sha256 is not None
    assert equivalence(clean, dirty).reason == "traversal_errors_present"
    print("PASS snapshot unreadable subtree is recorded and never equivalence")


def test_header_mismatch_refuses_equivalence(tmp_path: Path) -> None:
    """Given two manifests with different target identities, When diffing, Then the
    comparison refuses with a header mismatch instead of claiming filesystem change."""
    tree = tmp_path / "tree"
    tree.mkdir()
    (tree / "f.txt").write_bytes(b"data\n")
    before = snapshot(frozen_request((tree,), target_identity="target-a"),
                      tmp_path / "before.json")
    after = snapshot(frozen_request((tree,), target_identity="target-b"),
                     tmp_path / "after.json")
    diff = equivalence(before, after)
    assert diff.equivalent is False
    assert diff.reason == "header_mismatch:target_identity", diff.reason
    print("PASS snapshot provenance header mismatch refuses equivalence")


class _RacingScan:
    """Context-manager shim returning pre-listed entries for a patched scandir."""

    def __init__(self, entries: list) -> None:
        self._entries = entries

    def __enter__(self):
        return iter(self._entries)

    def __exit__(self, *exc) -> bool:
        return False


def test_directory_membership_change_blocks(tmp_path: Path) -> None:
    """Given a directory gaining an entry while it is being listed, When snapshotting,
    Then Blocked('snapshot_directory_race') is raised and no manifest is written."""
    tree = tmp_path / "tree"
    (tree / "sub").mkdir(parents=True)
    (tree / "sub" / "a.txt").write_bytes(b"a\n")
    real_scandir = os.scandir
    armed = {"pending": True}

    def racing_scandir(path):
        entries = list(real_scandir(path))
        if armed["pending"] and os.fsencode(str(path)) == os.fsencode(str(tree / "sub")):
            armed["pending"] = False
            time.sleep(0.05)
            (tree / "sub" / "late.txt").write_bytes(b"late\n")
        return _RacingScan(entries)

    out = tmp_path / "manifest.json"
    with patch("target_safety.snapshot_walk.os.scandir", side_effect=racing_scandir):
        try:
            snapshot(frozen_request((tree,)), out)
        except Blocked as error:
            assert error.reason == "snapshot_directory_race", str(error)
            assert not out.exists() or out.read_bytes() == b"", \
                "a Blocked run must leave no manifest (empty claim marker at most)"
            print(f"PASS snapshot directory membership change blocks: {error}")
            return
    raise AssertionError("directory membership change accepted")


def test_inode_replacement_race_blocks(tmp_path: Path) -> None:
    """Given the opened inode differing from the pre-open identity, When snapshotting,
    Then Blocked('snapshot_inode_race') is raised and no manifest is written."""
    tree = tmp_path / "tree"
    tree.mkdir()
    (tree / "f.txt").write_bytes(b"data\n")
    real_fstat = os.fstat

    def swapping_fstat(fd):
        info = real_fstat(fd)
        return os.stat_result((info.st_mode, info.st_ino + 1, info.st_dev, info.st_nlink,
                               info.st_uid, info.st_gid, info.st_size,
                               info.st_atime, info.st_mtime, info.st_ctime))

    out = tmp_path / "manifest.json"
    with patch("target_safety.snapshot_capture.os.fstat", side_effect=swapping_fstat):
        try:
            snapshot(frozen_request((tree,)), out)
        except Blocked as error:
            assert error.reason == "snapshot_inode_race", str(error)
            assert not out.exists() or out.read_bytes() == b"", \
                "a Blocked run must leave no manifest (empty claim marker at most)"
            print(f"PASS snapshot inode replacement race blocks: {error}")
            return
    raise AssertionError("inode race accepted")


def main() -> int:
    return run_cases((
        test_missing_file_mid_traversal_blocks,
        test_truncated_file_mid_traversal_blocks,
        test_unreadable_entry_recorded_not_equivalence,
        test_unreadable_subtree_recorded_not_equivalence,
        test_header_mismatch_refuses_equivalence,
        test_directory_membership_change_blocks,
        test_inode_replacement_race_blocks,
    ), "snapshot-races-")


if __name__ == "__main__":
    raise SystemExit(main())
