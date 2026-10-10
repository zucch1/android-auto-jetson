# SPDX-License-Identifier: GPL-3.0-or-later
# /// script
# requires-python = ">=3.11"
# dependencies = []
# ///
# Run: python3 -B tests/target_safety/snapshot_limits.py
"""Snapshot content-hash bound: pinned value, fail-closed enforcement, and coverage
of both accumulation paths (regular-file hashing and symlink-target hashing)."""
from __future__ import annotations

import sys
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parent))
from snapshot_fixtures import frozen_request, run_cases

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
from target_safety.kernel import Blocked
from target_safety.snapshot import snapshot
from target_safety.snapshot_models import CONTENT_BYTE_LIMIT


def test_content_byte_limit_is_pinned(tmp_path: Path) -> None:
    """Given the snapshot-era bound, Then it is the measured 64 GiB policy constant and
    distinct from the untouched watch-era 8 GiB inventory bound."""
    from target_safety.inventory import INVENTORY_BYTE_LIMIT
    assert CONTENT_BYTE_LIMIT == 64 * 1024 ** 3 == 68719476736
    assert INVENTORY_BYTE_LIMIT == 8 * 1024 ** 3
    print("PASS snapshot content bound is pinned at 64 GiB, watch-era bound untouched")


def test_regular_file_hashing_blocks_at_limit(tmp_path: Path) -> None:
    """Given a patched-small content bound and a file exceeding it, When snapshotting,
    Then the producer raises Blocked(snapshot_byte_limit) and writes no manifest."""
    tree = tmp_path / "tree"
    tree.mkdir()
    (tree / "big.bin").write_bytes(b"x" * 4096)
    with patch("target_safety.snapshot_capture.CONTENT_BYTE_LIMIT", 1024), \
            patch("target_safety.snapshot_walk.CONTENT_BYTE_LIMIT", 1024):
        try:
            snapshot(frozen_request((tree,)), tmp_path / "manifest.json")
        except Blocked as error:
            assert error.args[0] == "snapshot_byte_limit", error.args
            assert not (tmp_path / "manifest.json").exists() or \
                (tmp_path / "manifest.json").read_bytes() == b""
            print("PASS regular-file hashing fail-closes at the content bound")
            return
    raise AssertionError("snapshot_byte_limit must fire for oversized content")


def test_symlink_target_hashing_blocks_at_limit(tmp_path: Path) -> None:
    """Given a patched-small content bound and an in-root symlink whose literal target
    bytes exceed it, When snapshotting, Then the producer raises
    Blocked(snapshot_byte_limit)."""
    tree = tmp_path / "tree"
    tree.mkdir()
    long_target = tree / ("d" * 2048)
    (tree / "link").symlink_to(long_target)
    with patch("target_safety.snapshot_capture.CONTENT_BYTE_LIMIT", 1024), \
            patch("target_safety.snapshot_walk.CONTENT_BYTE_LIMIT", 1024):
        try:
            snapshot(frozen_request((tree,)), tmp_path / "manifest.json")
        except Blocked as error:
            assert error.args[0] == "snapshot_byte_limit", error.args
            print("PASS symlink-target hashing fail-closes at the content bound")
            return
    raise AssertionError("snapshot_byte_limit must fire for oversized link targets")


def test_normal_tree_passes_under_real_limit(tmp_path: Path) -> None:
    """Given a small fixture tree, When snapshotting under the real 64 GiB bound,
    Then the manifest is produced normally."""
    tree = tmp_path / "tree"
    tree.mkdir()
    (tree / "keep.txt").write_bytes(b"hello\n")
    manifest_path = tmp_path / "manifest.json"
    manifest = snapshot(frozen_request((tree,)), manifest_path)
    assert manifest_path.read_bytes()
    assert manifest.bytes_hashed == 6
    print("PASS normal fixture tree passes under the real content bound")


def main() -> int:
    return run_cases((
        test_content_byte_limit_is_pinned,
        test_regular_file_hashing_blocks_at_limit,
        test_symlink_target_hashing_blocks_at_limit,
        test_normal_tree_passes_under_real_limit,
    ), "snapshot-limits-")


if __name__ == "__main__":
    raise SystemExit(main())
