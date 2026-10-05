# /// script
# requires-python = ">=3.11"
# dependencies = []
# ///
# Run: python3 -B tests/target_safety/diag_stream.py
"""Assembled-bundle entry ordering and hardened-stream terminal completeness.

The entry record must be the earliest executable bounded write in every
assembled payload (static assembly order plus a real stdin run), and a hardened
diagnostic stream is only accepted with a complete terminal record set: entry
first, contiguous sequence numbers, balanced stage pairs, the cleanup/egress
transitions in order and the single outcome record last. Legacy outcome-only
streams keep their exact acceptance semantics.
"""
from __future__ import annotations

import io
import json
import subprocess
import sys
import time
from contextlib import redirect_stdout
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
sys.path.insert(0, str(Path(__file__).resolve().parent))
from target_safety import payload
from target_safety.egress import Writer
from target_safety.report import accepted_ceiling_output, accepted_sizing_output
from target_safety.resource import CHILD_ADDRESS_SPACE_LIMIT_BYTES
from target_safety.sizing_models import SIZING_COMPLETION_EVENT, SIZING_READY_EVENT
from target_safety.sizing_validation import parse_sizing_child
from sizing_outcomes import CEILING, SIZING, complete, ready, transcript, wire

PHASES = ("module_load", "setting_read", "memory_read", "admission", "guard_init", "collection")


def stream(outcome_event: str, outcome_data: dict, **steps: object) -> str:
    """One canonical hardened stream through the real Writer; steps select mutations."""
    out = io.StringIO()
    with redirect_stdout(out):
        writer = Writer(time.monotonic() + 60.0)
        writer.entry()
        writer.meta()
        for phase in PHASES:
            writer.stage_started(phase)
            writer.stage_completed(phase)
        writer.transition("cleanup_started")
        writer.transition("cleanup_completed")
        writer.transition("egress_started")
        if steps.get("extra_after_outcome"):
            writer.terminal(outcome_event, outcome_data)
            writer.record("guard_result", 0)
            return out.getvalue()
        writer.terminal(outcome_event, outcome_data)
    return out.getvalue()


def entry_precedes_module_loading_in_every_bundle() -> None:
    # Given: every assembled payload product.
    for name, assembled in (("default", payload.bundle()),
                            ("sizing_guard", payload.sizing_guard_bundle()),
                            ("supervisor", payload.supervisor_bundle()),
                            ("sizing", payload.sizing_bundle())):
        entry = assembled.index(b"writer().entry()")
        meta = assembled.index(b"writer().meta()")
        opened = assembled.index(b"writer().stage_started('module_load')")
        first_other = assembled.index(b"types.ModuleType('target_safety.resource')")
        # When: the assembly order is read. Then: entry is the earliest bounded write
        # (a supervisor bundle embeds the guard bundle, so counts may exceed one).
        assert assembled.count(b"writer().entry()") >= 1, name
        assert entry < meta < opened < first_other, name
    print("PASS entry/meta writes precede every other module load in all four bundles")


def assembled_bundle_emits_entry_first() -> None:
    # Given: the real stdin loader with an empty tail.
    assembled = payload.bundle(payload.MODULES, "raise SystemExit(0)")
    result = subprocess.run([sys.executable, "-B", "-"], input=assembled,
                            capture_output=True, check=False, timeout=30)
    lines = [json.loads(line) for line in result.stdout.decode().splitlines()]
    # When: the bundle runs. Then: entry first (minimal), meta second, bracket pair next.
    assert result.returncode == 0, result.stderr
    assert set(lines[0]) == {"event", "pid", "monotonic_ns"} and lines[0]["event"] == "remote_entry"
    assert lines[1]["event"] == "remote_entry_meta" and lines[1]["seq"] == 1
    assert lines[2]["event"] == "stage_started" and lines[2]["data"] == {"phase": "module_load"}
    assert lines[3]["event"] == "stage_completed" and lines[3]["seq"] == 3
    assert [record.get("seq") for record in lines[1:]] == list(range(1, 4))
    print("PASS real stdin run emits entry first, meta second and the module_load pair")


