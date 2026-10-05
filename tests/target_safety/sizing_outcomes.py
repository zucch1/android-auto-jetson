# /// script
# requires-python = ">=3.11"
# dependencies = []
# ///
# Run: python3 -B tests/target_safety/sizing_outcomes.py
"""Strict mode-bound sizing and ceiling outcome acceptance; no transport involved."""
from __future__ import annotations

import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
from target_safety.coverage import EXTERNAL, REPOSITORY, SQLITE_LINK, SQLITE_LINK_TEXT, SQLITE_TARGET
from target_safety.payload import MODULES, SIZING_GUARD_MODULES, SUPERVISOR_MODULES
from target_safety.report import accepted_ceiling_output, accepted_sizing_output
from target_safety.sizing_models import SIZING_COMPLETION_EVENT, SIZING_READY_EVENT
from target_safety.sizing_validation import parse_sizing_child, sizing_wire

STDOUT_SHA = "fce71a0788fa468fbf300908433de2a594da93225c800848ccc161e1a2b89801"
STDERR_SHA = "33940ea0bbd20c4269c72f772186eecab5ae6b1db32922c04a23189e44e98f7d"
CEILING = {"accepted": True, "guard_valid": True, "restored_original": True,
           "original_observed": 65536, "temp_observed": 1048576, "restore_observed": 65536,
           "guard_exit": 0, "errors": [], "conflicts": [], "signal": None, "mode": "ceiling"}
SQLITE_RECORD = {"link": str(SQLITE_LINK), "literal_link_text": SQLITE_LINK_TEXT,
                 "target": str(SQLITE_TARGET), "link_parent": str(SQLITE_LINK.parent),
                 "target_parent": str(SQLITE_TARGET.parent),
                 "enoent_observed": True, "link_watched": True,
                 "link_parent_watched": True, "target_parent_watched": True}
SIZING = {
    "accepted": True, "guard_valid": True, "restored_original": True,
    "raise_class": "proceed", "restore_class": "restore_original",
    "original_observed": 65536, "temp_observed": 1048576, "restore_observed": 65536,
    "guard_exit": 0, "errors": [], "conflicts": [], "signal": None, "mode": "topology-sizing",
    "guard_event_names": [SIZING_READY_EVENT, SIZING_COMPLETION_EVENT],
    "sizing": {"mode": "topology-sizing", "discovered": 10, "processed": 10, "pending": 0,
               "kinds": {"directories": 3, "regular_files": 3, "symlinks": 4},
               "watch_descriptors": 12, "path_bytes": 220, "regular_file_bytes": 44,
               "cleanup": {"watch_closed": True, "fd_closed": True, "descriptor_count": 12},
               "inventory_complete": False, "acquisition": False, "task5_completed": False,
               "roots": [str(REPOSITORY), str(EXTERNAL)], "sqlite_absence": SQLITE_RECORD},
    "resources": {"min_mem_available_bytes": 8589934592, "max_guard_rss_bytes": 134217728,
                  "max_supervisor_rss_bytes": 33554432, "samples": 4},
    "collection": {"combined_bytes": 1024, "records": 2,
                   "stdout_sha256": STDOUT_SHA, "stderr_sha256": STDERR_SHA,
                   "truncated": False, "invalidated": False, "first_cause": None},
}


def wire(event: str, data: dict) -> str:
    return json.dumps({"event": event, "monotonic_ns": 1, "data": data})


def with_sizing(**overrides: object) -> str:
    return wire("sizing_outcome", {**SIZING, **overrides})


def with_section(section: str, **overrides: object) -> str:
    return with_sizing(**{section: {**SIZING[section], **overrides}})


def ready(index: int = 1) -> str:
    return json.dumps({"event": SIZING_READY_EVENT, "data": 2147483648, "monotonic_ns": index})


def complete(data: object = SIZING["sizing"]) -> str:
    return json.dumps({"event": SIZING_COMPLETION_EVENT, "data": data, "monotonic_ns": 9})


def transcript(*lines: str) -> str:
    return "\n".join(lines)


