"""Strict topology-sizing evidence validation: child transcript and wire sections.

Every nonblank child stdout line must be a well-formed typed record of the
known sizing vocabulary; malformed, truncated, over-limit, duplicated and
completion+blocked transcripts are rejected with a reason, never silently
dropped and never first-of-duplicate. The compact projection retains mode and
cleanup so the final acceptance can re-verify exactly what the strict parser
validated. The hardened diagnostic records (entry, metadata, stage brackets)
extend the vocabulary; once any appears the discipline is required too:
LF-terminated records, entry first, metadata second, contiguous sequence
numbers, exactly one balanced pair per expected stage (hardened_stream_ok).
Truncated/unterminated records mark the stream incomplete, evidence preserved.
Legacy ceiling guard-event parsing stays in report untouched.
"""
from __future__ import annotations

import json
import re
from typing import Final

from .collector import COMBINED_OUTPUT_LIMIT_BYTES, RECORD_LIMIT_BYTES
from .coverage import (NAMESPACE_LINKS, PROTECTED_ENTRY_LIMIT, production_root_spellings,
                       sqlite_absence_wire_ok)
from .egress import CHILD_STAGES, hardened_records, hardened_stream_ok, lf_lines
from .kernel import MAX_WATCHES
from .resource import (ABORT_GUARD_RSS_ABOVE_BYTES, ABORT_MEM_AVAILABLE_BELOW_BYTES,
                       ABORT_SUPERVISOR_RSS_ABOVE_BYTES, CHILD_ADDRESS_SPACE_LIMIT_BYTES)
from .sizing_models import (CleanupWire, GuardEvent, Kinds, ORIGINAL_VALUE, SIZING_BLOCKED_EVENT,
                            SIZING_COMPLETION_EVENT, SIZING_MODE, SIZING_READY_EVENT, SizingChild,
                            SizingWire, TEMPORARY_VALUE)
from .sizing_records import child_chronology_ok, typed_record

HEX64: Final = re.compile(r"[0-9a-f]{64}")
COUNTERS: Final = ("discovered", "processed", "pending", "watch_descriptors",
                   "path_bytes", "regular_file_bytes")
KIND_KEYS: Final = ("directories", "regular_files", "symlinks")
FLAGS: Final = ("inventory_complete", "acquisition", "task5_completed")
RESTORED_CLASSES: Final = frozenset({"restore_original", "no_write_original_already"})
# coverage.protect blocks unless both protected roots and every approved link
# (the codegraph approved_link plus the owner-approved namespace links) are
# discovered, so a completed sizing is never empty: at least the two root
# directories plus (1 approved codegraph link + namespace links) symlink
# pathnames. Counted as pathnames, never inode-equal; hardlinks collapse only
# the watch count, not the discovered pathname floor.
GUARANTEED_MIN_DIRECTORIES: Final = 2
GUARANTEED_MIN_SYMLINKS: Final = 1 + len(NAMESPACE_LINKS)


def _exact_int(value: object) -> int | None:
    """Exact int with bool excluded; None marks missing or wrongly typed."""
    return value if type(value) is int else None


def _nonnegative(data: object, keys: tuple[str, ...]) -> dict[str, int] | None:
    """Exact nonnegative ints for every key; None marks one missing/wrong/negative."""
    if not isinstance(data, dict):
        return None
    out: dict[str, int] = {}
    for key in keys:
        value = _exact_int(data.get(key))
        if value is None or value < 0:
            return None
        out[key] = value
    return out


