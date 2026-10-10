# SPDX-License-Identifier: GPL-3.0-or-later
# /// script
# requires-python = ">=3.11"
# dependencies = []
# ///
# Run: python3 -B tests/target_safety/sizing_sqlite_wire.py
"""Operational sizing wire gate: exact production roots, exact record, symlink floor.

Given: a fully valid production completion (resources, collection, restoration).
When: the strict child parser or the final accepted_sizing_output sees it.
Then: both accept; every isolated one-field mutation rejects both. The generic
sizing_wire projection stays available (nullable for unrelated roots) but is
never an operational path: only production_sizing_wire gates acceptance.
"""
from __future__ import annotations

import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
from target_safety.report import SupervisorOutcome, accepted_sizing_output, wire_outcome
from target_safety.sizing_models import (SIZING_COMPLETION_EVENT, SIZING_MODE, SIZING_READY_EVENT,
                                         GuardEvent, SizingWire)
from target_safety.sizing_validation import (parse_sizing_child, production_sizing_wire,
                                             sizing_wire)
from sizing_outcomes import SIZING

RECORD = SIZING["sizing"]["sqlite_absence"]
ROOTS = list(SIZING["sizing"]["roots"])


def _acceptors(completion: dict) -> tuple[bool, bool]:
    child = parse_sizing_child("\n".join([
        json.dumps({"event": SIZING_READY_EVENT, "data": 2147483648, "monotonic_ns": 1}),
        json.dumps({"event": SIZING_COMPLETION_EVENT, "data": completion, "monotonic_ns": 2}),
    ])).valid
    final = accepted_sizing_output(json.dumps(
        {"event": "sizing_outcome", "monotonic_ns": 1, "data": {**SIZING, "sizing": completion}}))
    return child, final


def full_valid_outcome_accepted_and_retained_by_both() -> None:
    # Given: the complete positive control with record, roots, resources and
    # collection. Then: both acceptors accept and the projection retains the
    # validated record and ordered roots un-dropped.
    assert _acceptors(dict(SIZING["sizing"])) == (True, True)
    projected = production_sizing_wire(SIZING["sizing"])
    assert projected is not None and projected["sqlite_absence"] == RECORD
    assert projected["roots"] == ROOTS
    outcome = SupervisorOutcome(
        accepted=True, guard_valid=True, restored_original=True, raise_class="proceed",
        restore_class="restore_original", conflicts=(), errors=(), signal=None,
        original_observed=65536, temp_observed=1048576, restore_observed=65536, guard_exit=0,
        guard_events=(GuardEvent(SIZING_READY_EVENT, "{}"),
                      GuardEvent(SIZING_COMPLETION_EVENT, "{}")),
        guard_stdout="", guard_stderr="", mode=SIZING_MODE,
        sizing=SizingWire(**projected), resources=None, collection=None)
    _, data = wire_outcome(outcome)
    assert data["sizing"]["sqlite_absence"] == RECORD and data["sizing"]["roots"] == ROOTS
    print("PASS full valid outcome accepted by child and final acceptor; record retained")


def isolated_mutations_reject_both_acceptors() -> None:
    # Given: the positive control mutated in exactly one field per case.
    # Then: the child parser and the final acceptor both reject every case.
    no_key = {k: v for k, v in SIZING["sizing"].items() if k != "sqlite_absence"}
    mutations = {
        "roots only mutated, record kept": {**SIZING["sizing"],
                                           "roots": ["/tmp/not-approved", ROOTS[1]]},
        "record null": {**SIZING["sizing"], "sqlite_absence": None},
        "record wrong field": {**SIZING["sizing"],
                               "sqlite_absence": {**RECORD, "target": "/tmp/other"}},
        "record bool instead of string": {**SIZING["sizing"],
                                          "sqlite_absence": {**RECORD, "link": True}},
        "record missing inner key": {**SIZING["sizing"], "sqlite_absence": {
            k: v for k, v in RECORD.items() if k != "link"}},
        "roots and null together": {**SIZING["sizing"], "roots": ["/tmp/not-approved", ROOTS[1]],
                                    "sqlite_absence": None},
        "reversed roots": {**SIZING["sizing"], "roots": [ROOTS[1], ROOTS[0]]},
        "duplicate roots": {**SIZING["sizing"], "roots": [ROOTS[0], ROOTS[0]]},
        "malformed roots shorter": {**SIZING["sizing"], "roots": [ROOTS[0]]},
        "malformed roots wrong type": {**SIZING["sizing"], "roots": [ROOTS[0], 2]},
        "dictionary key removal record": no_key,
        "dictionary key removal roots": {k: v for k, v in SIZING["sizing"].items() if k != "roots"},
        "coherent three symlinks preserving total": {
            **SIZING["sizing"],
            "kinds": {"directories": 3, "regular_files": 4, "symlinks": 3}},
    }
    wrong: list[str] = []
    for reason, completion in mutations.items():
        assert json.loads(json.dumps(completion)) == completion, "mutations stay valid JSON"
        child, final = _acceptors(completion)
        if child or final:
            wrong.append(f"{reason}: child={child} final={final}")
    assert not wrong, f"mutated evidence wrongly accepted: {wrong}"
    print(f"PASS {len(mutations)} isolated mutations rejected by both acceptors")


def oracle_bypass_is_closed_for_operations_but_not_for_projection() -> None:
    # Given: the exact reported bypass - not-approved first root, production
    # external second, null record. Then: the generic projection still parses
    # it (projection only) while every operational path rejects it.
    bypass = {**SIZING["sizing"], "roots": ["/tmp/not-approved", ROOTS[1]], "sqlite_absence": None}
    assert sizing_wire(bypass) is not None
    assert production_sizing_wire(bypass) is None
    assert _acceptors(bypass) == (False, False)
    print("PASS oracle bypass closed for operations; generic projection unchanged")


def generic_projection_stays_nullable_for_unrelated_roots() -> None:
    # Given: unrelated roots with null evidence. Then: the generic projection
    # still returns the record-less wire, and no operational acceptor takes it.
    unrelated = {**SIZING["sizing"], "roots": ["/tmp/a", "/tmp/b"], "sqlite_absence": None}
    projected = sizing_wire(unrelated)
    assert projected is not None and projected["sqlite_absence"] is None
    assert production_sizing_wire(unrelated) is None
    assert _acceptors(unrelated) == (False, False)
    exact = {**SIZING["sizing"], "roots": ["/tmp/a", "/tmp/b"]}
    assert sizing_wire(exact) is not None and sizing_wire(exact)["sqlite_absence"] == RECORD
    assert sizing_wire({**exact, "sqlite_absence": {**RECORD, "enoent_observed": 1}}) is None
    print("PASS generic projection nullable for unrelated roots; non-null records stay exact")


def main() -> int:
    full_valid_outcome_accepted_and_retained_by_both()
    isolated_mutations_reject_both_acceptors()
    oracle_bypass_is_closed_for_operations_but_not_for_projection()
    generic_projection_stays_nullable_for_unrelated_roots()
    print("PASS all sqlite watched-absence operational wire suites")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
