# SPDX-License-Identifier: GPL-3.0-or-later
"""Establish complete two-root topology coverage before inventory reads."""
from __future__ import annotations

import errno
import os
import stat
from dataclasses import dataclass
from pathlib import Path
from typing import Final

from .kernel import Blocked, Watch
from .resource import GUARD_RESOURCE_CHECK_MAX_TOPOLOGY_ENTRIES, checkpoint as resource_checkpoint
from .sizing_models import SqliteAbsenceWire

# Exact owner-approved namespace-only links (task-5-namespace-approval.json and
# task-5-venv-namespace-approval.json). No wildcard, sibling or family matches.
NAMESPACE_LINKS: Final = (
    Path("/home/zucchi/Desktop/infotainment-plan/.local_env/aqt-venv/bin/python3"),
    Path("/home/zucchi/Desktop/infotainment-plan/.venv/bin/python3"),
)
NAMESPACE_TEXT: Final = "/usr/bin/python3"
REPOSITORY: Final = Path("/home/zucchi/Desktop/infotainment-plan")
EXTERNAL: Final = Path("/home/zucchi/.omo/codegraph/projects/infotainment-plan-68328f6064505b10")
# Exact owner-approved watched-absence contract (task-5-sqlite-absence-owner-approval.json).
# This exact link, this exact literal text, this exact absent target only; a
# separate rule, never a NAMESPACE_LINKS entry and never a family exception.
SQLITE_LINK: Final = Path("/home/zucchi/Desktop/infotainment-plan/.local_env/navigation/tools/"
                          "valhalla-deps/usr/include/spatialite/sqlite3.h")
SQLITE_LINK_TEXT: Final = "../sqlite3.h"
SQLITE_TARGET: Final = Path("/home/zucchi/Desktop/infotainment-plan/.local_env/navigation/tools/"
                            "valhalla-deps/usr/include/sqlite3.h")
# Fixed policy from Oracle ses_efcd07e79ffevkpmjv0yGGmc0r; never auto-grows.
# Exact roots add at most seven ancestor watches; no target-fit claim.
PROTECTED_ENTRY_LIMIT: Final = 1000000


@dataclass(frozen=True, slots=True)
class Scope:
    repository: Path
    external: Path
    approved_link: Path
    namespace_only: bool = False

    @property
    def roots(self) -> tuple[Path, Path]:
        return self.repository, self.external


@dataclass(frozen=True, slots=True)
class Coverage:
    """Walk result: paths plus metadata already collected during setup rechecks."""

    paths: tuple[Path, ...]
    directories: int
    regular_files: int
    symlinks: int
    regular_file_bytes: int
    path_bytes: int
    discovered: int
    processed: int
    pending: int
    sqlite_absence: SqliteAbsenceWire | None = None


def identity(path: Path) -> tuple[int, ...]:
    """Exclude access time, which read-only observations can update."""
    info = path.lstat()
    return (info.st_dev, info.st_ino, info.st_mode, info.st_uid, info.st_gid,
            info.st_nlink, info.st_size, info.st_mtime_ns, info.st_ctime_ns)


def ancestors(watch: Watch, root: Path) -> None:
    """Top-down nonrecursive name filters protect every pathname component."""
    current = Path(root.anchor)
    for name in root.parts[1:]:
        if not stat.S_ISDIR(current.lstat().st_mode):
            raise Blocked("ancestor_not_real_directory", str(current))
        before = identity(current)
        watch.add(current, name)
        if identity(current) != before:
            raise Blocked("ancestor_setup_race", str(current))
        current /= name
        watch.check()


def confined_target(path: Path, scope: Scope) -> Path | None:
    """Normalize link text without following unapproved external referents."""
    text = os.readlink(path)
    if scope.namespace_only and path in NAMESPACE_LINKS:
        if text != NAMESPACE_TEXT:
            raise Blocked("namespace_link_text_mismatch", f"{path} -> {text}")
        return None
    target = Path(os.path.abspath(path.parent / text))
    if path == scope.approved_link:
        if target != scope.external:
            raise Blocked("approved_link_target_mismatch", str(target))
    elif not any(path.is_relative_to(root) and target.is_relative_to(root) for root in scope.roots):
        raise Blocked("unapproved_external_link", f"{path} -> {text}")
    return target