def sizing_accepts_only_complete_compact_outcomes() -> None:
    # Given: one complete compact sizing outcome.
    assert accepted_sizing_output(with_sizing())
    # When: mode mismatch, duplicate, truncation, malformed or missing sections.
    broken = [wire("sizing_outcome", {**SIZING, "mode": "ceiling"}),
              wire("ceiling_outcome", SIZING),
              wire("sizing_outcome", SIZING) + "\n" + wire("sizing_outcome", SIZING),
              wire("sizing_outcome", SIZING)[:40],
              wire("sizing_outcome", {k: v for k, v in SIZING.items() if k != "sizing"}),
              wire("sizing_outcome", {**SIZING, "sizing": {}}),
              with_section("sizing", inventory_complete=True),
              with_section("sizing", acquisition=True),
              with_section("sizing", task5_completed=True),
              with_section("sizing", discovered="9"),
              with_section("collection", truncated=True),
              with_sizing(temp_observed=65536),
              with_sizing(guard_exit=3),
              "prose only",
              "",
              wire("sizing_outcome", SIZING) + "\nnot json",
              wire("sizing_outcome", SIZING) + "\n" + wire("ceiling_outcome", CEILING)]
    for stdout in broken:
        assert not accepted_sizing_output(stdout), stdout[:80]
    print(f"PASS sizing acceptance rejects {len(broken)} mismatched/duplicate/truncated/malformed cases")


def sizing_gate_mutations_rejected() -> None:
    # Given: the exact mutations the rejected gate wrongly accepted, named by
    # the missing/contradictory field each one carries rather than a unit pin.
    mutations = {
        "resources empty": with_sizing(resources={}),
        "resources null": with_sizing(resources=None),
        "resources missing": wire("sizing_outcome", {k: v for k, v in SIZING.items()
                                                    if k != "resources"}),
        "resources negative": with_section("resources", min_mem_available_bytes=-1),
        "resources wrong type": with_section("resources", min_mem_available_bytes="2147483648"),
        "resources bool": with_section("resources", max_guard_rss_bytes=True),
        "resources low memory": with_section("resources", min_mem_available_bytes=1),
        "resources guard rss over ceiling": with_section("resources", max_guard_rss_bytes=2 ** 31),
        "resources supervisor rss over ceiling": with_section("resources",
                                                             max_supervisor_rss_bytes=2 ** 28),
        "resources zero samples": with_section("resources", samples=0),
        "discovered negative": with_section("sizing", discovered=-1),
        "pending negative": with_section("sizing", pending=-1),
        "kinds negative": with_sizing(sizing={**SIZING["sizing"],
                                             "kinds": {"directories": -1, "regular_files": 5,
                                                       "symlinks": 1}}),
        "discovered processed disagree": with_section("sizing", processed=8),
        "pending nonzero": with_section("sizing", pending=1),
        "kinds sum disagrees": with_sizing(sizing={**SIZING["sizing"],
                                                  "kinds": {"directories": 3, "regular_files": 6,
                                                            "symlinks": 4}}),
        "zero files with bytes": with_sizing(sizing={**SIZING["sizing"], "regular_file_bytes": 7,
                                                    "kinds": {"directories": 3, "regular_files": 0,
                                                              "symlinks": 7}}),
        "path bytes zero": with_section("sizing", path_bytes=0),
        "watch descriptors zero": with_section("sizing", watch_descriptors=0),
        "watch descriptors over ancestors": with_section("sizing", watch_descriptors=18),
        "cleanup count mismatch": with_sizing(sizing={**SIZING["sizing"], "cleanup": {
            "watch_closed": True, "fd_closed": True, "descriptor_count": 11}}),
        "cleanup watch open": with_sizing(sizing={**SIZING["sizing"], "cleanup": {
            "watch_closed": False, "fd_closed": True, "descriptor_count": 12}}),
        "cleanup fd open": with_sizing(sizing={**SIZING["sizing"], "cleanup": {
            "watch_closed": True, "fd_closed": False, "descriptor_count": 12}}),
        "completion name absent": with_sizing(guard_event_names=[SIZING_READY_EVENT]),
        "ready name absent": with_sizing(guard_event_names=[SIZING_COMPLETION_EVENT]),
        "completion name duplicated": with_sizing(guard_event_names=[
            SIZING_READY_EVENT, SIZING_COMPLETION_EVENT, SIZING_COMPLETION_EVENT]),
        "event names missing": with_sizing(guard_event_names=None),
        "event names wrong type": with_sizing(guard_event_names=[1, 2]),
        "completion and blocked": with_sizing(guard_event_names=[
            SIZING_READY_EVENT, SIZING_COMPLETION_EVENT, "guard_blocked"]),
        "raise class not proceed": with_sizing(raise_class="no_write_raise_did_not_take"),
        "restore class conflict": with_sizing(restore_class="conflict_no_overwrite"),
        "restore class not_restored": with_sizing(restore_class="not_restored"),
        "restore class read_error": with_sizing(restore_class="read_error"),
        "restore class missing": with_sizing(restore_class=None),
        "raise class missing": with_sizing(raise_class=None),
        "collection hash absent": with_section("collection", stdout_sha256=None),
        "collection hash short": with_section("collection", stdout_sha256="ab"),
        "collection hash uppercase": with_section("collection", stdout_sha256=STDOUT_SHA.upper()),
        "collection hash non-string": with_section("collection", stderr_sha256=3),
        "collection counters absent": with_section("collection", records=None),
        "collection combined negative": with_section("collection", combined_bytes=-1),
        "collection records over combined": with_section("collection", records=4096),
        "collection over cap": with_section("collection", combined_bytes=2 ** 24 + 1),
        "collection recorded fault": with_section("collection", first_cause="stdin_write_fault"),
        "collection invalidated": with_section("collection", invalidated=True),
    }
    wrong: list[str] = []
    for reason, stdout in mutations.items():
        if accepted_sizing_output(stdout):
            print(f"WRONGLY ACCEPTED: {reason}")
            wrong.append(reason)
    assert not wrong, f"{len(wrong)} gate mutations wrongly accepted"
    assert accepted_sizing_output(with_sizing()), "control must stay valid"
    # The exact combined cap is collector-reachable without truncation when the
    # record count is physically consistent, so the boundary itself stays
    # accepted while one byte more cannot exist.
    assert accepted_sizing_output(with_section("collection", combined_bytes=2 ** 24, records=254))
    print(f"PASS sizing acceptance rejects {len(mutations)} gate mutations; control valid")


