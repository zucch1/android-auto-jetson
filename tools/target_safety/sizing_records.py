"""Record-level discipline of the sizing child transcript.

One well-formed typed record of the known sizing vocabulary and the hardened
chronology of the child stream (readiness between module load and watch
session, completion last). The strict transcript parser and its wire gates
live in sizing_validation and consume these checks; legacy transcripts (no
diagnostic record) keep their exact semantics and never reach the chronology.
"""
from __future__ import annotations

from typing import Final

from .egress import (ENTRY_EVENT, META_EVENT, STAGE_COMPLETED_EVENT, STAGE_STARTED_EVENT,
                     entry_record_ok, stage_phase)
from .sizing_models import (SIZING_BLOCKED_EVENT, SIZING_CLEANUP_EVENT, SIZING_COMPLETION_EVENT,
                            SIZING_READY_EVENT)

RECORD_KINDS: Final = frozenset({SIZING_READY_EVENT, SIZING_COMPLETION_EVENT,
                                 SIZING_CLEANUP_EVENT, SIZING_BLOCKED_EVENT,
                                 ENTRY_EVENT, META_EVENT,
                                 STAGE_STARTED_EVENT, STAGE_COMPLETED_EVENT})


def typed_record(parsed: object) -> tuple[str, object] | None:
    """One well-formed typed record of the known vocabulary; None marks a bad line.

    The entry record is the documented exception to the data envelope: exactly
    {event, pid, monotonic_ns} (entry_record_ok). Every other kind carries a
    data field, stage records carry exactly one phase identifier; sequence
    shapes are enforced by the stream check once any diagnostic record appears.
    """
    if not isinstance(parsed, dict):
        return None
    name = parsed.get("event")
    if not isinstance(name, str) or name not in RECORD_KINDS:
        return None
    timestamp = parsed.get("monotonic_ns")
    if type(timestamp) is not int or timestamp < 0:
        return None
    if name == ENTRY_EVENT:
        return (name, None) if entry_record_ok(parsed) else None
    if "data" not in parsed:
        return None
    if name in (STAGE_STARTED_EVENT, STAGE_COMPLETED_EVENT):
        return (name, parsed.get("data")) if stage_phase(parsed.get("data")) is not None else None
    return name, parsed.get("data")


def child_chronology_ok(records: list[dict]) -> bool:
    """Hardened child chronology: readiness between module_load and watch_session.

    The completion record is the child's terminal write, so it must be the last
    record of the transcript (topology_sizing_complete before module loading or
    before any later bracket is never accepted); the readiness handshake is
    established after the module load and before the watch session brackets.
    """
    ready_at = completion_at = module_load_end = watch_session_start = -1
    for index, record in enumerate(records):
        name = record.get("event")
        if name == SIZING_READY_EVENT:
            ready_at = index
        elif name == SIZING_COMPLETION_EVENT:
            completion_at = index
        elif name == STAGE_COMPLETED_EVENT and stage_phase(record.get("data")) == "module_load":
            module_load_end = index
        elif name == STAGE_STARTED_EVENT and stage_phase(record.get("data")) == "watch_session":
            watch_session_start = index
    return (completion_at == len(records) - 1
            and module_load_end < ready_at < watch_session_start)