def production_root_spellings() -> tuple[str, str]:
    """Ordered production root spellings for wire comparison (patchable)."""
    return str(REPOSITORY), str(EXTERNAL)


def _parent_proof(scope: Scope, parent: Path, seen: dict[Path, tuple[int, ...]] | None) -> None:
    """Root through one parent: covered real directories with stable identity."""
    root = scope.repository
    if parent == root or not parent.is_relative_to(root):
        raise Blocked("sqlite_parent_outside_root", str(parent))
    component = parent
    while True:
        saved = seen.get(component) if seen is not None else None
        if seen is not None and saved is None:
            raise Blocked("sqlite_parent_uncovered", str(component))
        try:
            current = identity(component)
        except OSError:
            raise Blocked("sqlite_parent_missing", str(component)) from None
        recorded = saved if saved is not None else current
        if not stat.S_ISDIR(recorded[2]) or not stat.S_ISDIR(current[2]):
            raise Blocked("sqlite_parent_not_real_directory", str(component))
        if saved is not None and current != saved:
            raise Blocked("sqlite_parent_identity_changed", str(component))
        if component == root:
            return
        component = component.parent


def sqlite_absence_check(watch: Watch, scope: Scope,
                         seen: dict[Path, tuple[int, ...]] | None) -> SqliteAbsenceWire:
    """Exact watched-absence observation: parents, literal text, strict ENOENT."""
    link, target = SQLITE_LINK, SQLITE_TARGET
    info = (identity(link) if link.is_symlink() else None) if seen is None else seen.get(link)
    if info is None or not stat.S_ISLNK(info[2]):
        raise Blocked("sqlite_link_missing_or_nonlink", str(link))
    for parent in (link.parent, target.parent):
        _parent_proof(scope, parent, seen)
    text = os.readlink(link)
    if text != SQLITE_LINK_TEXT:
        raise Blocked("sqlite_link_text_mismatch", f"{link} -> {text}")
    normalized = Path(os.path.abspath(link.parent / text))
    if normalized != target or not target.is_relative_to(scope.repository):
        raise Blocked("sqlite_target_mismatch", f"{link} -> {text} => {normalized}")
    try:
        os.lstat(target)
    except FileNotFoundError as error:
        if error.errno != errno.ENOENT:
            raise Blocked("sqlite_target_absence_unproven", f"{target} errno={error.errno}") from None
    except OSError as error:
        raise Blocked("sqlite_target_absence_unproven", f"{target} {type(error).__name__}") from None
    else:
        raise Blocked("sqlite_target_present", str(target))
    watched = set(watch.paths.values())
    for path in (link, link.parent, target.parent):
        if str(path) not in watched:
            raise Blocked("sqlite_object_unwatched", str(path))
    return SqliteAbsenceWire(link=str(link), literal_link_text=text, target=str(target),
                             link_parent=str(link.parent), target_parent=str(target.parent),
                             enoent_observed=True, link_watched=True,
                             link_parent_watched=True, target_parent_watched=True)


def sqlite_absence_recheck(watch: Watch, scope: Scope, record: object) -> None:
    """Final-boundary recheck: the retained record must reproduce before close."""
    if sqlite_absence_check(watch, scope, None) != record:
        raise Blocked("sqlite_absence_changed", str(SQLITE_LINK))
    watch.check()


def sqlite_absence_wire_ok(value: object) -> SqliteAbsenceWire | None:
    """Return the record iff every field exactly matches the fixed policy."""
    expected = SqliteAbsenceWire(
        link=str(SQLITE_LINK), literal_link_text=SQLITE_LINK_TEXT, target=str(SQLITE_TARGET),
        link_parent=str(SQLITE_LINK.parent), target_parent=str(SQLITE_TARGET.parent),
        enoent_observed=True, link_watched=True, link_parent_watched=True,
        target_parent_watched=True)
    if not isinstance(value, dict) or set(value) != set(expected):
        return None
    for key in ("link", "literal_link_text", "target", "link_parent", "target_parent"):
        if type(value[key]) is not str or value[key] != expected[key]:
            return None
    for key in ("enoent_observed", "link_watched", "link_parent_watched", "target_parent_watched"):
        if value[key] is not True:
            return None
    return expected


