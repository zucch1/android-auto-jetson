# SPDX-License-Identifier: GPL-3.0-or-later
# /// script
# requires-python = ">=3.11"
# dependencies = []
# ///
# Run: python3 -B tests/target_safety/snapshot_inventory.py
"""Physical-inventory coverage: every entry, no exclusions, lossless names."""
from __future__ import annotations

import hashlib
import os
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from snapshot_fixtures import (RICH_PATHS, build_rich_tree, entry_map,
                               expected_hashed_bytes, frozen_request, run_cases)

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
from target_safety.snapshot import decode_name, encode_name, snapshot


def test_every_physical_entry_inventoried(tmp_path: Path) -> None:
    """Given a tree with ignored files, .git admin, venv, build output and a nested repo,
    When snapshotting, Then every physical entry is recorded exactly once with no exclusions."""
    tree = build_rich_tree(tmp_path)
    manifest = snapshot(frozen_request((tree,)), tmp_path / "manifest.json")
    assert tuple(entry.path for entry in manifest.entries) == RICH_PATHS
    assert manifest.entries[0].path == b"" and manifest.entries[0].type == "dir"
    assert entry_map(manifest)[b".git/HEAD"].type == "file"
    assert entry_map(manifest)[b"nested-repo/.git/HEAD"].type == "file"
    assert entry_map(manifest)[b"ignored.log"].sha256 == hashlib.sha256(b"ignored\n").hexdigest()
    assert entry_map(manifest)[b"keep.txt"].sha256 == hashlib.sha256(b"hello\n").hexdigest()
    assert manifest.bytes_hashed == expected_hashed_bytes(tree)
    print("PASS snapshot inventory records every physical entry with zero exclusions")


def test_empty_directory_recorded(tmp_path: Path) -> None:
    """Given an empty directory, When snapshotting, Then a dir entry exists with no digest."""
    tree = tmp_path / "tree"
    (tree / "void").mkdir(parents=True)
    manifest = snapshot(frozen_request((tree,)), tmp_path / "manifest.json")
    empty = entry_map(manifest)[b"void"]
    assert empty.type == "dir" and empty.sha256 is None and empty.size is None
    assert len(manifest.entries) == 2
    print("PASS snapshot inventory records empty directories as first-class entries")


def test_non_utf8_filename_round_trips(tmp_path: Path) -> None:
    """Given a filename with invalid UTF-8 bytes, When snapshotting, Then the raw
    bytes round-trip losslessly through the tagged name encoding."""
    tree = tmp_path / "tree"
    tree.mkdir()
    raw_name = b"bad-\xff-name"
    fd = os.open(os.path.join(os.fsencode(str(tree)), raw_name),
                 os.O_CREAT | os.O_WRONLY, 0o644)
    os.write(fd, b"payload")
    os.close(fd)
    manifest = snapshot(frozen_request((tree,)), tmp_path / "manifest.json")
    entry = entry_map(manifest)[raw_name]
    assert entry.sha256 == hashlib.sha256(b"payload").hexdigest()
    tag, payload = encode_name(raw_name)
    assert tag == "b" and decode_name((tag, payload)) == raw_name
    print("PASS snapshot inventory round-trips non-UTF8 filename bytes losslessly")


def test_deep_nesting_recorded(tmp_path: Path) -> None:
    """Given a 300-level deep chain, When snapshotting, Then the deepest entry is
    recorded and the iterative walk never raises RecursionError."""
    tree = tmp_path / "tree"
    tree.mkdir()
    current = tree
    for _ in range(300):
        current = current / "n"
        current.mkdir()
    (current / "leaf.txt").write_bytes(b"x")
    manifest = snapshot(frozen_request((tree,)), tmp_path / "manifest.json")
    deepest = b"n/" * 300 + b"leaf.txt"
    assert entry_map(manifest)[deepest].sha256 == hashlib.sha256(b"x").hexdigest()
    assert len(manifest.entries) == 302
    print("PASS snapshot inventory records deep nesting without recursion limits")


def test_recorded_metadata_fields(tmp_path: Path) -> None:
    """Given a regular file, When snapshotting, Then identity, ownership, mode, link
    count, size, nanosecond mtime/ctime are recorded and atime is excluded."""
    tree = tmp_path / "tree"
    tree.mkdir()
    target = tree / "f.txt"
    target.write_bytes(b"data")
    info = os.lstat(target)
    entry = entry_map(snapshot(frozen_request((tree,)), tmp_path / "manifest.json"))[b"f.txt"]
    assert entry.ino == info.st_ino and entry.dev == info.st_dev
    assert entry.mode == info.st_mode and entry.nlink == info.st_nlink
    assert entry.uid == os.geteuid() and entry.gid == os.getegid()
    assert entry.size == 4
    assert entry.mtime_ns == info.st_mtime_ns and entry.ctime_ns == info.st_ctime_ns
    assert "atime" not in entry.jsonable()
    print("PASS snapshot inventory records compared metadata and excludes atime")


def main() -> int:
    return run_cases((
        test_every_physical_entry_inventoried,
        test_empty_directory_recorded,
        test_non_utf8_filename_round_trips,
        test_deep_nesting_recorded,
        test_recorded_metadata_fields,
    ), "snapshot-inventory-")


if __name__ == "__main__":
    raise SystemExit(main())