def sizing_wire(data: object) -> SizingWire | None:
    """Validate one completion/wire section; project mode and cleanup un-dropped."""
    if not isinstance(data, dict) or data.get("mode") != SIZING_MODE:
        return None
    kinds_raw, cleanup_raw = data.get("kinds"), data.get("cleanup")
    if not isinstance(kinds_raw, dict) or not isinstance(cleanup_raw, dict):
        return None
    if any(data.get(flag) is not False for flag in FLAGS):
        return None
    if "roots" not in data or "sqlite_absence" not in data:
        return None
    roots_raw = data.get("roots")
    if not isinstance(roots_raw, list) or len(roots_raw) != 2 or not all(
            type(item) is str for item in roots_raw):
        return None
    sqlite_raw = data.get("sqlite_absence")
    if roots_raw == list(production_root_spellings()):
        if sqlite_raw is None:
            return None
    elif sorted(roots_raw) == sorted(production_root_spellings()):
        return None
    record = None
    if sqlite_raw is not None:
        record = sqlite_absence_wire_ok(sqlite_raw)
        if record is None:
            return None
    counters = _nonnegative(data, COUNTERS)
    kinds = _nonnegative(kinds_raw, KIND_KEYS)
    descriptor_count = _exact_int(cleanup_raw.get("descriptor_count"))
    if counters is None or kinds is None or descriptor_count is None:
        return None
    if cleanup_raw.get("watch_closed") is not True or cleanup_raw.get("fd_closed") is not True:
        return None
    discovered, processed = counters["discovered"], counters["processed"]
    watches, pending = counters["watch_descriptors"], counters["pending"]
    path_bytes, file_bytes = counters["path_bytes"], counters["regular_file_bytes"]
    if discovered != processed or pending != 0 or discovered > PROTECTED_ENTRY_LIMIT:
        return None
    if sum(kinds.values()) != discovered or path_bytes <= 0:
        return None
    if kinds["regular_files"] == 0 and file_bytes != 0:
        return None
    if kinds["directories"] < GUARANTEED_MIN_DIRECTORIES or kinds["symlinks"] < GUARANTEED_MIN_SYMLINKS:
        return None
    # Shared ancestors and hardlinked inodes collapse watches, so the watch
    # count is bounded by, never equated to, the discovered path count.
    if not 1 <= watches <= min(MAX_WATCHES, discovered + 7):
        return None
    if descriptor_count != watches:
        return None
    return SizingWire(mode=SIZING_MODE, discovered=discovered, processed=processed, pending=pending,
                      kinds=Kinds(directories=kinds["directories"], regular_files=kinds["regular_files"],
                                  symlinks=kinds["symlinks"]),
                      watch_descriptors=watches, path_bytes=path_bytes, regular_file_bytes=file_bytes,
                      cleanup=CleanupWire(watch_closed=True, fd_closed=True,
                                          descriptor_count=descriptor_count),
                      inventory_complete=False, acquisition=False, task5_completed=False,
                      roots=list(roots_raw), sqlite_absence=record)


def production_sizing_wire(data: object) -> SizingWire | None:
    """Operational sizing gate on top of the generic projection.

    Every operational acceptor (the strict child transcript parser and the
    final accepted_sizing_output) applies this gate unconditionally: exactly
    the ordered production root spellings, an exact non-null sqlite_absence
    record, and at least the guaranteed symlink floor plus the exact watched
    link. The generic sizing_wire projection stays available for generic
    projection only - never as an operational toggle, flag or CLI option.
    """
    projected = sizing_wire(data)
    if projected is None:
        return None
    if projected["roots"] != list(production_root_spellings()):
        return None
    if sqlite_absence_wire_ok(projected["sqlite_absence"]) is None:
        return None
    if projected["kinds"]["symlinks"] < GUARANTEED_MIN_SYMLINKS + 1:
        return None
    return projected


def parse_sizing_child(stdout: str) -> SizingChild:
    """Strictly parse one sizing child transcript; never raises and never drops."""
    events: list[GuardEvent] = []
    parsed_records: list[dict] = []
    completion_data: object = None
    ready = completed = 0
    blocked = False
    for line in lf_lines(stdout):
        if not line.strip():
            continue
        if len(line.encode("utf-8", "surrogatepass")) > RECORD_LIMIT_BYTES:
            return SizingChild(False, "record_over_limit", tuple(events), None)
        try:
            parsed = json.loads(line)
        except json.JSONDecodeError:
            return SizingChild(False, "malformed_or_truncated_line", tuple(events), None)
        record = typed_record(parsed)
        if record is None:
            return SizingChild(False, "not_a_typed_known_record", tuple(events), None)
        name, data = record
        events.append(GuardEvent(name, line))
        if isinstance(parsed, dict):
            parsed_records.append(parsed)
        if name == SIZING_READY_EVENT:
            if _exact_int(data) != CHILD_ADDRESS_SPACE_LIMIT_BYTES:
                return SizingChild(False, "ready_data_inconsistent", tuple(events), None)
            ready += 1
        elif name == SIZING_COMPLETION_EVENT:
            completed += 1
            completion_data = data
        elif name == SIZING_BLOCKED_EVENT:
            blocked = True
    if not hardened_stream_ok(parsed_records, stdout, CHILD_STAGES):
        return SizingChild(False, "diagnostics_incomplete", tuple(events), None)
    if ready != 1:
        return SizingChild(False, "ready_not_exactly_once", tuple(events), None)
    if completed != 1:
        return SizingChild(False, "completion_not_exactly_once", tuple(events), None)
    if blocked:
        return SizingChild(False, "completion_and_blocked", tuple(events), None)
    if hardened_records(parsed_records) and not child_chronology_ok(parsed_records):
        return SizingChild(False, "child_chronology", tuple(events), None)
    completion = production_sizing_wire(completion_data)
    return (SizingChild(True, None, tuple(events), completion) if completion is not None
            else SizingChild(False, "completion_inconsistent", tuple(events), None))


