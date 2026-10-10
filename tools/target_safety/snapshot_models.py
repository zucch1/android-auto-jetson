# SPDX-License-Identifier: GPL-3.0-or-later
"""Frozen record types for the protected-path snapshot producer.

Every record is immutable and slots-based; serialization to canonical
JSON happens in the sibling envelope module. Compared entry fields cover
device/inode identity, type/mode, numeric UID/GID, link count, regular-file
size, nanosecond mtime and ctime, special-device rdev where relevant, and
supported persistent xattrs and inode flags. atime is excluded because
traversal itself updates it.
"""
from __future__ import annotations

import time
from collections.abc import Callable
from dataclasses import dataclass
from pathlib import Path
from typing import Final

from .kernel import WINDOW_SECONDS
from .snapshot_codec import encode_name

# Snapshot-era content-hash bound (64 GiB), replacing the watch-era
# inventory.INVENTORY_BYTE_LIMIT (8 GiB) for the snapshot-diff gate: the 2026-10-04
# target measurement (task-5-traversal-measurement-outcome.json) showed the real
# protected roots exceed 8 GiB (fail-closed at 8,590,439,522 bytes after 38.4s at
# ~224 MB/s). Hashing is streaming with O(1) memory and independently bounded by the
# traversal deadline (840s), so this bound guards against pathological hashing
# volume only. Fixed, never auto-grows; a traversal exceeding it fail-closes with
# snapshot_byte_limit and returns for review. Headroom: 7.5x observed content,
# ~306s at measured throughput vs the 840s deadline.
CONTENT_BYTE_LIMIT: Final = 68719476736


@dataclass(frozen=True, slots=True)
class LinkException:
    """Approved namespace-only link: exact path and exact literal target text."""

    path: Path
    expected_target: str


@dataclass(frozen=True, slots=True)
class SnapshotRequest:
    """Typed producer inputs; identity and clock are injectable for determinism."""

    roots: tuple[Path, ...]
    link_exceptions: tuple[LinkException, ...] = ()
    task_id: str = ""
    target_identity: str = ""
    boot_id: str | None = None
    now_ns: Callable[[], int] = time.time_ns
    deadline_seconds: float = WINDOW_SECONDS


@dataclass(frozen=True, slots=True)
class Entry:
    """One physical entry: compared metadata plus content evidence."""

    root: int
    path: bytes
    type: str
    mode: int
    uid: int
    gid: int
    nlink: int
    size: int | None
    mtime_ns: int
    ctime_ns: int
    dev: int
    ino: int
    rdev: int | None
    sha256: str | None
    link_target: bytes | None
    external_target: bool
    xattrs: tuple[tuple[bytes, str], ...] | None
    inode_flags: int | None

    def jsonable(self) -> dict[str, object]:
        return {
            "root": self.root,
            "path": encode_name(self.path),
            "type": self.type,
            "mode": self.mode,
            "uid": self.uid,
            "gid": self.gid,
            "nlink": self.nlink,
            "size": self.size,
            "mtime_ns": self.mtime_ns,
            "ctime_ns": self.ctime_ns,
            "dev": self.dev,
            "ino": self.ino,
            "rdev": self.rdev,
            "sha256": self.sha256,
            "link_target": None if self.link_target is None else encode_name(self.link_target),
            "external_target": self.external_target,
            "xattrs": None if self.xattrs is None
                      else [[encode_name(name), digest] for name, digest in self.xattrs],
            "inode_flags": self.inode_flags,
        }


@dataclass(frozen=True, slots=True)
class FsCapabilities:
    """Per-root filesystem capability probe results; never silently omitted."""

    root: int
    xattrs: str
    acls: str
    inode_flags: str
    detail: str

    def jsonable(self) -> dict[str, object]:
        return {"root": self.root, "xattrs": self.xattrs, "acls": self.acls,
                "inode_flags": self.inode_flags, "detail": self.detail}


@dataclass(frozen=True, slots=True)
class GitRepoRecord:
    """Supplemental read-only git evidence for one discovered repository."""

    root: int
    path: bytes
    result: str
    reason: str | None
    git_kind: str | None
    gitdir: bytes | None
    status: tuple[bytes, ...]
    tracked: tuple[bytes, ...]
    untracked: tuple[bytes, ...]
    commands: tuple[tuple[str, int, str], ...]

    def jsonable(self) -> dict[str, object]:
        return {
            "root": self.root,
            "path": encode_name(self.path),
            "result": self.result,
            "reason": self.reason,
            "git_kind": self.git_kind,
            "gitdir": None if self.gitdir is None else encode_name(self.gitdir),
            "status": [encode_name(line) for line in self.status],
            "tracked": [encode_name(line) for line in self.tracked],
            "untracked": [encode_name(line) for line in self.untracked],
            "commands": [list(command) for command in self.commands],
        }


@dataclass(frozen=True, slots=True)
class TraversalError:
    """Recorded evidence gap; any occurrence blocks equivalence."""

    root: int
    path: bytes
    reason: str
    detail: str

    def jsonable(self) -> dict[str, object]:
        return {"root": self.root, "path": encode_name(self.path),
                "reason": self.reason, "detail": self.detail}