def hardened_streams_complete_and_mode_bound() -> None:
    # Given: complete hardened streams carrying each mode's terminal result.
    sizing = stream("sizing_outcome", SIZING)
    ceiling = stream("ceiling_outcome", CEILING)
    # When: the acceptors validate them. Then: each mode accepts only its own.
    assert accepted_sizing_output(sizing), sizing
    assert not accepted_ceiling_output(sizing)
    assert accepted_ceiling_output(ceiling), ceiling
    assert not accepted_sizing_output(ceiling)
    print("PASS complete hardened streams validate terminal records in their own mode only")


def terminal_completeness_rejects_torn_streams() -> None:
    # Given: every way a hardened stream can lose its terminal completeness.
    full = stream("sizing_outcome", SIZING).splitlines()
    missing_transition = [line for line in full
                          if json.loads(line)["event"] != "egress_started"]
    no_entry = full[1:]
    outcome_not_last = stream("sizing_outcome", SIZING, extra_after_outcome=True)
    unbalanced = ['{"event": "remote_entry", "pid": 1, "monotonic_ns": 1}',
                  '{"event": "remote_entry_meta", "data": {}, "monotonic_ns": 1, "seq": 1}',
                  '{"event": "stage_started", "data": {"phase": "collection"},'
                  ' "monotonic_ns": 1, "seq": 2}',
                  '{"event": "cleanup_started", "data": null, "monotonic_ns": 1, "seq": 3}',
                  '{"event": "cleanup_completed", "data": null, "monotonic_ns": 1, "seq": 4}',
                  '{"event": "egress_started", "data": null, "monotonic_ns": 1, "seq": 5}',
                  json.dumps({**json.loads(full[-1]), "seq": 6})]
    gap = list(full)
    gap[2] = json.dumps({**json.loads(full[2]), "seq": 99})
    cases = {"missing egress_started": "\n".join(missing_transition),
             "entry missing": "\n".join(no_entry),
             "outcome not last": outcome_not_last,
             "open stage pair": "\n".join(unbalanced),
             "sequence gap": "\n".join(gap),
             "torn tail": "\n".join(full) + '\n{"event": "heartbeat",'}
    # When: acceptance runs. Then: every torn stream is rejected as incomplete.
    for reason, stdout in cases.items():
        assert not accepted_sizing_output(stdout), reason
    # Then: the legacy outcome-only stream keeps its exact acceptance semantics.
    assert accepted_sizing_output(wire("sizing_outcome", SIZING))
    assert not accepted_sizing_output(wire("sizing_outcome", SIZING) + "\nnot json")
    print(f"PASS terminal completeness rejects {len(cases)} torn forms; legacy outcome kept")


def child_brackets_identify_the_stall_interval() -> None:
    # Given: a complete hardened child transcript and its mid-watch truncation.
    out = io.StringIO()
    with redirect_stdout(out):
        writer = Writer(time.monotonic() + 60.0)
        writer.entry()
        writer.meta()
        writer.stage_started("module_load")
        writer.stage_completed("module_load")
        writer.stage_started("guard_init")
        writer.record(SIZING_READY_EVENT, CHILD_ADDRESS_SPACE_LIMIT_BYTES)
        writer.stage_completed("guard_init")
        writer.stage_started("watch_session")
        writer.stage_completed("watch_session")
        writer.record(SIZING_COMPLETION_EVENT, SIZING["sizing"])
    healthy = parse_sizing_child(out.getvalue())
    stalled = parse_sizing_child("\n".join(out.getvalue().splitlines()[:-2]))
    # Then: the full transcript validates; the stall lands inside an open interval.
    assert healthy.valid, healthy.reason
    assert not stalled.valid and stalled.reason == "diagnostics_incomplete", stalled.reason
    assert stalled.events[-1].event == "stage_started", stalled.events[-1]
    print("PASS child brackets validate fully; a stall localizes to the open watch_session interval")


