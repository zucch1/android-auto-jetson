#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Export the raw tree of one commit into an empty directory, with NO git attribute processing.

Used by tools/ci/arm64-cross-relay.sh to materialise the UNTRUSTED PR head. `git archive`
is not safe for that: it honours the PR's own .gitattributes, so a PR can select a filter
(smudge/process drivers configured in the owner's local or global git config) or
`export-subst`, and the owner's machine then runs that command with the owner's HOME and
SSH agent. This exporter only reads objects (`git ls-tree`, `git cat-file --batch`, which
never run filters or hooks) and writes the bytes verbatim:

* regular files 100644/100755 (any other regular mode is written 0644) are written
  create-exclusive and no-follow, relative to a directory fd, so a symlink or a duplicate
  tree entry can never redirect a write outside the export root;
* symlinks (120000) are created as symlinks and never followed;
* gitlinks (160000, submodules) are skipped (an empty directory is not created);
* a path with an empty, `.`, `..` or `.git` (any case) component is rejected.

Usage: relay-export-tree.py <repo> <commit-sha> <dest-dir>   (dest must exist and be empty)
"""
import errno
import os
import subprocess
import sys

DIR_FLAGS = os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW | os.O_CLOEXEC


class ExportError(Exception):
    pass


def git_env() -> dict[str, str]:
    # Nothing of the owner's configuration is needed to read objects; keep it out regardless.
    return {'PATH': '/usr/bin:/bin', 'GIT_CONFIG_NOSYSTEM': '1', 'GIT_CONFIG_GLOBAL': '/dev/null',
            'HOME': '/nonexistent', 'GIT_TERMINAL_PROMPT': '0', 'LC_ALL': 'C',
            # refs/replace/* in the owner's repo must not rewrite the object read for the PR SHA.
            'GIT_NO_REPLACE_OBJECTS': '1'}


def parse_ls_tree(raw: bytes) -> list[tuple[bytes, bytes, bytes]]:
    entries = []
    for record in raw.split(b'\0'):
        if not record:
            continue
        meta, _, path = record.partition(b'\t')
        mode, otype, oid = meta.split(b' ')
        entries.append((mode, oid, path))
    return entries


def components(path: bytes) -> list[bytes]:
    parts = path.split(b'/')
    for part in parts:
        if part in (b'', b'.', b'..') or part.lower() == b'.git' or b'\0' in part:
            raise ExportError(f'refusing unsafe path in the tree: {path!r}')
    return parts


def open_dir(root_fd: int, parts: list[bytes]) -> int:
    """Return an fd for the directory root/parts, creating it; never follows a symlink."""
    fd = os.dup(root_fd)
    walked = b''
    try:
        for part in parts:
            walked += b'/' + part
            try:
                os.mkdir(part, 0o755, dir_fd=fd)
            except FileExistsError:
                pass
            try:
                nxt = os.open(part, DIR_FLAGS, dir_fd=fd)
            except OSError as exc:
                if exc.errno in (errno.ELOOP, errno.ENOTDIR):
                    raise ExportError(f'tree path component is not a plain directory: {walked!r}') from exc
                raise
            os.close(fd)
            fd = nxt
        return fd
    except BaseException:
        os.close(fd)
        raise


def read_blobs(repo: str, oids: list[bytes]):
    proc = subprocess.Popen(['git', '-C', repo, 'cat-file', '--batch'], stdin=subprocess.PIPE,
                            stdout=subprocess.PIPE, env=git_env(), close_fds=True)
    assert proc.stdin is not None and proc.stdout is not None
    try:
        for oid in oids:
            proc.stdin.write(oid + b'\n')
            proc.stdin.flush()
            header = proc.stdout.readline().split()
            if len(header) != 3 or header[1] != b'blob':
                raise ExportError(f'object {oid!r} is not a readable blob: {header!r}')
            size = int(header[2])
            data = proc.stdout.read(size)
            if len(data) != size or proc.stdout.read(1) != b'\n':
                raise ExportError(f'short read for object {oid!r}')
            yield data
    finally:
        proc.stdin.close()
        proc.stdout.close()
        proc.wait()


def export(repo: str, sha: str, dest: str) -> int:
    listing = subprocess.run(['git', '-C', repo, 'ls-tree', '-r', '-z', '--full-tree', sha + '^{tree}'],
                             env=git_env(), stdout=subprocess.PIPE, check=True).stdout
    entries = parse_ls_tree(listing)
    root_fd = os.open(dest, DIR_FLAGS)
    try:
        if os.listdir(root_fd):
            raise ExportError('destination directory is not empty')
        blobs = [(mode, oid, path) for mode, oid, path in entries if mode != b'160000']
        skipped = len(entries) - len(blobs)
        for (mode, oid, path), data in zip(blobs, read_blobs(repo, [oid for _, oid, _ in blobs])):
            parts = components(path)
            parent = open_dir(root_fd, parts[:-1])
            try:
                name = parts[-1]
                if mode == b'120000':
                    os.symlink(data, name, dir_fd=parent)
                else:
                    perm = 0o755 if mode == b'100755' else 0o644
                    fd = os.open(name, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW | os.O_CLOEXEC,
                                 perm, dir_fd=parent)
                    try:
                        os.fchmod(fd, perm)
                        view = memoryview(data)
                        while view:
                            view = view[os.write(fd, view):]
                    finally:
                        os.close(fd)
            finally:
                os.close(parent)
        if skipped:
            print(f'relay-export: skipped {skipped} gitlink(s) (submodules are not exported)', file=sys.stderr)
        print(f'relay-export: exported {len(blobs)} entries from {sha} without attribute processing',
              file=sys.stderr)
    finally:
        os.close(root_fd)
    return 0


def main(argv: list[str]) -> int:
    if len(argv) != 4:
        print(__doc__, file=sys.stderr)
        return 64
    try:
        return export(argv[1], argv[2], argv[3])
    except (ExportError, OSError, subprocess.CalledProcessError) as exc:
        print(f'relay-export: error: {exc}', file=sys.stderr)
        return 65


if __name__ == '__main__':
    sys.exit(main(sys.argv))
