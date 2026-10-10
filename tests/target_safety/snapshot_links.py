# SPDX-License-Identifier: GPL-3.0-or-later
# /// script
# requires-python = ">=3.11"
# dependencies = []
# ///
# Run: python3 -B tests/target_safety/snapshot_links.py
"""Symlink policy: in-root links, approved exceptions, unapproved external blocks."""
from __future__ import annotations

import hashlib
import json
import os
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from snapshot_fixtures import entry_map, frozen_request, run_cases

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
from target_safety.kernel import Blocked
from target_safety.snapshot import LinkException, snapshot


def test_in_root_symlink_recorded_as_link_object(tmp_path: Path) -> None:
    """Given a symlink whose target stays inside the root, When snapshotting, Then the
    link is a link object hashing its literal target text and the referent keeps its
    own entry, proving the link is never followed."""
    tree = tmp_path / "tree"
    tree.mkdir()
    (tree / "keep.txt").write_bytes(b"hello\n")
    (tree / "soft").symlink_to("keep.txt")
    manifest = snapshot(frozen_request((tree,)), tmp_path / "manifest.json")
    soft = entry_map(manifest)[b"soft"]
    keep = entry_map(manifest)[b"keep.txt"]
    assert soft.type == "symlink" and soft.link_target == b"keep.txt"
    assert soft.sha256 == hashlib.sha256(b"keep.txt").hexdigest()
    assert soft.sha256 != keep.sha256
    assert soft.external_target is False and soft.size is None
    assert keep.type == "file" and keep.sha256 == hashlib.sha256(b"hello\n").hexdigest()
    print("PASS snapshot in-root symlink is a link object and is never followed")


def test_approved_exception_link_not_traversed(tmp_path: Path) -> None:
    """Given a namespace-only link with a matching approved exception, When snapshotting,
    Then its literal target bytes are hashed, the referent is not traversed, and the
    exception is recorded in the manifest envelope."""
    tree = tmp_path / "tree"
    (tree / ".venv" / "bin").mkdir(parents=True)
    link = tree / ".venv" / "bin" / "python3"
    link.symlink_to("/usr/bin/python3")
    exception = LinkException(path=link, expected_target="/usr/bin/python3")
    manifest = snapshot(frozen_request((tree,), link_exceptions=(exception,)),
                        tmp_path / "manifest.json")
    entry = entry_map(manifest)[b".venv/bin/python3"]
    assert entry.type == "symlink" and entry.external_target is True
    assert entry.link_target == b"/usr/bin/python3"
    assert entry.sha256 == hashlib.sha256(b"/usr/bin/python3").hexdigest()
    assert all(entry.path.startswith(b".venv") or entry.path == b""
               for entry in manifest.entries)
    assert len(manifest.entries) == 4
    recorded = json.loads((tmp_path / "manifest.json").read_text())["link_exceptions"]
    assert recorded == [{"path": ["u", str(link)], "expected_target": ["u", "/usr/bin/python3"]}]
    print("PASS snapshot approved exception link hashes target bytes and skips referent")


def test_unapproved_external_link_blocks(tmp_path: Path) -> None:
    """Given an external link with no approved exception, When snapshotting, Then the
    run raises Blocked('unapproved_external_link') and writes no manifest."""
    tree = tmp_path / "tree"
    tree.mkdir()
    (tree / "evil").symlink_to("/etc/passwd")
    out = tmp_path / "manifest.json"
    try:
        snapshot(frozen_request((tree,)), out)
    except Blocked as error:
        assert error.reason == "unapproved_external_link", str(error)
        assert not out.exists() or out.read_bytes() == b"", \
                "a Blocked run must leave no manifest (empty claim marker at most)"
        print(f"PASS snapshot unapproved external link blocks: {error}")
        return
    raise AssertionError("unapproved external link accepted")


def test_exception_text_mismatch_blocks(tmp_path: Path) -> None:
    """Given an approved path whose literal link text differs from the expected text,
    When snapshotting, Then the run raises Blocked('link_exception_text_mismatch')."""
    tree = tmp_path / "tree"
    tree.mkdir()
    link = tree / "python3"
    link.symlink_to("/usr/bin/python3")
    exception = LinkException(path=link, expected_target="/bin/sh")
    try:
        snapshot(frozen_request((tree,), link_exceptions=(exception,)),
                 tmp_path / "manifest.json")
    except Blocked as error:
        assert error.reason == "link_exception_text_mismatch", str(error)
        print(f"PASS snapshot exception text mismatch blocks: {error}")
        return
    raise AssertionError("mismatched exception text accepted")


def test_declared_exception_missing_blocks(tmp_path: Path) -> None:
    """Given a declared link exception with no matching link in the tree, When snapshotting,
    Then the run raises Blocked('link_exception_missing_or_nonlink')."""
    tree = tmp_path / "tree"
    tree.mkdir()
    exception = LinkException(path=tree / "absent", expected_target="/usr/bin/python3")
    try:
        snapshot(frozen_request((tree,), link_exceptions=(exception,)),
                 tmp_path / "manifest.json")
    except Blocked as error:
        assert error.reason == "link_exception_missing_or_nonlink", str(error)
        print(f"PASS snapshot missing declared exception blocks: {error}")
        return
    raise AssertionError("missing declared exception accepted")


def main() -> int:
    return run_cases((
        test_in_root_symlink_recorded_as_link_object,
        test_approved_exception_link_not_traversed,
        test_unapproved_external_link_blocks,
        test_exception_text_mismatch_blocks,
        test_declared_exception_missing_blocks,
    ), "snapshot-links-")


if __name__ == "__main__":
    raise SystemExit(main())
