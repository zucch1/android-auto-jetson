"""Bounded content hashing and persistent-metadata capture for snapshot entries.

Reads are no-follow and bounded: every regular file is hashed from its
actual bytes through os.open(O_RDONLY|O_NOFOLLOW|O_NONBLOCK), with the
opened inode checked against the pre-open identity; symlink targets are
hashed from their literal readlink bytes. Metadata never implies an
unchanged digest. xattrs (including the system.posix_acl_* ACLs) are
captured with follow_symlinks=False; inode flags are read through
FS_IOC_GETFLAGS. Unsupported or unreadable captures are recorded as
traversal errors instead of being silently omitted.
"""
from __future__ import annotations

import errno
import fcntl
import hashlib
import os
from array import array
from pathlib import Path
from typing import Final

from .kernel import Blocked
from .snapshot_models import CONTENT_BYTE_LIMIT, FsCapabilities, TraversalError
from .snapshot_probe import FS_IOC_GETFLAGS

READ_CHUNK_BYTES: Final = 1024 * 1024


class _CaptureMixin:
    """Content and persistent-metadata capture, mixed into the traversal run."""

    errors: list[TraversalError]
    capabilities: tuple[FsCapabilities, ...]
    bytes_hashed: int

    def check(self) -> None:
        raise NotImplementedError

    def _hash_regular(self, root_index: int, path: Path, rel: bytes,
                      before: tuple[int, ...]) -> str | None:
        try:
            fd = os.open(path, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK)
        except FileNotFoundError as error:
            raise Blocked("snapshot_entry_vanished", str(path)) from error
        except OSError as error:
            self.errors.append(TraversalError(root_index, rel, "unreadable_file",
                                              str(error)))
            return None
        with os.fdopen(fd, "rb") as stream:
            info = os.fstat(stream.fileno())
            if (info.st_dev, info.st_ino) != before[:2]:
                raise Blocked("snapshot_inode_race", str(path))
            expected = info.st_size
            hasher = hashlib.sha256()
            total = 0
            while chunk := stream.read(READ_CHUNK_BYTES):
                total += len(chunk)
                self.bytes_hashed += len(chunk)
                if self.bytes_hashed > CONTENT_BYTE_LIMIT:
                    raise Blocked("snapshot_byte_limit", str(self.bytes_hashed))
                hasher.update(chunk)
                self.check()
            if total != expected:
                raise Blocked("snapshot_truncated_read", f"{path} {total}!={expected}")
            return hasher.hexdigest()

    def _capture_xattrs(self, root_index: int, path: Path,
                        rel: bytes) -> tuple[tuple[bytes, str], ...] | None:
        caps = self.capabilities[root_index]
        if caps.xattrs != "supported":
            return None
        try:
            names = os.listxattr(path, follow_symlinks=False)
        except FileNotFoundError as error:
            raise Blocked("snapshot_entry_vanished", str(path)) from error
        except OSError as error:
            if error.errno in (errno.ENOTSUP, errno.ENOSYS, errno.EOPNOTSUPP):
                raise Blocked("snapshot_capability_mismatch", str(path)) from error
            self.errors.append(TraversalError(root_index, rel, "xattr_unreadable",
                                              str(error)))
            return ()
        captured: list[tuple[bytes, str]] = []
        for name in sorted(names, key=os.fsencode):
            try:
                value = os.getxattr(path, name, follow_symlinks=False)
            except FileNotFoundError as error:
                raise Blocked("snapshot_entry_vanished", str(path)) from error
            except OSError as error:
                if error.errno == errno.ENODATA:
                    raise Blocked("snapshot_xattr_race", f"{path} {name}") from error
                if error.errno in (errno.ENOTSUP, errno.ENOSYS, errno.EOPNOTSUPP):
                    raise Blocked("snapshot_capability_mismatch", str(path)) from error
                self.errors.append(TraversalError(root_index, rel, "xattr_unreadable",
                                                  str(error)))
                continue
            captured.append((os.fsencode(name), hashlib.sha256(value).hexdigest()))
        return tuple(captured)

    def _capture_flags(self, root_index: int, path: Path, rel: bytes,
                       kind: str) -> int | None:
        # Symlinks cannot be opened without following; device/socket nodes are
        # never opened by this tool. Their flags stay null by documented policy.
        if kind not in ("file", "dir"):
            return None
        if self.capabilities[root_index].inode_flags != "supported":
            return None
        try:
            fd = os.open(path, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK)
        except FileNotFoundError as error:
            raise Blocked("snapshot_entry_vanished", str(path)) from error
        except OSError as error:
            self.errors.append(TraversalError(root_index, rel, "inode_flags_unreadable",
                                              str(error)))
            return None
        try:
            buf = array("L", [0])
            fcntl.ioctl(fd, FS_IOC_GETFLAGS, buf, True)
            return int(buf[0])
        except OSError as error:
            if error.errno in (errno.ENOTTY, errno.EOPNOTSUPP, errno.ENOSYS):
                raise Blocked("snapshot_capability_mismatch", str(path)) from error
            self.errors.append(TraversalError(root_index, rel, "inode_flags_unreadable",
                                              str(error)))
            return None
        finally:
            os.close(fd)