def sizing_child_transcript_strict() -> None:
    # Given: a well-formed sizing child transcript with one completion.
    assert parse_sizing_child(transcript(ready(), complete())).valid
    assert parse_sizing_child(transcript(ready(), complete(), "")).valid
    # Then: hardlink/shared-ancestor watch collapse stays valid (watches < paths).
    hardlink = {**SIZING["sizing"], "watch_descriptors": 4,
                "cleanup": {"watch_closed": True, "fd_closed": True, "descriptor_count": 4}}
    assert parse_sizing_child(transcript(ready(), complete(hardlink))).valid
    assert accepted_sizing_output(with_sizing(sizing=hardlink))
    # When: duplicated, malformed, truncated, blocked, absent or inconsistent.
    rejected = {
        "duplicate completion": transcript(ready(), complete(), complete()),
        "malformed line": transcript(ready(), complete(), "not json"),
        "truncated line": transcript(ready(), complete(), '{"event": "topology_sizing_compl'),
        "completion and blocked": transcript(ready(), complete(), json.dumps(
            {"event": "guard_blocked", "data": "entry_limit", "monotonic_ns": 9})),
        "completion absent": transcript(ready()),
        "ready absent": transcript(complete()),
        "ready duplicated": transcript(ready(), ready(2), complete()),
        "unknown record": transcript(ready(), complete(), json.dumps(
            {"event": "probe", "data": 1, "monotonic_ns": 9})),
        "record missing data": transcript(ready(), complete(),
                                          '{"event": "child_limit_ready", "monotonic_ns": 9}'),
        "record bad monotonic": transcript(ready(), complete(),
                                           '{"event": "watch_cleanup", "data": "{}",'
                                           ' "monotonic_ns": true}'),
        "record over limit": transcript(ready(), complete(), "x" * 65537),
        "completion mode wrong": transcript(ready(), complete({**SIZING["sizing"],
                                                               "mode": "ceiling"})),
        "completion cleanup missing": transcript(ready(), complete(
            {k: v for k, v in SIZING["sizing"].items() if k != "cleanup"})),
    }
    wrong: list[str] = []
    for reason, stdout in rejected.items():
        parsed = parse_sizing_child(stdout)
        if parsed.valid or not parsed.reason:
            print(f"WRONGLY ACCEPTED: {reason}")
            wrong.append(reason)
    assert not wrong, f"{len(wrong)} child transcripts wrongly accepted"
    print(f"PASS strict sizing child transcript rejects {len(rejected)} cases; never drops a line")


