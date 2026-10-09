# /// script
# requires-python = ">=3.11"
# dependencies = []
# ///
# Run: python3 -B tests/target_safety/sizing_completion_integrity.py
"""Permanent red/green matrices for the five proven sizing consistency bugs.

Replays the joint-qualification raw adversarial examples (A child negative
monotonic_ns, B ready.data type/value, E impossible two-stream framing relation,
C zero-byte/record cross-section, D missing first_cause key) and locks the
coherent zero-topology probe plus the two-stream framing distinction. Every
matrix is named and encodes the DESIRED post-fix behavior: it runs RED against
the pre-repair validators and GREEN after. Reuses the sizing_outcomes fixtures
(no duplicate fixture copies). The integrator wires this file into the
ceiling_qa suite list; it is deliberately not a legacy shim.
"""
from __future__ import annotations

import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
from sizing_outcomes import (CEILING, SIZING, SQLITE_RECORD, complete, ready, transcript, wire,
                             with_sizing, with_section)
from target_safety.report import accepted_ceiling_output, accepted_sizing_output
from target_safety.resource import CHILD_ADDRESS_SPACE_LIMIT_BYTES
from target_safety.sizing_models import SIZING_COMPLETION_EVENT, SIZING_READY_EVENT
from target_safety.sizing_validation import parse_sizing_child, sizing_collection_ok


def record(event: str, data: object, ts: object = 1) -> str:
    return json.dumps({"event": event, "monotonic_ns": ts, "data": data})


def outer(event: str, data: dict, ts: object) -> str:
    return json.dumps({"event": event, "monotonic_ns": ts, "data": data})


def ready_data(data: object, ts: object = 1) -> str:
    return record(SIZING_READY_EVENT, data, ts)


def coll(**over: object) -> dict:
    return {**SIZING["collection"], **over}


def sizing_with_collection(**over: object) -> str:
    return with_sizing(collection=coll(**over))


def bug_a_child_negative_timestamp() -> None:
    # Given: a ready record carrying a negative monotonic_ns plus a valid completion.
    # When: the transcript is strictly parsed. Then: rejected (real emit() is >= 0).
    for ts in (-5, -1):
        parsed = parse_sizing_child(transcript(ready_data(CHILD_ADDRESS_SPACE_LIMIT_BYTES, ts),
                                               complete()))
        assert not parsed.valid and parsed.reason, f"A child ts={ts} must be rejected"
    # bool is not an int timestamp.
    b = parse_sizing_child(transcript(ready_data(CHILD_ADDRESS_SPACE_LIMIT_BYTES, True), complete()))
    assert not b.valid and b.reason, "A child bool ts must be rejected"
    # Control: nonnegative timestamp stays valid.
    assert parse_sizing_child(transcript(ready_data(CHILD_ADDRESS_SPACE_LIMIT_BYTES, 5),
                                         complete())).valid
    print("PASS A: child monotonic_ns exact int >= 0 (bool rejected); control valid")


def bug_a_sizing_outer_timestamp_nonnegative() -> None:
    # Given: a valid sizing outcome whose OUTER monotonic_ns is negative.
    # When: sizing acceptance runs. Then: rejected (sizing outer ts >= 0).
    neg = outer("sizing_outcome", SIZING, -5)
    assert not accepted_sizing_output(neg), "A sizing outer negative ts must be rejected"
    # Control: sizing outer ts >= 0 accepted; ceiling LEGACY negative outer ts preserved.
    assert accepted_sizing_output(outer("sizing_outcome", SIZING, 5))
    assert accepted_ceiling_output(outer("ceiling_outcome", CEILING, -5)), \
        "ceiling legacy negative outer ts must stay accepted"
    print("PASS A: sizing outer ts nonnegative; ceiling legacy preserved")


def bug_b_ready_data_exact_type_and_value() -> None:
    # Given: a ready record whose data is not the exact int child address-space limit.
    # When: the transcript is parsed. Then: every wrong value/type is rejected.
    wrong = {"value_999": 999, "value_zero": 0, "value_big": 2147483649,
             "string": "not-int", "null": None, "bool": True,
             "float": float(CHILD_ADDRESS_SPACE_LIMIT_BYTES)}
    for label, data in wrong.items():
        parsed = parse_sizing_child(transcript(ready_data(data), complete()))
        assert not parsed.valid and parsed.reason, f"B ready.data {label} must be rejected"
    # Control: the exact int value stays valid.
    assert parse_sizing_child(transcript(ready_data(CHILD_ADDRESS_SPACE_LIMIT_BYTES),
                                         complete())).valid
    print(f"PASS B: ready.data exact int {CHILD_ADDRESS_SPACE_LIMIT_BYTES}; {len(wrong)} wrong rejected")