def oracle_counterexamples_reject_hardened_streams() -> None:
    # Given: the Oracle counterexamples over a canonical hardened stream.
    full = stream("sizing_outcome", SIZING)
    records = [json.loads(line) for line in full.splitlines()]
    missing_final_lf = full[:-1]
    assert full.endswith("\n") and not missing_final_lf.endswith("\n")
    no_stage_brackets = [record for record in records
                         if record["event"] not in ("stage_started", "stage_completed")]
    meta = records[1]
    body = records[2:]
    meta_fourth_records = [records[0], body[0], body[1], meta, *body[2:]]
    for sequence, record in enumerate(meta_fourth_records[1:], 1):
        record["seq"] = sequence
    meta_fourth = "\n".join(json.dumps(record) for record in meta_fourth_records) + "\n"
    # When: the receiver validates them. Then: every counterexample is rejected.
    assert not accepted_sizing_output(missing_final_lf), "missing final LF accepted"
    assert not accepted_sizing_output("\n".join(json.dumps(r) for r in no_stage_brackets)
                                      + "\n"), "no stage brackets accepted"
    assert not accepted_sizing_output(meta_fourth), "metadata fourth accepted"
    # Then: the child parser rejects an LF-unterminated hardened child stream too.
    child = io.StringIO()
    with redirect_stdout(child):
        writer = Writer(time.monotonic() + 60.0)
        writer.entry()
        writer.meta()
        writer.stage_started("module_load")
        writer.stage_completed("module_load")
        writer.stage_started("guard_init")
        writer.record(SIZING_READY_EVENT, CHILD_ADDRESS_SPACE_LIMIT_BYTES)
        writer.stage_completed("guard_init")
        writer.stage_started("watch_session")
        writer.stage_completed("watch_session")
        writer.record(SIZING_COMPLETION_EVENT, SIZING["sizing"])
    assert parse_sizing_child(child.getvalue()).valid
    unterminated = parse_sizing_child(child.getvalue()[:-1])
    assert not unterminated.valid and unterminated.reason == "diagnostics_incomplete", unterminated.reason
    # Then: the legacy outcome-only stream keeps its exact acceptance semantics.
    assert accepted_sizing_output(wire("sizing_outcome", SIZING))
    print("PASS Oracle counterexamples (missing LF, no brackets, metadata fourth) rejected; legacy kept")


def _renumbered(selected: list[dict]) -> str:
    """One reassembled hardened transcript: entry unsequenced, seqs 1..N contiguous."""
    for sequence, record in enumerate(selected[1:], 1):
        record["seq"] = sequence
    return "\n".join(json.dumps(record) for record in selected) + "\n"


def round3_stage_order_and_entry_uniqueness_rejected() -> None:
    # Given: the round-3 Oracle order/uniqueness bypasses over a canonical
    # hardened stream (LF intact, seqs renumbered contiguous).
    records = [json.loads(line) for line in stream("sizing_outcome", SIZING).splitlines()]
    pairs = [records[2:4], records[4:6], records[6:8], records[8:10], records[10:12], records[12:14]]
    reversed_stages = [records[0], records[1],
                       *[record for pair in reversed(pairs) for record in pair],
                       *records[14:]]
    transitions_first = [records[0], records[1], *records[14:17], *records[2:14], *records[17:]]
    duplicate_entry = [dict(record) for record in records]
    duplicate_entry.insert(14, {**records[0], "seq": 14})
    cases = {"reversed stage pairs": _renumbered(reversed_stages),
             "cleanup/egress before all stage pairs": _renumbered(transitions_first),
             "duplicate remote_entry": _renumbered(duplicate_entry)}
    # When: both acceptors validate them. Then: every bypass is rejected.
    for reason, stdout in cases.items():
        assert not accepted_sizing_output(stdout), reason
        assert not accepted_ceiling_output(stdout.replace("sizing_outcome", "ceiling_outcome")
                                       .replace('\"topology-sizing\"', '\"ceiling\"')), "CR ceiling accepted"
    # Then: the canonical stream and the legacy outcome-only stream are unchanged.
    assert accepted_sizing_output(stream("sizing_outcome", SIZING))
    assert accepted_sizing_output(wire("sizing_outcome", SIZING))
    print("PASS round-3 order/uniqueness bypasses (reversed stages, early transitions, "
          "duplicate entry) rejected; canonical and legacy kept")


