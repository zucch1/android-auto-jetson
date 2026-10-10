# SPDX-License-Identifier: GPL-3.0-or-later
"""Guard-event parsing and the window receipt outcomes for independent review.

Two strict, mode-bound acceptors exist: ceiling acceptance for prerequisite
windows and sizing acceptance for topology-only sizing windows. A restored
sizing window can never satisfy ceiling or prerequisite acceptance, and a
ceiling window can never satisfy sizing acceptance. Sizing wire outcomes are
compact (counters and hashes); raw child output is never wrapped twice. Both
acceptors also validate terminal-record completeness on hardened streams:
once diagnostic records appear, the cleanup/egress transitions and the single
terminal result must be complete, ordered and un-truncated. Legacy streams
carrying only the outcome record keep their exact acceptance semantics.
"""
from __future__ import annotations

import json
from dataclasses import asdict, dataclass
from typing import Final

from .egress import (ENTRY_EVENT, HEARTBEAT_EVENT, META_EVENT, STAGE_COMPLETED_EVENT,
                     STAGE_STARTED_EVENT, SUPERVISOR_STAGES, TERMINAL_TRANSITIONS,
                     hardened_records, hardened_stream_ok, lf_lines)
from .guard import GuardResult
from .resource import (ADMISSION_MEM_AVAILABLE_BYTES, SIZING_ADMISSION_MEM_AVAILABLE_BYTES,
                       Sampler)
from .sizing_models import (CEILING_MODE, CEILING_OUTCOME_EVENT, CollectionWire, GuardEvent, Kinds,
                            ORIGINAL_VALUE, SIZING_MODE, SIZING_OUTCOME_EVENT, ResourceWire,
                            SizingWire, TEMPORARY_VALUE)
from .sizing_validation import (parse_sizing_child, production_sizing_wire, sizing_collection_ok,
                                sizing_events_ok, sizing_evidence_consistent, sizing_resources_ok,
                                sizing_restoration_ok)

STREAM_KINDS: Final = frozenset({
    ENTRY_EVENT, META_EVENT, STAGE_STARTED_EVENT, STAGE_COMPLETED_EVENT, HEARTBEAT_EVENT,
    "cleanup_started", "cleanup_completed", "egress_started",
    "limit_readiness", "ceiling_raise", "ceiling_raised", "ceiling_cleanup",
    "guard_result", "guard_event", "guard_stderr",
    CEILING_OUTCOME_EVENT, SIZING_OUTCOME_EVENT})


def parse_guard_events(stdout: str) -> tuple[GuardEvent, ...]:
    """Parse guard stdout into structured events; prose/malformed lines are dropped."""
    events: list[GuardEvent] = []
    for line in stdout.splitlines():
        line = line.strip()
        if not line:
            continue
        try:
            obj = json.loads(line)
        except json.JSONDecodeError:
            continue
        if isinstance(obj, dict) and isinstance(obj.get("event"), str):
            events.append(GuardEvent(obj["event"], line))
    return tuple(events)


@dataclass(frozen=True, slots=True)
class GuardEvidence:
    """Parsed guard evidence for one window: validity, events and forward records."""

    valid: bool
    events: tuple[GuardEvent, ...]
    sizing: SizingWire | None
    forward: tuple[tuple[str, str], ...]
    error: str | None


def guard_evidence(result: GuardResult, mode: str, final_event: str) -> GuardEvidence:
    """Strict sizing transcript or legacy ceiling events; forward records retained.

    Claim limitation (Oracle correction 4): the forwarded child records are
    collected after the window, never streamed live. Live localization is
    supervisor-level only; child-stage localization is retrospective via the
    collected child transcript.
    """
    forward: list[tuple[str, str]] = []
    error: str | None = None
    if mode == SIZING_MODE:
        child = parse_sizing_child(result.stdout)
        events, sizing = child.events, child.completion
        if not child.valid:
            error = f"sizing_child_rejected {child.reason}"
    else:
        child, events, sizing = None, parse_guard_events(result.stdout), None
    forward.extend(("guard_event", event.raw) for event in events)
    if result.stderr:
        forward.append(("guard_stderr", result.stderr))
    valid = (result.exit_status == 0 and not result.timed_out and not result.killed
             and not result.invalidated and not result.truncated
             and (child.valid if child is not None
                  else any(event.event == final_event for event in events)))
    return GuardEvidence(valid, events, sizing, tuple(forward), error)


