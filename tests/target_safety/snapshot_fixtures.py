# SPDX-License-Identifier: GPL-3.0-or-later
# /// script
# requires-python = ">=3.11"
# dependencies = []
# ///
# Run: python3 -B tests/target_safety/snapshot_fixtures.py
"""Shared local fixture builders for the snapshot suites; stdlib only, no network."""
from __future__ import annotations

import os
import stat
import subprocess
import sys
import tempfile
from collections.abc import Callable, Iterable
from pathlib import Path
from typing import Final

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
from target_safety.snapshot import (Entry, LinkException, SnapshotManifest,
                                    SnapshotRequest, decode_name, encode_name)

FIXED_NS: Final = 1_700_000_000_000_000_000

RICH_PATHS: Final = (
    b"",
    b".git", b".git/HEAD", b".git/config", b".git/objects", b".git/objects/info",
    b".gitignore",
    b".venv", b".venv/lib", b".venv/lib/site.py",
    b"build", b"build/deep", b"build/deep/x.o",
    b"empty",
    b"ignored.log",
    b"inner-link",
    b"keep.txt",
    b"nested-repo", b"nested-repo/.git", b"nested-repo/.git/HEAD",
)


def frozen_request(roots: Iterable[Path], *,
                   link_exceptions: Iterable[LinkException] = (),
                   task_id: str = "task-1",
                   target_identity: str = "local-fixture",
                   boot_id: str = "boot-fixture",
                   deadline_seconds: float = 900.0) -> SnapshotRequest:
    """Deterministic request: fixed clock, identity and window identifiers."""
    return SnapshotRequest(
        roots=tuple(roots),
        link_exceptions=tuple(link_exceptions),
        task_id=task_id,
        target_identity=target_identity,
        boot_id=boot_id,
        now_ns=lambda: FIXED_NS,
        deadline_seconds=deadline_seconds,
    )


def build_rich_tree(root: Path) -> Path:
    """Root, .git administration, ignored file, venv, build output, empty dir, nested repo."""
    tree = root / "tree"
    (tree / ".git" / "objects" / "info").mkdir(parents=True)
    (tree / ".git" / "HEAD").write_text("ref: refs/heads/main\n")
    (tree / ".git" / "config").write_text("[core]\n")
    (tree / ".gitignore").write_text("ignored.log\n")
    (tree / "ignored.log").write_bytes(b"ignored\n")
    (tree / ".venv" / "lib").mkdir(parents=True)
    (tree / ".venv" / "lib" / "site.py").write_bytes(b"# site\n")
    (tree / "build" / "deep").mkdir(parents=True)
    (tree / "build" / "deep" / "x.o").write_bytes(b"\x00\x01")
    (tree / "empty").mkdir()
    (tree / "keep.txt").write_bytes(b"hello\n")
    (tree / "inner-link").symlink_to("keep.txt")
    (tree / "nested-repo" / ".git").mkdir(parents=True)
    (tree / "nested-repo" / ".git" / "HEAD").write_text("ref: refs/heads/main\n")
    return tree


def entry_map(manifest: SnapshotManifest) -> dict[bytes, Entry]:
    return {entry.path: entry for entry in manifest.entries}


def expected_hashed_bytes(tree: Path) -> int:
    """Regular-file content bytes plus literal symlink-target bytes."""
    total = 0
    pending = [tree]
    while pending:
        with os.scandir(pending.pop()) as scan:
            for item in scan:
                info = os.lstat(item.path)
                if stat.S_ISLNK(info.st_mode):
                    total += len(os.fsencode(os.readlink(item.path)))
                elif stat.S_ISDIR(info.st_mode):
                    pending.append(Path(item.path))
                elif stat.S_ISREG(info.st_mode):
                    total += info.st_size
    return total


def git_env(home: Path) -> dict[str, str]:
    """Hermetic git environment: no user or system configuration influence."""
    return {**os.environ,
            "GIT_CONFIG_NOSYSTEM": "1",
            "GIT_CONFIG_GLOBAL": "/dev/null",
            "GIT_CONFIG_SYSTEM": "/dev/null",
            "GIT_TERMINAL_PROMPT": "0",
            "HOME": str(home),
            "LC_ALL": "C"}


def git_init(path: Path, home: Path) -> None:
    subprocess.run(["git", "init", "-q", str(path)], check=True, capture_output=True,
                   env=git_env(home))


def run_cases(cases: tuple[Callable[[Path], None], ...], prefix: str) -> int:
    os.makedirs("/tmp/opencode", exist_ok=True)
    for case in cases:
        case(Path(tempfile.mkdtemp(prefix=prefix, dir="/tmp/opencode")))
    return 0


def main() -> int:
    tree = build_rich_tree(Path(tempfile.mkdtemp(prefix="snapshot-fixtures-", dir="/tmp/opencode")))
    assert set(os.listdir(tree)) >= {"build", "empty", "keep.txt"}
    assert encode_name(b"plain") == ("u", "plain")
    assert decode_name(encode_name(b"bad-\xff")) == b"bad-\xff"
    print("PASS snapshot fixtures builders and name codec round-trip")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
