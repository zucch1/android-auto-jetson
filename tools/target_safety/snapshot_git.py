# SPDX-License-Identifier: GPL-3.0-or-later
"""Supplemental read-only git evidence for discovered repositories.

The physical inventory stays authoritative; git output is supplemental.
Each discovered repository (a directory containing .git) is probed with
read-only status/ls-files commands that disable optional index writes and
fsmonitor (GIT_OPTIONAL_LOCKS=0, -c core.fsmonitor=false,
-c core.untrackedCache=false). External gitdirs and external alternate
object stores are recorded as blocked results and never traversed.
"""
from __future__ import annotations

import hashlib
import os
import stat
import subprocess
from pathlib import Path
from typing import Final

from .snapshot_models import GitRepoRecord
from .snapshot_probe import _contained

GIT_COMMAND_TIMEOUT_SECONDS: Final = 30.0
GIT_AUX_LIMIT_BYTES: Final = 65536


def _read_aux(raw: bytes) -> bytes:
    """Bounded no-follow read of a small git administrative file."""
    fd = os.open(raw, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK)
    with os.fdopen(fd, "rb") as stream:
        blob = stream.read(GIT_AUX_LIMIT_BYTES + 1)
    if len(blob) > GIT_AUX_LIMIT_BYTES:
        raise OSError("git auxiliary file exceeds bound")
    return blob


def _git_record(root_index: int, rel: bytes, repo: Path,
                roots_raw: tuple[bytes, ...]) -> GitRepoRecord:
    """Supplemental read-only git evidence; external gitdirs/alternates block."""
    git_path = repo / ".git"
    try:
        git_info = os.lstat(git_path)
    except OSError as error:
        return GitRepoRecord(root_index, rel, "error", "git_gitdir_unreadable",
                             None, None, (), (), (), ())
    git_kind: str | None = None
    gitdir_raw: bytes | None = None
    if stat.S_ISDIR(git_info.st_mode):
        git_kind = "directory"
        gitdir_raw = os.fsencode(str(git_path))
    elif stat.S_ISREG(git_info.st_mode):
        git_kind = "file"
        try:
            first = _read_aux(os.fsencode(str(git_path))).split(b"\n", 1)[0]
        except OSError as error:
            return GitRepoRecord(root_index, rel, "error", "git_gitdir_unreadable",
                                 git_kind, None, (), (), (), ())
        prefix = b"gitdir: "
        if not first.startswith(prefix):
            return GitRepoRecord(root_index, rel, "error", "git_gitdir_unparseable",
                                 git_kind, None, (), (), (), ())
        target = first[len(prefix):].strip()
        if target.startswith(b"/"):
            gitdir_raw = os.path.normpath(target)
        else:
            gitdir_raw = os.path.normpath(os.path.join(os.fsencode(str(repo)), target))
    else:
        return GitRepoRecord(root_index, rel, "error", "git_gitdir_unsupported_type",
                             None, None, (), (), (), ())
    if not _contained(gitdir_raw, roots_raw):
        return GitRepoRecord(root_index, rel, "blocked", "external_gitdir",
                             git_kind, gitdir_raw, (), (), (), ())
    alternates = os.path.join(gitdir_raw, b"objects", b"info", b"alternates")
    if os.path.lexists(alternates):
        try:
            lines = [line.strip()
                     for line in _read_aux(alternates).splitlines() if line.strip()]
        except OSError as error:
            return GitRepoRecord(root_index, rel, "error", "git_alternates_unreadable",
                                 git_kind, gitdir_raw, (), (), (), ())
        for line in lines:
            if line.startswith(b"/"):
                resolved = os.path.normpath(line)
            else:
                resolved = os.path.normpath(os.path.join(gitdir_raw, b"objects", line))
            if not _contained(resolved, roots_raw):
                return GitRepoRecord(root_index, rel, "blocked", "external_alternate",
                                     git_kind, gitdir_raw, (), (), (), ())
    env = {**os.environ, "GIT_OPTIONAL_LOCKS": "0", "LC_ALL": "C"}
    base = ("git", "-c", "core.fsmonitor=false", "-c", "core.untrackedCache=false",
            "-C", str(repo))
    outputs: dict[str, bytes] = {}
    commands: list[tuple[str, int, str]] = []
    for name, argv in (("status", ("status", "--porcelain")),
                       ("tracked", ("ls-files",)),
                       ("untracked", ("ls-files", "--others", "--exclude-standard"))):
        try:
            result = subprocess.run(base + argv, capture_output=True,
                                    timeout=GIT_COMMAND_TIMEOUT_SECONDS,
                                    stdin=subprocess.DEVNULL, env=env, check=False)
        except subprocess.TimeoutExpired:
            return GitRepoRecord(root_index, rel, "error", "git_timeout",
                                 git_kind, gitdir_raw, (), (), (),
                                 tuple(commands + [(name, 124, "")]))
        except FileNotFoundError:
            return GitRepoRecord(root_index, rel, "error", "git_unavailable",
                                 git_kind, gitdir_raw, (), (), (),
                                 tuple(commands + [(name, 127, "")]))
        outputs[name] = result.stdout
        commands.append((name, result.returncode,
                         hashlib.sha256(result.stderr).hexdigest()))
        if result.returncode != 0:
            return GitRepoRecord(root_index, rel, "error", "git_failed",
                                 git_kind, gitdir_raw, (), (), (), tuple(commands))

    def _sorted_lines(raw: bytes) -> tuple[bytes, ...]:
        return tuple(sorted(raw.splitlines()))

    return GitRepoRecord(root_index, rel, "ok", None, git_kind, gitdir_raw,
                         _sorted_lines(outputs["status"]), _sorted_lines(outputs["tracked"]),
                         _sorted_lines(outputs["untracked"]), tuple(commands))
