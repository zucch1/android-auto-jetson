# SPDX-License-Identifier: GPL-3.0-or-later
"""Protected-path snapshot producer: lossless no-follow tree inventory for before/after diff.

This is the public entry point of the snapshot producer. It replaces
watch-coupled target-write verification with before/after snapshot-diff
evidence (owner-approved, Oracle-reviewed amendment). The physical
inventory is authoritative; git output is supplemental only. One traversal
records EVERY physical entry below each root -- .git administration,
ignored files, virtualenvs, build outputs, empty directories and nested
repositories included -- with no exclusion rules.

Path encoding scheme (manifest schema "ppic/1"): every filename-bearing
value (entry relative path, symlink target text, root/exception absolute
path, git output line) is the raw POSIX byte string of that value. Its JSON
representation is a two-element array [tag, payload] where tag "u" carries
the strict-UTF-8 text when the bytes decode cleanly and tag "b" carries
standard base64 of the raw bytes otherwise. Decoding is exact in both
directions and no surrogate character ever enters the JSON text. The codec
lives in snapshot_codec; records in snapshot_models; the manifest envelope
and equivalence diff in snapshot_envelope; capability probing and the tool
digest in snapshot_probe; capture in snapshot_capture; traversal in
snapshot_walk; the git supplement in snapshot_git.

Compared entry fields: device/inode identity, type/mode, numeric UID/GID,
link count, regular-file size, nanosecond mtime and ctime, special-device
rdev where relevant, and supported persistent xattrs (POSIX ACLs are
captured through their system.posix_acl_* xattrs) and inode flags. atime is
excluded from the record because traversal itself updates it. Filesystem
support for xattrs/ACLs/inode flags is probed per root and recorded in the
capabilities section; per-entry nulls mean "not captured here" and are
explained by that section plus these conventions: inode flags are captured
only for regular files and directories (symlinks cannot be opened without
following, and device/socket nodes are never opened by this tool), xattrs
are null only when the root probe reported the capability unsupported.

Content evidence is SHA-256 of every regular file's actual bytes (bounded
no-follow reads) and of literal symlink-target bytes; metadata never
implies an unchanged digest. Entry identity/metadata is checked before and
after hashing and every directory is rechecked after its subtree; any race
raises Blocked with a specific reason code and NO manifest is produced
(missing evidence is a failed gate). Unreadable entries and skipped
subtrees are recorded in traversal_errors and still block acceptance:
equivalence refuses any manifest carrying traversal errors, so an
unreadable entry is never equivalence.

Link exceptions list approved namespace-only links (path + expected literal
target text): they are inventoried as link objects with their literal
target bytes hashed and their referents never traversed. Any other symlink
whose textual target resolves outside the protected roots raises
Blocked("unapproved_external_link", ...). Declared exceptions must be
matched exactly once or the run blocks (fail-closed, matching the
coverage/guard policy semantics), and the same path declared twice in one
request is rejected at request validation (Blocked("link_exception_duplicate"))
so a duplicated declaration can never defeat the exactly-once rule.

The manifest envelope records schema version "ppic/1", task/window id,
target identity, boot id, tool digest (SHA-256 over all producer module
sources in deterministic filename order), roots and link exceptions, entry
count, bytes hashed (regular-file content bytes plus literal symlink-target
bytes), start/end timestamps (ns), traversal errors and an explicit
completion trailer {"complete": true, "manifest_digest": ...} where
manifest_digest is SHA-256 over the canonical serialization of everything
except the trailer. Canonical serialization is json.dumps(...,
sort_keys=True, separators=(",", ":"), ensure_ascii=True) encoded ASCII;
the retained file adds a trailing newline. The target identity is recorded
exactly as the caller provides it: nothing authenticates that string
(accepted limitation under the ratified trusted-executor threat model; it
is cross-checked for equality across a before/after pair by the consumer).
The digest is self-consistency only, never a MAC: an actor who can rewrite
a manifest can recompute its trailer digest.

The producer writes exactly one complete manifest to the caller-specified
local path (retention is mandatory; the path must live outside the
protected roots). The evidence location is checked with symlink-aware
realpath resolution of both the parent directory and the final path
component -- a manifest path reached through a symlink that resolves into a
protected root is rejected (Blocked("evidence_inside_roots")) before any
file is touched. The manifest destination is then claimed before any
traversal or evidence capture begins: any pre-existing file at the path is
removed and the exact name is reserved with O_CREAT|O_EXCL|O_NOFOLLOW, so
a Blocked run leaves at most that empty claim marker and never a stale
prior window's manifest. The complete manifest is published by writing a
same-directory temporary file and atomically renaming it over the claim, so
the final path never holds a partial manifest.

Equivalence is an exact canonical diff of the evidence body (roots, link
exceptions, capabilities, entries, entry count, bytes hashed, git
supplement) with a machine-readable zero-diff or detailed diff. Timing
headers (start_ns, end_ns), the window id (task_id) and traversal_errors
are never diffed as filesystem evidence: timestamps vary per run, the
window id is provenance, and traversal errors gate instead -- any error on
either side is "never equivalence". Provenance headers (schema, target
identity, boot id, tool digest/version) must match or the comparison
refuses.
"""
from __future__ import annotations

