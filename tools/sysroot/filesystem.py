# SPDX-License-Identifier: GPL-3.0-or-later
# /// script
# requires-python = ">=3.12"
# dependencies = []
# ///
# --- How to run ---
# Import tools.sysroot.filesystem; CLI: python3 -B tools/sysroot/cli.py --help
"""Descriptor-relative snapshot access: never follow parent or directory symlinks."""
from __future__ import annotations

from contextlib import ExitStack
from dataclasses import dataclass
import hashlib
import os
from pathlib import Path
import stat
from typing import BinaryIO, Final, assert_never

from .models import Code, Manifest, RegularFile, Symlink, SysrootError

DIR_FLAGS: Final = os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW


def directory(path: Path, stack: ExitStack) -> int:
    """Open every absolute ancestor without resolving symlinks."""
    absolute = path.absolute()
    if '..' in absolute.parts:
        raise SysrootError(Code.UNSAFE_PATH, str(path))
    try:
        descriptor = os.open('/', DIR_FLAGS)
        stack.callback(os.close, descriptor)
        for component in absolute.parts[1:]:
            descriptor = os.open(component, DIR_FLAGS, dir_fd=descriptor)
            stack.callback(os.close, descriptor)
        return descriptor
    except OSError as error:
        raise SysrootError(Code.UNSAFE_PATH, str(path)) from error


def parent(root: int, path: str, stack: ExitStack) -> int:
    descriptor = root
    for part in path.split('/')[:-1]:
        descriptor = os.open(part, DIR_FLAGS, dir_fd=descriptor)
        stack.callback(os.close, descriptor)
    return descriptor


def regular(root: int, path: str, stack: ExitStack) -> BinaryIO:
    descriptor = parent(root, path, stack)
    file = os.open(path.split('/')[-1], os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK,
                   dir_fd=descriptor)
    stream = stack.enter_context(os.fdopen(file, 'rb'))
    if not stat.S_ISREG(os.fstat(file).st_mode):
        raise SysrootError(Code.INVENTORY_MISMATCH, path)
    return stream


@dataclass(frozen=True, slots=True)
class Inventory:
    leaves: frozenset[str]
    directories: frozenset[str]


def inventory(root: int) -> Inventory:
    leaves: set[str] = set()
    directories: set[str] = set()

    def walk(descriptor: int, prefix: str) -> None:
        with os.scandir(descriptor) as entries:
            for entry in entries:
                path = prefix + entry.name
                mode = entry.stat(follow_symlinks=False).st_mode
                if stat.S_ISDIR(mode):
                    directories.add(path)
                    with ExitStack() as stack:
                        child = os.open(entry.name, DIR_FLAGS, dir_fd=descriptor)
                        stack.callback(os.close, child)
                        walk(child, path + '/')
                elif stat.S_ISREG(mode) or stat.S_ISLNK(mode):
                    leaves.add(path)
                else:
                    raise SysrootError(Code.INVENTORY_MISMATCH, path)
    walk(root, '')
    return Inventory(frozenset(leaves), frozenset(directories))


def validate_tree(root: int, manifest: Manifest, staged: bool = False) -> None:
    """Verify exact leaf/directory inventory, bytes and recorded link text without following links."""
    paths = {str(f.staged_path if staged else f.original_input_path[1:]) for f in manifest.files}
    dirs = {'/'.join(p.split('/')[:i]) for p in paths for i in range(1, len(p.split('/')))}
    if inventory(root) != Inventory(frozenset(paths), frozenset(dirs)):
        raise SysrootError(Code.INVENTORY_MISMATCH, 'tree')
    for entry in manifest.files:
        path = str(entry.staged_path if staged else entry.original_input_path[1:])
        with ExitStack() as stack:
            descriptor = parent(root, path, stack)
            info = os.stat(path.split('/')[-1], dir_fd=descriptor, follow_symlinks=False)
            match entry:
                case RegularFile():
                    stream = regular(root, path, stack)
                    if hashlib.file_digest(stream, 'sha256').hexdigest() != entry.sha256:
                        raise SysrootError(Code.HASH_MISMATCH, path)
                    if staged and stat.S_IMODE(info.st_mode) != 0o444:
                        raise SysrootError(Code.ARTIFACT_MISMATCH, path)
                case Symlink(link_text=link):
                    if not stat.S_ISLNK(info.st_mode) or os.readlink(
                            path.split('/')[-1], dir_fd=descriptor) != link:
                        raise SysrootError(Code.HASH_MISMATCH, path)
                case unreachable:
                    assert_never(unreachable)
    if staged:
        for path in ('', *sorted(dirs)):
            with ExitStack() as stack:
                descriptor = root if not path else parent(root, path + '/_', stack)
                if stat.S_IMODE(os.fstat(descriptor).st_mode) != 0o555:
                    raise SysrootError(Code.ARTIFACT_MISMATCH, path)
