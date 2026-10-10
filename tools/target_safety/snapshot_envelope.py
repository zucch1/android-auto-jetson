# SPDX-License-Identifier: GPL-3.0-or-later
"""Canonical manifest envelope and exact equivalence for snapshot evidence.

The manifest records schema version "ppic/1", task/window id, target
identity, boot id, tool digest (SHA-256 over all producer module sources in
deterministic filename order), roots and link exceptions, entry count,
bytes hashed, start/end timestamps (ns), traversal errors and an explicit
completion trailer {"complete": true, "manifest_digest": ...} where
manifest_digest is SHA-256 over the canonical serialization of everything
except the trailer. Canonical serialization is json.dumps(...,
sort_keys=True, separators=(",", ":"), ensure_ascii=True) encoded ASCII;
the retained file adds a trailing newline. The trailer digest is
self-consistency only, NOT a MAC: it detects corruption and truncation, but
any actor who can rewrite a manifest can recompute the trailer digest and
forge any content (accepted limitation under the ratified trusted-executor
threat model; sign the canonical body if forgery resistance is ever
required). The target identity is a caller-provided string and is not
authenticated by this module.

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

import hashlib
import json
import os
import tempfile
from dataclasses import dataclass
from pathlib import Path
from typing import Final

from .kernel import Blocked
from .snapshot_codec import encode_name
from .snapshot_models import (Entry, FsCapabilities, GitRepoRecord,
                              LinkException, TraversalError)

SCHEMA_VERSION: Final = "ppic/1"
TOOL_VERSION: Final = "ppic-snapshot/1"
PROVENANCE_KEYS: Final = ("schema_version", "target_identity", "boot_id",
                          "tool_digest", "tool_version")
EVIDENCE_KEYS: Final = ("roots", "link_exceptions", "capabilities", "entries",
                        "entry_count", "bytes_hashed", "git")


@dataclass(frozen=True, slots=True)
class SnapshotManifest:
    """Complete manifest: evidence body plus explicit completion trailer."""

    task_id: str
    target_identity: str
    boot_id: str
    tool_digest: str
    start_ns: int
    end_ns: int
    roots: tuple[bytes, ...]
    link_exceptions: tuple[LinkException, ...]
    capabilities: tuple[FsCapabilities, ...]
    entries: tuple[Entry, ...]
    bytes_hashed: int
    git: tuple[GitRepoRecord, ...]
    traversal_errors: tuple[TraversalError, ...]

    def body_jsonable(self) -> dict[str, object]:
        return {
            "boot_id": self.boot_id,
            "bytes_hashed": self.bytes_hashed,
            "capabilities": [record.jsonable() for record in self.capabilities],
            "entries": [entry.jsonable() for entry in self.entries],
            "entry_count": len(self.entries),
            "git": [record.jsonable() for record in self.git],
            "link_exceptions": [
                {"path": encode_name(os.path.normpath(os.fsencode(str(exc.path)))),
                 "expected_target": encode_name(os.fsencode(exc.expected_target))}
                for exc in self.link_exceptions
            ],
            "roots": [encode_name(root) for root in self.roots],
            "schema_version": SCHEMA_VERSION,
            "start_ns": self.start_ns,
            "end_ns": self.end_ns,
            "target_identity": self.target_identity,
            "task_id": self.task_id,
            "tool_digest": self.tool_digest,
            "tool_version": TOOL_VERSION,
            "traversal_errors": [error.jsonable() for error in self.traversal_errors],
        }

    def canonical_body(self) -> bytes:
        return json.dumps(self.body_jsonable(), sort_keys=True,
                          separators=(",", ":"), ensure_ascii=True).encode()

    @property
    def manifest_digest(self) -> str:
        return hashlib.sha256(self.canonical_body()).hexdigest()

    def payload(self) -> dict[str, object]:
        trailer = {"complete": True, "manifest_digest": self.manifest_digest}
        return {**self.body_jsonable(), "trailer": trailer}

    def write(self, manifest_path: Path) -> None:
        """Retain the complete manifest atomically at the claimed local path.

        The bytes go to a same-directory temporary file (O_EXCL) which is
        fsynced and renamed over the claimed destination: the final path never
        holds a partial manifest, and a symlink at the destination is never
        followed (rename replaces the directory entry itself).
        """
        blob = json.dumps(self.payload(), sort_keys=True,
                          separators=(",", ":"), ensure_ascii=True).encode() + b"\n"
        try:
            tmp_fd, tmp_name = tempfile.mkstemp(dir=manifest_path.parent,
                                                prefix=f".{manifest_path.name}.",
                                                suffix=".tmp")
        except OSError as error:
            raise Blocked("evidence_write_failed", str(error)) from error
        tmp_path = Path(tmp_name)
        try:
            with os.fdopen(tmp_fd, "wb") as stream:
                stream.write(blob)
                stream.flush()
                os.fsync(stream.fileno())
            os.replace(tmp_path, manifest_path)
        except OSError as error:
            tmp_path.unlink(missing_ok=True)
            raise Blocked("evidence_write_failed", str(error)) from error


@dataclass(frozen=True, slots=True)
class Equivalence:
    """Exact canonical-manifest diff outcome with machine-readable detail."""

    equivalent: bool
    reason: str
    differences: tuple[str, ...]
    before_digest: str
    after_digest: str

    def jsonable(self) -> dict[str, object]:
        return {"equivalent": self.equivalent, "reason": self.reason,
                "differences": list(self.differences),
                "before_digest": self.before_digest,
                "after_digest": self.after_digest}


def _entry_fields(entry: Entry) -> tuple[str, ...]:
    return tuple(field.name for field in Entry.__dataclass_fields__.values())


def equivalence(before: SnapshotManifest, after: SnapshotManifest) -> Equivalence:
    """Exact canonical-manifest diff; timing/window headers excluded, errors gate."""
    for manifest in (before, after):
        if manifest.traversal_errors:
            details = tuple(
                f"traversal_error:{manifest.task_id}:{error.root}:"
                f"{os.fsdecode(error.path)}:{error.reason}"
                for error in manifest.traversal_errors)
            return Equivalence(False, "traversal_errors_present", details,
                               before.manifest_digest, after.manifest_digest)
    for key in PROVENANCE_KEYS:
        left = before.body_jsonable()[key]
        right = after.body_jsonable()[key]
        if left != right:
            return Equivalence(False, f"header_mismatch:{key}", (f"{key}~",),
                               before.manifest_digest, after.manifest_digest)
    differences: list[str] = []
    for key in EVIDENCE_KEYS:
        if key == "entries":
            continue
        if before.body_jsonable()[key] != after.body_jsonable()[key]:
            differences.append(f"{key}~")
    left_map = {(entry.root, entry.path): entry for entry in before.entries}
    right_map = {(entry.root, entry.path): entry for entry in after.entries}
    for key in sorted(set(left_map) | set(right_map)):
        label = f"{key[0]}:{os.fsdecode(key[1])}"
        if key not in right_map:
            differences.append(f"entries-:{label}")
        elif key not in left_map:
            differences.append(f"entries+:{label}")
        elif left_map[key] != right_map[key]:
            changed = [name for name in _entry_fields(left_map[key])
                       if getattr(left_map[key], name) != getattr(right_map[key], name)]
            differences.append(f"entries~:{label}:{','.join(sorted(changed))}")
    if differences:
        return Equivalence(False, "body_differs", tuple(sorted(differences)),
                           before.manifest_digest, after.manifest_digest)
    return Equivalence(True, "identical", (),
                       before.manifest_digest, after.manifest_digest)
