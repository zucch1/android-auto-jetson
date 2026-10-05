"""Protected metadata/content inventories with bounded no-follow reads."""
from __future__ import annotations

import hashlib
import json
import os
import stat
from dataclasses import dataclass
from pathlib import Path
from typing import Final

from .coverage import identity
from .kernel import Blocked, Watch
from .resource import checkpoint as resource_checkpoint

# Fixed bounded envelope from Oracle consultation bg_72e527f3; bytes hashed per
# inventory (read bytes, not RSS). Never auto-grows.
INVENTORY_BYTE_LIMIT: Final = 8589934592


@dataclass(frozen=True, slots=True)
class Row:
    path: str
    metadata: tuple[int, ...]
    sha256: str


def inventory(watch: Watch, paths: tuple[Path, ...]) -> tuple[Row, ...]:
    """Hash covered objects only; reject races, special objects and byte excess."""
    rows: list[Row] = []
    total = 0
    for path in paths:
        watch.check()
        before = identity(path)
        digest = ""
        if stat.S_ISREG(before[2]):
            fd = os.open(path, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK)
            with os.fdopen(fd, "rb") as stream:
                info = os.fstat(stream.fileno())
                if (info.st_dev, info.st_ino) != before[:2]:
                    raise Blocked("inventory_inode_race", str(path))
                hasher = hashlib.sha256()
                while chunk := stream.read(1024 * 1024):
                    total += len(chunk)
                    if total > INVENTORY_BYTE_LIMIT:
                        raise Blocked("inventory_byte_limit", str(total))
                    hasher.update(chunk)
                    watch.check()
                    resource_checkpoint()
                digest = hasher.hexdigest()
        elif stat.S_ISLNK(before[2]):
            digest = hashlib.sha256(os.fsencode(os.readlink(path))).hexdigest()
        if identity(path) != before:
            raise Blocked("inventory_changed", str(path))
        rows.append(Row(str(path), before, digest))
        watch.check()
    return tuple(rows)


def inventory_digest(rows: tuple[Row, ...]) -> str:
    """Stable receipt digest excludes observation-only access times."""
    payload = [(row.path, row.metadata, row.sha256) for row in rows]
    return hashlib.sha256(json.dumps(payload, separators=(",", ":")).encode()).hexdigest()