def bug_e_two_stream_framing_bound() -> None:
    # Given: the derived two-stream framing bound records*(record_limit+1)+2*record_limit.
    # When: byte/record relations are checked. Then: impossible relations rejected.
    assert not sizing_collection_ok(coll(records=1, combined_bytes=200000)), \
        "E records=1 bytes=200000 exceeds 196609 bound"
    assert not sizing_collection_ok(coll(records=253, combined_bytes=2 ** 24)), \
        "E 16MiB with 253 records exceeds framing bound"
    # The exact 16MiB boundary is physically reachable with a consistent record count.
    assert sizing_collection_ok(coll(records=254, combined_bytes=2 ** 24)), \
        "E 16MiB with 254 records is physically consistent"
    # Distinction: records=0 bytes=100000 is generic-two-tail legal framing ...
    assert sizing_collection_ok(coll(records=0, combined_bytes=100000)), \
        "E records=0 bytes=100000 must stay framing-legal (two unterminated tails)"
    # ... but must NOT justify a completed sizing's two records (see bug C).
    assert not accepted_sizing_output(sizing_with_collection(records=0, combined_bytes=100000)), \
        "E records=0 cannot justify completed sizing's 2 records"
    print("PASS E: two-stream framing bound; distinction (generic-legal vs sizing-consistent)")


def bug_c_cross_section_collection_justifies_events() -> None:
    # Given: a valid outcome whose collection has zero bytes/records but asserts two events.
    # When: sizing acceptance runs. Then: rejected (0-frame placeholder cannot justify it).
    zero = sizing_with_collection(combined_bytes=0, records=0)
    assert not accepted_sizing_output(zero), "C zero collection cannot justify 2 events"
    # A single record cannot justify ready+completion either.
    one = sizing_with_collection(combined_bytes=1024, records=1)
    assert not accepted_sizing_output(one), "C records=1 cannot justify ready+completion"
    # Control: the fixture's 2-record positive collection stays accepted.
    assert accepted_sizing_output(with_sizing())
    print("PASS C: positive bytes + enough records justify ready+completion")


def bug_d_first_cause_key_required() -> None:
    # Given: a valid outcome whose collection is MISSING the first_cause key entirely.
    # When: sizing acceptance runs. Then: rejected (absence is not a clean None).
    missing = {k: v for k, v in SIZING["collection"].items() if k != "first_cause"}
    assert not accepted_sizing_output(with_sizing(collection=missing)), \
        "D missing first_cause key must be rejected"
    # Control: present-and-None (clean) accepted; present-and-cause rejected.
    assert accepted_sizing_output(sizing_with_collection(first_cause=None))
    assert not accepted_sizing_output(sizing_with_collection(first_cause="stdin_write_fault"))
    print("PASS D: first_cause key required (absence != clean None)")


def coherent_zero_topology_rejected() -> None:
    # Given: an internally-coherent but EMPTY topology (a real sizing covers two
    # roots + approved codegraph link + two namespace links, so it can never be
    # empty). When: wire/acceptance runs. Then: rejected by the guaranteed minimum.
    empty = {"mode": "topology-sizing", "discovered": 0, "processed": 0, "pending": 0,
             "kinds": {"directories": 0, "regular_files": 0, "symlinks": 0},
             "watch_descriptors": 1, "path_bytes": 1, "regular_file_bytes": 0,
             "cleanup": {"watch_closed": True, "fd_closed": True, "descriptor_count": 1},
             "inventory_complete": False, "acquisition": False, "task5_completed": False,
             "roots": list(SIZING["sizing"]["roots"]), "sqlite_absence": SQLITE_RECORD}
    assert not accepted_sizing_output(with_sizing(sizing=empty)), \
        "coherent zero-topology must be rejected (2 roots + 3 symlinks cannot be empty)"
    # Regression-lock the exact probe mutation shape from the raw evidence.
    probe = {**empty, "discovered": 0, "processed": 0,
             "kinds": {"directories": 0, "regular_files": 0, "symlinks": 0}}
    assert not accepted_sizing_output(with_sizing(sizing=probe))
    print("PASS coherent zero-topology rejected; regression-locked")


def record_limit_measures_utf8_bytes() -> None:
    # Given: a record whose UTF-8 bytes exceed RECORD_LIMIT_BYTES while its
    # Unicode code-point length does not (bounded multibyte payload).
    # When: the transcript is parsed. Then: rejected as record_over_limit.
    from target_safety.collector import RECORD_LIMIT_BYTES
    huge = json.dumps({"event": "watch_cleanup", "data": "\u00e9" * 40000,
                       "monotonic_ns": 1}, ensure_ascii=False)
    assert len(huge) <= RECORD_LIMIT_BYTES and len(huge.encode("utf-8")) > RECORD_LIMIT_BYTES
    parsed = parse_sizing_child(transcript(ready_data(CHILD_ADDRESS_SPACE_LIMIT_BYTES), huge,
                                           complete()))
    assert not parsed.valid and parsed.reason == "record_over_limit", \
        "UTF-8 over-64KiB byte record must be rejected"
    print("PASS record limit measures UTF-8 bytes, not code points")


def main() -> int:
    bug_a_child_negative_timestamp()
    bug_a_sizing_outer_timestamp_nonnegative()
    bug_b_ready_data_exact_type_and_value()
    bug_e_two_stream_framing_bound()
    bug_c_cross_section_collection_justifies_events()
    bug_d_first_cause_key_required()
    coherent_zero_topology_rejected()
    record_limit_measures_utf8_bytes()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
