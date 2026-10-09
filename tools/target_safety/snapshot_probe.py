"""Per-root capability probe and small filesystem helpers for snapshots.

Filesystem support for xattrs/ACLs/inode flags is probed once per root and
recorded rather than silently omitted. The tool digest is SHA-256 over every
producer module source in deterministic filename order (each source's name
followed by its bytes), so a manifest binds to the exact tool revision.
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
from .snapshot_models import FsCapabilities

FS_IOC_GETFLAGS: Final = 0x80086601


def _stat_or_vanish(path: Path) -> os.stat_result:
    try:
        return os.lstat(path)
    except FileNotFoundError as error:
        raise Blocked("snapshot_entry_vanished", str(path)) from error
    except OSError as error:
        raise Blocked("snapshot_entry_unreadable", f"{path} {error}") from error


def _contained(raw: bytes, roots_raw: tuple[bytes, ...]) -> bool:
    return any(raw == root or raw.startswith(root + b"/") for root in roots_raw)


def _probe_capabilities(root_index: int, root: Path) -> FsCapabilities:
    """Probe xattr/ACL/inode-flag support on the root's filesystem."""
    notes: list[str] = []
    try:
        os.listxattr(root)
        xattrs = "supported"
    except OSError as error:
        xattrs = "unsupported"
        notes.append(f"xattrs:{errno.errorcode.get(error.errno, error.errno)}")
    if xattrs == "unsupported":
        acls = "unsupported"
        notes.append("acls:via-xattrs")
    else:
        try:
            os.getxattr(root, "system.posix_acl_access")
            acls = "supported"
        except OSError as error:
            if error.errno == errno.ENODATA:
                acls = "supported"
            else:
                acls = "unsupported"
                notes.append(f"acls:{errno.errorcode.get(error.errno, error.errno)}")
    try:
        fd = os.open(root, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK)
        try:
            fcntl.ioctl(fd, FS_IOC_GETFLAGS, array("L", [0]), True)
            inode_flags = "supported"
        finally:
            os.close(fd)
    except OSError as error:
        inode_flags = "unsupported"
        notes.append(f"inode_flags:{errno.errorcode.get(error.errno, error.errno)}")
    return FsCapabilities(root=root_index, xattrs=xattrs, acls=acls,
                          inode_flags=inode_flags,
                          detail="|".join(notes) if notes else "clean")


def _read_boot_id() -> str:
    try:
        with open("/proc/sys/kernel/random/boot_id", "rb") as stream:
            return stream.read().decode("ascii").strip()
    except (OSError, UnicodeDecodeError) as error:
        raise Blocked("snapshot_boot_id_unreadable", str(error)) from error


def _tool_digest() -> str:
    """SHA-256 over all producer module sources in deterministic filename order."""
    hasher = hashlib.sha256()
    for path in sorted(Path(__file__).parent.glob("snapshot*.py")):
        hasher.update(path.name.encode())
        hasher.update(b"\0")
        hasher.update(path.read_bytes())
        hasher.update(b"\0")
    return hasher.hexdigest()
