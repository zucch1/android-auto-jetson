"""No-follow iterative traversal producing the physical snapshot inventory.

The walk records every physical entry below each root -- the root itself,
.git administration, ignored files, virtualenvs, build outputs, empty
directories and nested repositories included -- with no exclusion rules.
Identity and metadata are checked before and after capture, directory
membership is bracketed around os.scandir and rechecked after each
subtree, and any race raises Blocked with a specific reason code so no
manifest is produced from unstable evidence.
"""
from __future__ import annotations

import hashlib
import os
import stat
import time
from pathlib import Path

from .kernel import Blocked
from .snapshot_capture import _CaptureMixin
from .snapshot_codec import _entry_type, _identity
from .snapshot_models import (CONTENT_BYTE_LIMIT, Entry, FsCapabilities,
                              SnapshotRequest, TraversalError)
from .snapshot_probe import _contained, _stat_or_vanish


class _Traversal(_CaptureMixin):
    """One no-follow traversal run collecting entries under a monotonic deadline."""

    def __init__(self, request: SnapshotRequest,
                 roots_raw: tuple[bytes, ...],
                 capabilities: tuple[FsCapabilities, ...]) -> None:
        self.request = request
        self.roots_raw = roots_raw
        self.capabilities = capabilities
        self.deadline = time.monotonic() + request.deadline_seconds
        self.entries: list[Entry] = []
        self.errors: list[TraversalError] = []
        self.repos: list[tuple[int, bytes, Path]] = []
        self.recheck: list[tuple[Path, tuple[int, ...]]] = []
        self.post_stack: list[tuple[Path, tuple[int, ...]]] = []
        self.matched_exceptions: set[Path] = set()
        self.bytes_hashed = 0

    def check(self) -> None:
        if time.monotonic() > self.deadline:
            raise Blocked("snapshot_timeout", str(self.request.deadline_seconds))

    def walk(self) -> None:
        """Iterative depth-first walk; every physical entry is recorded, none skipped."""
        for root_index, root in enumerate(self.request.roots):
            pending: list[tuple[Path, bytes]] = [(root, b"")]
            while pending:
                dir_path, rel = pending.pop()
                self._process_directory(root_index, dir_path, rel, pending)

    def _process_directory(self, root_index: int, dir_path: Path, rel: bytes,
                           pending: list[tuple[Path, bytes]]) -> None:
        self.check()
        before = _identity(_stat_or_vanish(dir_path))
        children: list[tuple[bytes, str]] = []
        try:
            with os.scandir(dir_path) as scan:
                for item in scan:
                    children.append((os.fsencode(item.name), item.name))
        except FileNotFoundError as error:
            raise Blocked("snapshot_entry_vanished", str(dir_path)) from error
        except OSError as error:
            self.errors.append(TraversalError(root_index, rel, "unreadable_directory",
                                              str(error)))
            self._record_entry(root_index, dir_path, rel, before)
            return
        children.sort()
        if any(name_raw == b".git" for name_raw, _ in children):
            self.repos.append((root_index, rel, dir_path))
        if _identity(_stat_or_vanish(dir_path)) != before:
            raise Blocked("snapshot_directory_race", str(dir_path))
        self._record_entry(root_index, dir_path, rel, before)
        # Post-order marker: recheck this directory after its whole subtree.
        self.post_stack.append((dir_path, before))
        for name_raw, name_str in reversed(children):
            child_rel = name_raw if rel == b"" else rel + b"/" + name_raw
            self._process_child(root_index, dir_path / name_str, child_rel, pending)

    def _record_entry(self, root_index: int, path: Path, rel: bytes,
                      before: tuple[int, ...]) -> None:
        self.entries.append(self._entry_from(root_index, path, rel, before))
        self.recheck.append((path, before))

    def _process_child(self, root_index: int, path: Path, rel: bytes,
                       pending: list[tuple[Path, bytes]]) -> None:
        self.check()
        info = _stat_or_vanish(path)
        if stat.S_ISDIR(info.st_mode):
            pending.append((path, rel))
            return
        self._record_entry(root_index, path, rel, _identity(info))

    def _entry_from(self, root_index: int, path: Path, rel: bytes,
                    before: tuple[int, ...]) -> Entry:
        mode = before[2]
        kind = _entry_type(mode)
        digest: str | None = None
        link_target: bytes | None = None
        external = False
        if kind == "file":
            digest = self._hash_regular(root_index, path, rel, before)
        elif kind == "symlink":
            try:
                text = os.readlink(path)
            except FileNotFoundError as error:
                raise Blocked("snapshot_entry_vanished", str(path)) from error
            except OSError as error:
                raise Blocked("snapshot_entry_unreadable", f"{path} {error}") from error
            link_target = os.fsencode(text)
            digest = hashlib.sha256(link_target).hexdigest()
            self.bytes_hashed += len(link_target)
            if self.bytes_hashed > CONTENT_BYTE_LIMIT:
                raise Blocked("snapshot_byte_limit", str(self.bytes_hashed))
            external = self._classify_link(root_index, path, rel, link_target)
        xattrs = self._capture_xattrs(root_index, path, rel)
        inode_flags = self._capture_flags(root_index, path, rel, kind)
        if _identity(_stat_or_vanish(path)) != before:
            raise Blocked("snapshot_changed", str(path))
        return Entry(
            root=root_index,
            path=rel,
            type=kind,
            mode=mode,
            uid=before[3],
            gid=before[4],
            nlink=before[5],
            size=before[6] if kind == "file" else None,
            mtime_ns=before[7],
            ctime_ns=before[8],
            dev=before[0],
            ino=before[1],
            rdev=before[9] if kind in ("chardev", "blockdev") else None,
            sha256=digest,
            link_target=link_target,
            external_target=external,
            xattrs=xattrs,
            inode_flags=inode_flags,
        )

    def _classify_link(self, root_index: int, path: Path, rel: bytes,
                       target: bytes) -> bool:
        """True when the link target is an approved external exception."""
        resolved = os.path.normpath(os.path.join(os.fsencode(str(path.parent)), target))
        if _contained(resolved, self.roots_raw):
            return False
        raw_path = os.path.normpath(os.fsencode(str(path)))
        for exc in self.request.link_exceptions:
            if os.path.normpath(os.fsencode(str(exc.path))) != raw_path:
                continue
            if os.fsencode(exc.expected_target) != target:
                raise Blocked("link_exception_text_mismatch",
                              f"{path} -> {os.fsdecode(target)}")
            self.matched_exceptions.add(exc.path)
            return True
        raise Blocked("unapproved_external_link",
                      f"{path} -> {os.fsdecode(target)}")