def sizing_restoration_ok(data: object) -> bool:
    """Restoration classes must justify the observed original/temp/final pair."""
    if not isinstance(data, dict) or data.get("restored_original") is not True:
        return False
    if data.get("raise_class") != "proceed" or data.get("restore_class") not in RESTORED_CLASSES:
        return False
    observed = _nonnegative(data, ("original_observed", "temp_observed", "restore_observed"))
    return (observed is not None and observed["original_observed"] == ORIGINAL_VALUE
            and observed["temp_observed"] == TEMPORARY_VALUE
            and observed["restore_observed"] == ORIGINAL_VALUE)


def sizing_resources_ok(resources: object) -> bool:
    """Complete readings inside the active resource bounds; samples must exist."""
    readings = _nonnegative(resources, ("min_mem_available_bytes", "max_guard_rss_bytes",
                                        "max_supervisor_rss_bytes", "samples"))
    if readings is None or readings["samples"] == 0:
        return False
    return (readings["min_mem_available_bytes"] >= ABORT_MEM_AVAILABLE_BELOW_BYTES
            and readings["max_guard_rss_bytes"] <= ABORT_GUARD_RSS_ABOVE_BYTES
            and readings["max_supervisor_rss_bytes"] <= ABORT_SUPERVISOR_RSS_ABOVE_BYTES)


def sizing_collection_ok(collection: object) -> bool:
    """Counters inside the collection caps with intact 64-hex stream hashes."""
    counters = _nonnegative(collection, ("combined_bytes", "records"))
    if not isinstance(collection, dict) or counters is None:
        return False
    combined, records = counters["combined_bytes"], counters["records"]
    if records > combined or combined > COMBINED_OUTPUT_LIMIT_BYTES:
        return False
    # Two-stream framing upper bound: each completed line is at most
    # RECORD_LIMIT_BYTES plus one LF, and each of the two streams carries at most
    # one unterminated tail of at most RECORD_LIMIT_BYTES. Any larger relation is
    # impossible for the collector to have produced.
    if combined > records * (RECORD_LIMIT_BYTES + 1) + 2 * RECORD_LIMIT_BYTES:
        return False
    if collection.get("truncated") is not False or collection.get("invalidated") is not False:
        return False
    if "first_cause" not in collection or collection["first_cause"] is not None:
        return False
    for name in ("stdout_sha256", "stderr_sha256"):
        digest = collection.get(name)
        if not isinstance(digest, str) or HEX64.fullmatch(digest) is None:
            return False
    return True


def sizing_evidence_consistent(collection: object, names: object) -> bool:
    """Cross-section: collected bytes/records must justify the asserted events."""
    if not isinstance(collection, dict) or not isinstance(names, list):
        return False
    counters = _nonnegative(collection, ("combined_bytes", "records"))
    if counters is None:
        return False
    combined, records = counters["combined_bytes"], counters["records"]
    return combined > 0 and records >= 2 and records >= len(names)


def sizing_events_ok(names: object) -> bool:
    """Completion and readiness names exactly once; a blocked record never counts."""
    if not isinstance(names, list) or not all(isinstance(name, str) for name in names):
        return False
    return (names.count(SIZING_COMPLETION_EVENT) == 1
            and names.count(SIZING_READY_EVENT) == 1
            and SIZING_BLOCKED_EVENT not in names)