def payload_module_dependency_order() -> None:
    # Given: every bundled module list, the collector helper must precede the
    # collector, the pure sizing models must precede coverage, and the new
    # validation layer must precede its report consumer.
    for modules in (MODULES, SIZING_GUARD_MODULES, SUPERVISOR_MODULES):
        assert modules.index("collector_io") < modules.index("collector"), modules
        assert modules.index("sizing_models") < modules.index("coverage"), modules
    for modules in (SUPERVISOR_MODULES,):
        assert modules.index("sizing_models") < modules.index("sizing_validation")
        assert modules.index("sizing_validation") < modules.index("report")
        assert modules.index("collector") < modules.index("sizing_validation")
    assert "inventory" not in SIZING_GUARD_MODULES and "probes" not in SIZING_GUARD_MODULES
    print("PASS payload module lists keep collector_io and validation dependency order")


def sizing_cannot_satisfy_ceiling_or_prerequisite_acceptance() -> None:
    # Given: a fully restored sizing outcome, then a ceiling outcome with sizing events.
    assert accepted_sizing_output(with_sizing())
    assert not accepted_ceiling_output(wire("sizing_outcome", SIZING))
    assert not accepted_ceiling_output(wire("sizing_outcome", SIZING) + "\n" + wire("ceiling_outcome", CEILING))
    assert not accepted_ceiling_output(wire("ceiling_outcome", {**CEILING, "mode": "topology-sizing"}))
    assert not accepted_ceiling_output(wire("ceiling_outcome", CEILING) + "\n" + wire("sizing_outcome", SIZING))
    assert not accepted_sizing_output(wire("ceiling_outcome", CEILING))
    print("PASS restored sizing output never satisfies ceiling/prerequisite acceptance")


def ceiling_acceptance_keeps_restoration_contract() -> None:
    assert accepted_ceiling_output(wire("ceiling_outcome", CEILING))
    for field, value in (("accepted", False), ("guard_valid", False), ("restored_original", False),
                         ("original_observed", 65535), ("temp_observed", 123456),
                         ("restore_observed", 1048576), ("guard_exit", 1), ("errors", ["x"]),
                         ("conflicts", ["c"]), ("signal", 15), ("mode", None)):
        altered = {k: v for k, v in CEILING.items() if k != field}
        if value is not None:
            altered[field] = value
        assert not accepted_ceiling_output(wire("ceiling_outcome", altered)), field
    assert not accepted_ceiling_output(wire("ceiling_outcome", CEILING) + "\n" + wire("ceiling_outcome", CEILING))
    print("PASS ceiling acceptance keeps the exact restoration contract and rejects duplicates")


def sizing_wire_projection_keeps_validated_fields() -> None:
    # Given: a completion record with extra producer fields, the projection must
    # retain exactly what acceptance re-verifies instead of dropping it.
    completion = {**SIZING["sizing"], "roots": ["/a", "/b"], "namespace_only": ["/a/link"],
                  "literal_link_text": "/usr/bin/python3"}
    projected = sizing_wire(completion)
    assert projected is not None
    assert projected["mode"] == "topology-sizing" and projected["cleanup"]["fd_closed"] is True
    assert projected["cleanup"]["descriptor_count"] == projected["watch_descriptors"]
    assert projected["roots"] == ["/a", "/b"] and projected["sqlite_absence"] == SQLITE_RECORD
    null_evidence = sizing_wire({**completion, "sqlite_absence": None})
    assert null_evidence is not None and null_evidence["sqlite_absence"] is None
    assert sizing_wire(SIZING["sizing"])["sqlite_absence"] == SQLITE_RECORD
    assert sizing_wire({**SIZING["sizing"], "cleanup": None}) is None
    print("PASS sizing wire projection keeps mode/cleanup verifiable and rejects a dropped cleanup")


def main() -> int:
    sizing_accepts_only_complete_compact_outcomes()
    sizing_gate_mutations_rejected()
    sizing_child_transcript_strict()
    payload_module_dependency_order()
    sizing_wire_projection_keeps_validated_fields()
    sizing_cannot_satisfy_ceiling_or_prerequisite_acceptance()
    ceiling_acceptance_keeps_restoration_contract()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