import os
import stat
from pathlib import Path

from .kernel import Blocked
from .snapshot_codec import _identity, decode_name, encode_name
from .snapshot_envelope import (EVIDENCE_KEYS, PROVENANCE_KEYS, SCHEMA_VERSION,
                                TOOL_VERSION, Equivalence, SnapshotManifest,
                                equivalence)
from .snapshot_git import _git_record
from .snapshot_models import (Entry, FsCapabilities, GitRepoRecord,
                              LinkException, SnapshotRequest, TraversalError)
from .snapshot_probe import (_contained, _probe_capabilities, _read_boot_id,
                             _stat_or_vanish, _tool_digest)
from .snapshot_walk import _Traversal

__all__ = [
    "EVIDENCE_KEYS", "PROVENANCE_KEYS", "SCHEMA_VERSION", "TOOL_VERSION",
    "Entry", "Equivalence", "FsCapabilities", "GitRepoRecord", "LinkException",
    "SnapshotManifest", "SnapshotRequest", "TraversalError", "decode_name",
    "encode_name", "equivalence", "snapshot",
]


def _evidence_outside_roots(manifest_path: Path, roots: tuple[Path, ...]) -> None:
    """Reject an evidence path that resolves (symlink-aware) inside any root.

    Both the parent directory and the final path component are resolved with
    os.path.realpath before the containment check, so a manifest location
    reached through a symlinked directory or a symlinked final name that lands
    in the protected tree is refused exactly like a lexically-inside path.
    """
    absolute = os.path.abspath(str(manifest_path))
    parent, name = os.path.split(absolute)
    final = os.path.realpath(os.path.join(os.path.realpath(parent), name))
    evidence_real = os.path.normpath(os.fsencode(final))
    roots_real = tuple(os.path.normpath(os.fsencode(os.path.realpath(str(root))))
                       for root in roots)
    if _contained(evidence_real, roots_real):
        raise Blocked("evidence_inside_roots", str(manifest_path))


def _admit_evidence_location(request: SnapshotRequest, manifest_path: Path) -> None:
    """Phase-1 admission (read-only): root path sanity plus evidence containment.

    Runs before any filesystem mutation at the evidence path because the
    containment proof must precede the destination claim itself.
    """
    if not request.roots:
        raise Blocked("snapshot_root_invalid", "no roots")
    for root in request.roots:
        if not root.is_absolute() or root == Path(root.anchor):
            raise Blocked("snapshot_root_invalid", str(root))
    _evidence_outside_roots(manifest_path, request.roots)