def collection_wire(result: GuardResult) -> CollectionWire:
    """Compact collection section from one collected guard result."""
    return CollectionWire(combined_bytes=result.combined_bytes, records=result.records,
                          stdout_sha256=result.stdout_sha256, stderr_sha256=result.stderr_sha256,
                          truncated=result.truncated, invalidated=result.invalidated,
                          first_cause=result.first_cause)


def resource_wire(sampler: Sampler) -> ResourceWire | None:
    """Compact resource section from the supervisor sampler, if it sampled."""
    extrema = sampler.extrema()
    return ResourceWire(min_mem_available_bytes=extrema.min_mem_available_bytes,
                        max_guard_rss_bytes=extrema.max_guard_rss_bytes,
                        max_supervisor_rss_bytes=extrema.max_supervisor_rss_bytes,
                        samples=sampler.samples)


@dataclass(frozen=True, slots=True)
class SupervisorOutcome:
    """Full window disposition receipted separately for independent review."""

    accepted: bool
    guard_valid: bool
    restored_original: bool
    raise_class: str
    restore_class: str
    conflicts: tuple[str, ...]
    errors: tuple[str, ...]
    signal: int | None
    original_observed: int | None
    temp_observed: int | None
    restore_observed: int | None
    guard_exit: int | None
    guard_events: tuple[GuardEvent, ...]
    guard_stdout: str
    guard_stderr: str
    mode: str = CEILING_MODE
    sizing: SizingWire | None = None
    resources: ResourceWire | None = None
    collection: CollectionWire | None = None


def _restored(data: dict) -> bool:
    return (all(data.get(field) is True for field in ("accepted", "guard_valid", "restored_original"))
            and data.get("errors") == [] and data.get("conflicts") == []
            and "signal" in data and data["signal"] is None
            and all(type(data.get(field)) is int and data[field] == value for field, value in
                    (("original_observed", ORIGINAL_VALUE), ("temp_observed", TEMPORARY_VALUE),
                     ("restore_observed", ORIGINAL_VALUE), ("guard_exit", 0))))


def _single_outcome(stdout: str, event: str, mode: str) -> dict | None:
    """Exactly one structured outcome of the named event in the named mode."""
    outcomes = []
    for line in lf_lines(stdout):
        try:
            parsed = json.loads(line)
        except json.JSONDecodeError:
            return None
        if not isinstance(parsed, dict) or not isinstance(parsed.get("event"), str):
            return None
        cross_mode = (parsed["event"] == CEILING_OUTCOME_EVENT and event != CEILING_OUTCOME_EVENT) or (
            parsed["event"] == SIZING_OUTCOME_EVENT and event != SIZING_OUTCOME_EVENT)
        if cross_mode:
            return None
        if parsed["event"] == event:
            outcomes.append(parsed)
    if len(outcomes) != 1:
        return None
    candidate = outcomes[0]
    data = candidate.get("data")
    timestamp = candidate.get("monotonic_ns")
    if not isinstance(data, dict) or type(timestamp) is not int:
        return None
    if mode == SIZING_MODE and timestamp < 0:
        return None
    return data if data.get("mode") == mode and _restored(data) else None


