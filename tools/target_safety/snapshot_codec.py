# SPDX-License-Identifier: GPL-3.0-or-later
"""Lossless filename-byte codec and compared-identity helpers for snapshots.

Encoding scheme (manifest schema "ppic/1"): every filename-bearing value
(entry relative path, symlink target text, absolute root/exception path,
git output line) is the raw POSIX byte string of that value. Its JSON
representation is a two-element array [tag, payload] where tag "u" carries
strict-UTF-8 text when the bytes decode cleanly and tag "b" carries standard
base64 of the raw bytes otherwise. Decoding is exact in both directions and
no surrogate character ever enters the JSON text.
"""
from __future__ import annotations

import base64
import os
import stat
from collections.abc import Sequence


def encode_name(raw: bytes) -> tuple[str, str]:
    """Tag raw filename bytes as ['u', text] or ['b', base64] for lossless JSON."""
    try:
        return ("u", raw.decode("utf-8"))
    except UnicodeDecodeError:
        return ("b", base64.b64encode(raw).decode("ascii"))


def decode_name(pair: Sequence[str]) -> bytes:
    """Exact inverse of encode_name for a ['u'|'b', payload] pair."""
    tag, payload = pair
    if tag == "u":
        return payload.encode("utf-8")
    if tag == "b":
        return base64.b64decode(payload.encode("ascii"), validate=True)
    raise ValueError(f"unknown name tag: {tag!r}")


def _identity(info: os.stat_result) -> tuple[int, ...]:
    """Compared identity: atime excluded (traversal updates it), rdev included."""
    return (info.st_dev, info.st_ino, info.st_mode, info.st_uid, info.st_gid,
            info.st_nlink, info.st_size, info.st_mtime_ns, info.st_ctime_ns,
            info.st_rdev)


def _entry_type(mode: int) -> str:
    if stat.S_ISREG(mode):
        return "file"
    if stat.S_ISDIR(mode):
        return "dir"
    if stat.S_ISLNK(mode):
        return "symlink"
    if stat.S_ISFIFO(mode):
        return "fifo"
    if stat.S_ISSOCK(mode):
        return "socket"
    if stat.S_ISCHR(mode):
        return "chardev"
    if stat.S_ISBLK(mode):
        return "blockdev"
    return "other"
