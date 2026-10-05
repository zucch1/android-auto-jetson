"""Typed wire and record models shared by sizing evidence validation.

Pure data definitions with no sibling imports: the strict sizing child
transcript, its compact outcome wire and the window restoration constants live
here so report and sizing_validation both consume one typed source without an
import cycle and without a projection ever dropping a validated field.
"""
from __future__ import annotations

from dataclasses import dataclass
from typing import Final, TypedDict

CEILING_MODE: Final = "ceiling"
SIZING_MODE: Final = "topology-sizing"
CEILING_OUTCOME_EVENT: Final = "ceiling_outcome"
SIZING_OUTCOME_EVENT: Final = "sizing_outcome"
SIZING_READY_EVENT: Final = "child_limit_ready"
SIZING_COMPLETION_EVENT: Final = "topology_sizing_complete"
SIZING_CLEANUP_EVENT: Final = "watch_cleanup"
SIZING_BLOCKED_EVENT: Final = "guard_blocked"
ORIGINAL_VALUE: Final = 65536
TEMPORARY_VALUE: Final = 1048576


class Kinds(TypedDict):
    directories: int
    regular_files: int
    symlinks: int


class SqliteAbsenceWire(TypedDict):
    """Exact owner-approved watched-absence record; nullable on the wire."""

    link: str
    literal_link_text: str
    target: str
    link_parent: str
    target_parent: str
    enoent_observed: bool
    link_watched: bool
    link_parent_watched: bool
    target_parent_watched: bool


class CleanupWire(TypedDict):
    watch_closed: bool
    fd_closed: bool
    descriptor_count: int


class SizingWire(TypedDict):
    """Compact sizing counters; mode and cleanup stay verifiable downstream."""

    mode: str
    discovered: int
    processed: int
    pending: int
    kinds: Kinds
    watch_descriptors: int
    path_bytes: int
    regular_file_bytes: int
    cleanup: CleanupWire
    inventory_complete: bool
    acquisition: bool
    task5_completed: bool
    roots: list[str]
    sqlite_absence: SqliteAbsenceWire | None


class ResourceWire(TypedDict):
    min_mem_available_bytes: int | None
    max_guard_rss_bytes: int | None
    max_supervisor_rss_bytes: int | None
    samples: int


class CollectionWire(TypedDict):
    combined_bytes: int
    records: int
    stdout_sha256: str
    stderr_sha256: str
    truncated: bool
    invalidated: bool
    first_cause: str | None


@dataclass(frozen=True, slots=True)
class GuardEvent:
    """One parsed guard stdout event (structured JSON line, not prose)."""

    event: str
    raw: str


@dataclass(frozen=True, slots=True)
class SizingChild:
    """Strict parse result for one sizing guard child stdout transcript."""

    valid: bool
    reason: str | None
    events: tuple[GuardEvent, ...]
    completion: SizingWire | None