def round3_cr_separator_never_frames_a_record() -> None:
    # Given: a hardened transcript whose records are separated by bare CR and
    # whose raw still ends in LF (splitlines would frame it; LF framing must not).
    records = [json.loads(line) for line in stream("sizing_outcome", SIZING).splitlines()]
    cr_joined = "\r".join(json.dumps(record) for record in records) + "\n"
    cr_child = "\r".join([
        json.dumps({"event": "remote_entry", "pid": 1, "monotonic_ns": 1}),
        json.dumps({"event": "remote_entry_meta", "data": {}, "monotonic_ns": 1, "seq": 1}),
    ]) + "\n"
    # When: the receivers parse them. Then: CR-separated records are never framed.
    assert not accepted_sizing_output(cr_joined), "CR-separated supervisor stream accepted"
    assert not accepted_ceiling_output(cr_joined.replace("sizing_outcome", "ceiling_outcome")
                                       .replace('\"topology-sizing\"', '\"ceiling\"')), "CR ceiling accepted"
    child = parse_sizing_child(cr_child)
    assert not child.valid and child.reason == "malformed_or_truncated_line", child.reason
    # Then: the legacy outcome-only stream keeps its exact acceptance semantics.
    assert accepted_sizing_output(wire("sizing_outcome", SIZING))
    print("PASS CR-separated records never frame a record in either acceptor; legacy kept")


def round3_child_completion_chronology_enforced() -> None:
    # Given: a complete hardened child transcript and the round-3 chronology
    # bypasses: completion before module loading, and completion not last.
    out = io.StringIO()
    with redirect_stdout(out):
        writer = Writer(time.monotonic() + 60.0)
        writer.entry()
        writer.meta()
        writer.stage_started("module_load")
        writer.stage_completed("module_load")
        writer.stage_started("guard_init")
        writer.record(SIZING_READY_EVENT, CHILD_ADDRESS_SPACE_LIMIT_BYTES)
        writer.stage_completed("guard_init")
        writer.stage_started("watch_session")
        writer.stage_completed("watch_session")
        writer.record(SIZING_COMPLETION_EVENT, SIZING["sizing"])
    records = [json.loads(line) for line in out.getvalue().splitlines()]
    completion = next(record for record in records if record["event"] == SIZING_COMPLETION_EVENT)
    others = [record for record in records if record["event"] != SIZING_COMPLETION_EVENT]
    before_module_load = [*others[:2], dict(completion), *[dict(r) for r in others[2:]]]
    mid_watch = [*others[:7], dict(completion), *[dict(r) for r in others[7:]]]
    # When: the strict child parser validates them. Then: both are rejected and
    # the untouched transcript still validates.
    for reason, broken in (("completion before module loading", before_module_load),
                           ("completion not last", mid_watch)):
        parsed = parse_sizing_child(_renumbered(broken))
        assert not parsed.valid and parsed.reason == "child_chronology", (reason, parsed.reason)
    assert parse_sizing_child(out.getvalue()).valid
    # Then: legacy child transcripts keep their exact acceptance semantics.
    legacy = parse_sizing_child(transcript(ready(1), complete()))
    assert legacy.valid, legacy.reason
    print("PASS child completion-last and readiness/stage chronology enforced; legacy kept")


def main() -> int:
    entry_precedes_module_loading_in_every_bundle()
    assembled_bundle_emits_entry_first()
    hardened_streams_complete_and_mode_bound()
    terminal_completeness_rejects_torn_streams()
    child_brackets_identify_the_stall_interval()
    oracle_counterexamples_reject_hardened_streams()
    round3_stage_order_and_entry_uniqueness_rejected()
    round3_cr_separator_never_frames_a_record()
    round3_child_completion_chronology_enforced()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