def _terminal_complete(stdout: str, outcome_event: str) -> bool:
    """Terminal-record completeness on hardened streams; legacy streams pass.

    A truncated or malformed record marks the stream incomplete while the
    preserved capture keeps every earlier byte as evidence. Once any diagnostic
    record appears the full set is required: known record kinds only, every
    record LF-terminated, the stream discipline (entry first, metadata second,
    sequence, exactly one balanced pair per expected successful-stage phase)
    and the terminal transitions cleanup_started -> cleanup_completed ->
    egress_started exactly once in order, with the single outcome record as the
    last record.
    """
    records: list[dict] = []
    malformed = False
    for line in lf_lines(stdout):
        line = line.strip()
        if not line:
            continue
        try:
            parsed = json.loads(line)
        except json.JSONDecodeError:
            malformed = True
            continue
        if not isinstance(parsed, dict) or not isinstance(parsed.get("event"), str):
            malformed = True
            continue
        records.append(parsed)
    if not hardened_records(records):
        return True
    if malformed or any(record["event"] not in STREAM_KINDS for record in records):
        return False
    if not hardened_stream_ok(records, stdout, SUPERVISOR_STAGES):
        return False
    transitions = [[index for index, record in enumerate(records)
                    if record["event"] == name] for name in TERMINAL_TRANSITIONS]
    if any(len(found) != 1 for found in transitions):
        return False
    if not transitions[0][0] < transitions[1][0] < transitions[2][0]:
        return False
    outcomes = [index for index, record in enumerate(records) if record["event"] == outcome_event]
    return (len(outcomes) == 1 and outcomes[0] == len(records) - 1
            and outcomes[0] > transitions[2][0])


def accepted_ceiling_output(stdout: str) -> bool:
    """Accept exactly one structured ceiling restoration outcome, never prose."""
    return (_single_outcome(stdout, CEILING_OUTCOME_EVENT, CEILING_MODE) is not None
            and _terminal_complete(stdout, CEILING_OUTCOME_EVENT))


def admission_for_mode(mode: str) -> int:
    """Mode-bound admission floor: sizing-only 3 GiB, ceiling/prerequisite 4 GiB.

    Selected only by the supervisor's existing mode; the shared 4 GiB ceiling
    constant stays the default for every other mode spelling (fail closed to
    the stricter floor).
    """
    return (SIZING_ADMISSION_MEM_AVAILABLE_BYTES if mode == SIZING_MODE
            else ADMISSION_MEM_AVAILABLE_BYTES)


def wire_outcome(outcome: SupervisorOutcome) -> tuple[str, dict]:
    """Serialize one outcome: compact counters/hashes for sizing, full for ceiling."""
    if outcome.mode == SIZING_MODE:
        return SIZING_OUTCOME_EVENT, {
            "mode": outcome.mode,
            "accepted": outcome.accepted,
            "guard_valid": outcome.guard_valid,
            "restored_original": outcome.restored_original,
            "raise_class": outcome.raise_class,
            "restore_class": outcome.restore_class,
            "conflicts": list(outcome.conflicts),
            "errors": list(outcome.errors),
            "signal": outcome.signal,
            "original_observed": outcome.original_observed,
            "temp_observed": outcome.temp_observed,
            "restore_observed": outcome.restore_observed,
            "guard_exit": outcome.guard_exit,
            "guard_event_names": [event.event for event in outcome.guard_events],
            "sizing": outcome.sizing,
            "resources": outcome.resources,
            "collection": outcome.collection,
        }
    return CEILING_OUTCOME_EVENT, asdict(outcome)


def accepted_sizing_output(stdout: str) -> bool:
    """Accept exactly one compact topology-sizing outcome with complete evidence.

    Sizing-only strictness: exact mode and completion names exactly once, a
    justified restoration class pair, and complete counters, resources and
    collection evidence, plus terminal-record completeness on hardened streams.
    Ceiling acceptance and routing stay untouched.
    """
    data = _single_outcome(stdout, SIZING_OUTCOME_EVENT, SIZING_MODE)
    return (data is not None and sizing_restoration_ok(data)
            and sizing_events_ok(data.get("guard_event_names"))
            and sizing_evidence_consistent(data.get("collection"), data.get("guard_event_names"))
            and production_sizing_wire(data.get("sizing")) is not None
            and sizing_resources_ok(data.get("resources"))
            and sizing_collection_ok(data.get("collection"))
            and _terminal_complete(stdout, SIZING_OUTCOME_EVENT))