def _claim_manifest_destination(manifest_path: Path) -> None:
    """Claim the manifest destination before traversal (fail-closed freshness).

    Removes any pre-existing file at the path -- a stale manifest from an
    earlier window must never survive into this run -- and reserves the exact
    name with O_CREAT|O_EXCL|O_NOFOLLOW. A Blocked run leaves at most this
    empty claim marker: never a prior window's manifest and never a partial
    write. The complete manifest is later published by an atomic
    same-directory rename (SnapshotManifest.write).
    """
    try:
        os.unlink(manifest_path)
    except FileNotFoundError:
        pass
    except OSError as error:
        raise Blocked("evidence_write_failed", str(error)) from error
    try:
        fd = os.open(manifest_path,
                     os.O_CREAT | os.O_EXCL | os.O_WRONLY | os.O_NOFOLLOW, 0o600)
    except OSError as error:
        raise Blocked("evidence_write_failed", str(error)) from error
    os.close(fd)


def _validate_request(request: SnapshotRequest) -> tuple[bytes, ...]:
    """Phase-2 admission (read-only): real roots, no overlap, exact link exceptions.

    Runs after the destination claim so a request rejected here (or later) can
    never leave a stale prior manifest at the evidence path.
    """
    roots_raw: list[bytes] = []
    for root in request.roots:
        if not stat.S_ISDIR(_stat_or_vanish(root).st_mode):
            raise Blocked("root_not_real_directory", str(root))
        roots_raw.append(os.path.normpath(os.fsencode(str(root))))
    for index, raw in enumerate(roots_raw):
        for other in roots_raw[index + 1:]:
            if raw.startswith(other + b"/") or other.startswith(raw + b"/") \
                    or raw == other:
                raise Blocked("snapshot_root_overlap", os.fsdecode(raw))
    seen_exceptions: set[bytes] = set()
    for exc in request.link_exceptions:
        if not exc.path.is_absolute():
            raise Blocked("link_exception_outside_roots", str(exc.path))
        raw = os.path.normpath(os.fsencode(str(exc.path)))
        if raw in seen_exceptions:
            raise Blocked("link_exception_duplicate", str(exc.path))
        seen_exceptions.add(raw)
        if not _contained(raw, tuple(roots_raw)):
            raise Blocked("link_exception_outside_roots", str(exc.path))
    return tuple(roots_raw)


def snapshot(request: SnapshotRequest, manifest_path: Path) -> SnapshotManifest:
    """Produce one complete protected-path snapshot manifest at manifest_path."""
    start_ns = request.now_ns()
    _admit_evidence_location(request, manifest_path)
    _claim_manifest_destination(manifest_path)
    roots_raw = _validate_request(request)
    boot_id = request.boot_id if request.boot_id is not None else _read_boot_id()
    capabilities = tuple(_probe_capabilities(index, root)
                         for index, root in enumerate(request.roots))
    run = _Traversal(request, roots_raw, capabilities)
    run.walk()
    for path, before in reversed(run.post_stack):
        if _identity(_stat_or_vanish(path)) != before:
            raise Blocked("snapshot_directory_race", str(path))
    for exc in request.link_exceptions:
        if exc.path not in run.matched_exceptions:
            raise Blocked("link_exception_missing_or_nonlink", str(exc.path))
    git = tuple(_git_record(root_index, rel, repo, roots_raw)
                for root_index, rel, repo in sorted(run.repos))
    # Final identity recheck runs AFTER the supplemental git probes so a probe
    # side effect can never survive behind an already-sealed identity record.
    for path, before in run.recheck:
        try:
            info = os.lstat(path)
        except FileNotFoundError as error:
            raise Blocked("snapshot_entry_vanished", str(path)) from error
        if _identity(info) != before:
            raise Blocked("snapshot_changed", str(path))
        run.check()
    manifest = SnapshotManifest(
        task_id=request.task_id,
        target_identity=request.target_identity,
        boot_id=boot_id,
        tool_digest=_tool_digest(),
        start_ns=start_ns,
        end_ns=request.now_ns(),
        roots=roots_raw,
        link_exceptions=request.link_exceptions,
        capabilities=capabilities,
        entries=tuple(sorted(run.entries, key=lambda entry: (entry.root, entry.path))),
        bytes_hashed=run.bytes_hashed,
        git=git,
        traversal_errors=tuple(sorted(run.errors,
                                      key=lambda error: (error.root, error.path))),
    )
    manifest.write(manifest_path)
    return manifest