def protect(watch: Watch, scope: Scope) -> Coverage:
    """Watch link objects, all directories/files and replacement-sensitive entries."""
    for root in scope.roots:
        if not root.is_absolute() or root == Path(root.anchor):
            raise Blocked("invalid_root", str(root))
        ancestors(watch, root)
        if not stat.S_ISDIR(root.lstat().st_mode):
            raise Blocked("root_not_real_directory", str(root))
    pending = list(scope.roots)
    seen: dict[Path, tuple[int, ...]] = {}
    links: list[tuple[Path, Path | None]] = []
    while pending:
        path = pending.pop()
        watch.check()
        before = identity(path)
        mode = before[2]
        if not (stat.S_ISDIR(mode) or stat.S_ISREG(mode) or stat.S_ISLNK(mode)):
            raise Blocked("unsupported_protected_object", str(path))
        watch.add(path)
        if identity(path) != before:
            raise Blocked("object_setup_race", str(path))
        seen[path] = before
        if len(seen) % GUARD_RESOURCE_CHECK_MAX_TOPOLOGY_ENTRIES == 0:
            resource_checkpoint()
        if stat.S_ISDIR(mode):
            with os.scandir(path) as entries:
                for entry in entries:
                    pending.append(Path(entry.path))
                    if len(seen) + len(pending) > PROTECTED_ENTRY_LIMIT:
                        raise Blocked("entry_limit", str(path))
        if stat.S_ISLNK(mode):
            links.append((path, confined_target(path, scope)))
        if len(seen) + len(pending) > PROTECTED_ENTRY_LIMIT:
            raise Blocked("entry_limit", str(len(seen)))
    # The exact watched-absence contract is validated before any chain
    # exemption is granted; only the validated origin link is exempt below.
    active_sqlite = scope.namespace_only and scope.repository == REPOSITORY
    record = None
    if active_sqlite:
        watch.check()
        record = sqlite_absence_check(watch, scope, seen)
    # Every intermediate link is itself covered and confined; never resolve()
    # through unknown paths, including external links hidden behind internal ones.
    for link, target in links:
        if target is None:
            continue
        if active_sqlite and link == SQLITE_LINK:
            continue
        hops = 0
        while target not in seen or stat.S_ISLNK(seen[target][2]):
            hops += 1
            if hops > 40:
                raise Blocked("link_cycle_or_dangling", str(link))
            prefix = next((part for part in reversed((target, *target.parents))
                           if part in seen and stat.S_ISLNK(seen[part][2])), None)
            if prefix is None:
                raise Blocked("uncovered_link_target", f"{link} -> {target}")
            if active_sqlite and prefix == SQLITE_LINK:
                raise Blocked("sqlite_alias_not_exempt", f"{link} -> {target}")
            following = confined_target(prefix, scope)
            if following is None:
                if target != prefix:
                    raise Blocked("namespace_link_not_directory", str(target))
                break
            target = following / target.relative_to(prefix)
    if scope.approved_link not in seen or not stat.S_ISLNK(seen[scope.approved_link][2]):
        raise Blocked("approved_link_missing", str(scope.approved_link))
    if scope.namespace_only:
        for link in NAMESPACE_LINKS:
            if link not in seen or not stat.S_ISLNK(seen[link][2]):
                raise Blocked("namespace_link_missing_or_nonlink", str(link))
    watch.check()
    for index, (path, before) in enumerate(seen.items()):
        if identity(path) != before:
            raise Blocked("coverage_setup_changed", str(path))
        watch.check()
        if (index + 1) % GUARD_RESOURCE_CHECK_MAX_TOPOLOGY_ENTRIES == 0:
            resource_checkpoint()
    paths = tuple(sorted(seen))
    regular = [info for info in seen.values() if stat.S_ISREG(info[2])]
    return Coverage(
        paths=paths,
        directories=sum(stat.S_ISDIR(info[2]) for info in seen.values()),
        regular_files=len(regular),
        symlinks=sum(stat.S_ISLNK(info[2]) for info in seen.values()),
        regular_file_bytes=sum(info[6] for info in regular),
        path_bytes=sum(len(os.fsencode(str(path))) for path in paths),
        discovered=len(seen) + len(pending),
        processed=len(seen),
        pending=len(pending),
        sqlite_absence=record,
    )
