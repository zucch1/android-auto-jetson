# /// script
# requires-python = ">=3.11"
# dependencies = []
# ///
# Run: python3 -B tests/target_safety/snapshot_diff_fixtures.py
"""Standalone dict-manifest builders for the snapshot_diff suites; stdlib only.

Builds producer-shaped manifest dicts (key "trailer", "task_id", "start_ns",
"end_ns", per-root capabilities list) plus the documented reseal rule: the
trailer digest is SHA-256 over the canonical body with the trailer key
removed entirely (the producer's exact rule).
"""
from __future__ import annotations

import hashlib
import json
import os
import sys
import tempfile
from collections.abc import Callable
from pathlib import Path
from typing import Final

FIXED_NS: Final = 1_700_000_000_000_000_000


def canonical_digest(manifest: dict[str, object]) -> str:
    """Independent copy of the producer digest rule: body minus trailer key."""
    body = {key: value for key, value in manifest.items()
            if key not in ("completion_trailer", "trailer")}
    return hashlib.sha256(json.dumps(
        body, sort_keys=True, separators=(",", ":"), ensure_ascii=True,
    ).encode("utf-8")).hexdigest()


def make_entry(path: str = "f.txt", **overrides: object) -> dict[str, object]:
    """One producer-shaped entry; keyword overrides replace individual fields."""
    entry: dict[str, object] = {
        "root": 0,
        "path": ["u", path],
        "type": "file",
        "mode": 0o100644,
        "uid": 1000,
        "gid": 1000,
        "nlink": 1,
        "size": 5,
        "mtime_ns": FIXED_NS,
        "ctime_ns": FIXED_NS,
        "dev": 2049,
        "ino": 1000,
        "rdev": None,
        "sha256": "a" * 64,
        "link_target": None,
        "external_target": False,
        "xattrs": [["u", "user.tag"], "d" * 64],
        "inode_flags": 0,
    }
    entry.update(overrides)
    return entry


def make_manifest(*, entries: list[dict[str, object]] | None = None,
                  **overrides: object) -> dict[str, object]:
    """One producer-shaped manifest with a sealed trailer; overrides replace fields."""
    entry_list = [make_entry()] if entries is None else entries
    manifest: dict[str, object] = {
        "boot_id": "boot-fixture",
        "bytes_hashed": 5,
        "capabilities": [{"root": 0, "xattrs": "supported", "acls": "supported",
                          "inode_flags": "supported", "detail": ""}],
        "entries": entry_list,
        "entry_count": len(entry_list),
        "git": [],
        "link_exceptions": [],
        "roots": [["u", "/tmp/opencode/protected"]],
        "schema_version": "ppic/1",
        "start_ns": FIXED_NS,
        "end_ns": FIXED_NS + 1,
        "target_identity": "jetson-fixture",
        "task_id": "task-1",
        "tool_digest": "b" * 64,
        "tool_version": "ppic-snapshot/1",
        "traversal_errors": [],
    }
    manifest.update(overrides)
    if "trailer" not in manifest and "completion_trailer" not in manifest:
        reseal(manifest)
    return manifest


def reseal(manifest: dict[str, object]) -> dict[str, object]:
    """Recompute the trailer digest over the current body; mutation is the point."""
    manifest["trailer"] = {"complete": True, "manifest_digest": canonical_digest(manifest)}
    return manifest


def write_manifest(directory: Path, manifest: object,
                   name: str = "manifest.json") -> Path:
    """Retain one fixture manifest as JSON with the producer's trailing newline."""
    path = directory / name
    path.write_text(json.dumps(manifest, sort_keys=True, separators=(",", ":"),
                               ensure_ascii=True) + "\n")
    return path


def run_cases(cases: tuple[Callable[[Path], None], ...], prefix: str) -> int:
    os.makedirs("/tmp/opencode", exist_ok=True)
    for case in cases:
        case(Path(tempfile.mkdtemp(prefix=prefix, dir="/tmp/opencode")))
    return 0


def main() -> int:
    tmp = Path(tempfile.mkdtemp(prefix="snapshot-diff-fixtures-", dir="/tmp/opencode"))
    manifest = make_manifest()
    assert manifest["entry_count"] == 1
    trailer = manifest["trailer"]
    assert isinstance(trailer, dict)
    assert trailer["manifest_digest"] == canonical_digest(manifest)
    path = write_manifest(tmp, manifest)
    assert json.loads(path.read_text())["task_id"] == "task-1"
    assert canonical_digest(make_manifest(entry_count=9)) != trailer["manifest_digest"]
    print("PASS snapshot diff fixture builders seal and round-trip")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
