#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# /// script
# requires-python = ">=3.12"
# dependencies = []
# ///
# Run: python3 -B tests/protocol/audit_scenarios.py ROOT CASE
"""Mutation scenarios for the protobuf audit guard (tools/protocol/protobuf_audit.py).

Each case copies the repository inputs to a private temporary root, applies ONE
mutation and runs the guard there. Negative cases must exit 1 and print the
named violation code; ``baseline`` and ``repin-comment`` must pass.

Schema mutations come in two flavours:

* plain edits of a vendored ``.proto`` (the guard must fail on the blob pin);
* *re-pinned* edits, which also rewrite the fixture's upstream ``ls-tree``
  listing, its pinned sha256 (tool constant and provenance inventory) and
  regenerate the inventory. They simulate a deliberate, internally consistent
  upstream bump and prove that a real schema change still cannot pass unless a
  finding or an explicit accepted entry covers the new generated difference.
"""
from __future__ import annotations

import hashlib
import json
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import Callable, Final

AUDIT_SCRIPT: Final[str] = "tools/protocol/protobuf_audit.py"
TOOL_DIR: Final[str] = "tools/protocol"
INVENTORY: Final[str] = "docs/evidence/protobuf-audit-inventory.json"
REPORT: Final[str] = "docs/evidence/protobuf-audit-report.md"
FINDINGS: Final[str] = "docs/evidence/protobuf-audit-findings.json"
TRIAGE: Final[str] = "docs/evidence/protobuf-audit-triage.json"
MAPPING: Final[str] = "docs/evidence/protobuf-audit-mapping.json"
MATRIX: Final[str] = "docs/evidence/wire-conformance-matrix.json"
AASDK_TREE: Final[str] = "third_party/aasdk/protobuf/aap_protobuf"
OAA_TREE: Final[str] = ("third_party/reference/open-android-auto/"
                        "61eab61c5f9968154ff1a80faa8c0a427b208479/oaa")
COPY_DIRS: Final[tuple[str, ...]] = ("third_party", "docs", "src", "include", "tests", "tools/protocol")


def run(fixture: Path, *args: str) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [sys.executable, "-B", str(fixture / AUDIT_SCRIPT), "--root", str(fixture), *args],
        capture_output=True, text=True, check=False)


def load(fixture: Path, rel: str) -> object:
    return json.loads((fixture / rel).read_text(encoding="utf-8"))


def save(fixture: Path, rel: str, data: object) -> None:
    (fixture / rel).write_text(json.dumps(data, indent=1, sort_keys=True, ensure_ascii=True) + "\n",
                               encoding="utf-8")


def edit_text(path: Path, old: str, new: str) -> None:
    text = path.read_text(encoding="utf-8")
    if old not in text:
        raise SystemExit(f"scenario setup error: {old!r} not found in {path}")
    path.write_text(text.replace(old, new, 1), encoding="utf-8")


def repin(fixture: Path, tree: str, prefix: str, rel: str, mutate: Callable[[str], str],
          generate_must_pass: bool = True) -> None:
    """Edit a vendored .proto and make the fixture's pins consistent with the edit."""
    proto = fixture / tree / rel
    new_text = mutate(proto.read_text(encoding="utf-8"))
    data = new_text.encode("utf-8")
    proto.write_bytes(data)
    oid = hashlib.sha1(f"blob {len(data)}\0".encode() + data).hexdigest()
    listing = fixture / "third_party/provenance" / ("aasdk.ls-tree" if tree == AASDK_TREE else "oaa.ls-tree")
    old_sha = hashlib.sha256(listing.read_bytes()).hexdigest()
    lines = listing.read_text(encoding="utf-8").splitlines()
    target = prefix + rel
    for i, line in enumerate(lines):
        meta, _, path = line.partition("\t")
        if path == target:
            mode, kind, _oid, _size = meta.split()
            lines[i] = f"{mode} {kind} {oid} {len(data):>7}\t{path}"
            break
    else:
        raise SystemExit(f"scenario setup error: {target} not in listing")
    listing.write_text("\n".join(lines) + "\n", encoding="utf-8")
    new_sha = hashlib.sha256(listing.read_bytes()).hexdigest()
    for rel_file in (AUDIT_SCRIPT, "third_party/provenance/inventory.json"):
        p = fixture / rel_file
        p.write_text(p.read_text(encoding="utf-8").replace(old_sha, new_sha), encoding="utf-8")
    gen = run(fixture, "--generate")
    if generate_must_pass and gen.returncode != 0:
        raise SystemExit(f"scenario setup error: regenerate failed:\n{gen.stderr}")


def aasdk(fixture: Path, rel: str, mutate: Callable[[str], str], generate_must_pass: bool = True) -> None:
    repin(fixture, AASDK_TREE, "protobuf/aap_protobuf/", rel, mutate, generate_must_pass)


FOCUS = "service/media/video/message/VideoFocusReason.proto"
VFRN = "service/media/video/message/VideoFocusRequestNotification.proto"
MEDIA_STATUS = "shared/MessageStatus.proto"
SEQ = "service/control/message/ByeByeRequest.proto"


def sc_baseline(f: Path) -> str:
    return ""


def sc_drift_inventory(f: Path) -> str:
    data = load(f, INVENTORY)
    data["title"] = "drifted-title"  # type: ignore[index]
    save(f, INVENTORY, data)
    return "INVENTORY_DRIFT"


def sc_report_drift(f: Path) -> str:
    (f / REPORT).write_text((f / REPORT).read_text(encoding="utf-8") + "\nstale edit\n", encoding="utf-8")
    return "REPORT_DRIFT"


def sc_missing_file(f: Path) -> str:
    next((f / AASDK_TREE).rglob("*.proto")).unlink()
    return "MISSING_PROTO_FILE"


def sc_tampered_proto(f: Path) -> str:
    # Edit a vendored schema and regenerate WITHOUT touching the pins: generate refuses,
    # and --check fails on the blob pin.
    proto = f / AASDK_TREE / FOCUS
    proto.write_text(proto.read_text(encoding="utf-8") + "\n// tampered\n", encoding="utf-8")
    gen = run(f, "--generate")
    if gen.returncode == 0:
        raise SystemExit("FAIL: --generate accepted a tree that violates its pins")
    return "BLOB_PIN_MISMATCH"


def sc_unpinned_file(f: Path) -> str:
    (f / AASDK_TREE / "service/Extra.proto").write_text('syntax="proto2";\nmessage Extra {}\n', encoding="utf-8")
    return "UNPINNED_PROTO_FILE"


def sc_listing_tampered(f: Path) -> str:
    listing = f / "third_party/provenance/oaa.ls-tree"
    listing.write_text(listing.read_text(encoding="utf-8") + "100644 blob " + "0" * 40 + "       1\tREADME.md\n",
                       encoding="utf-8")
    return "LS_TREE_SHA_MISMATCH"


def sc_repin_comment(f: Path) -> str:
    aasdk(f, FOCUS, lambda t: t + "\n// harmless comment\n")
    return ""  # a consistent re-pin of a non-semantic change passes


def sc_field_number_changed(f: Path) -> str:
    aasdk(f, VFRN, lambda t: t.replace("mode = 2;", "mode = 12;"))
    return "UNCOVERED_DIFFERENCE"


def sc_field_type_changed(f: Path) -> str:
    # varint -> fixed64 changes the wire type of InputReport.timestamp
    aasdk(f, "service/inputsource/message/InputReport.proto",
          lambda t: t.replace("required uint64 timestamp = 1;", "required fixed64 timestamp = 1;"))
    return "UNCOVERED_DIFFERENCE"


def sc_enum_value_removed(f: Path) -> str:
    aasdk(f, "service/control/message/ByeByeReason.proto",
          lambda t: re.sub(r"\n\s*\w+\s*=\s*5\s*;", "", t, count=1))
    return "UNCOVERED_DIFFERENCE"


def sc_new_unmatched_message(f: Path) -> str:
    aasdk(f, "service/control/message/ByeByeResponse.proto",
          lambda t: t + "\nmessage BrandNewUnmatchedProbe\n{\n    optional int32 value = 1;\n}\n")
    return "UNCOVERED_DIFFERENCE"


def sc_vanished_difference(f: Path) -> str:
    # Removing AASDK's deprecated field 1 makes the generated difference that D6 covers vanish.
    def drop(t: str) -> str:
        return re.sub(r"\n\s*optional int32 disp_channel_id = 1[^\n]*", "", t, count=1)
    aasdk(f, VFRN, drop)
    return "FINDING_DANGLING_DIFF"


def sc_unparsed_construct(f: Path) -> str:
    # keep the pins consistent so the parser, not the pin, is what must reject the file
    aasdk(f, FOCUS, lambda t: t + "\n@@@ stray\n", generate_must_pass=False)
    return "PARSE_ERROR"


def sc_finding_dangling_diff(f: Path) -> str:
    data = load(f, FINDINGS)
    data["findings"][0]["covers"].append("W-000000000000")  # type: ignore[index]
    save(f, FINDINGS, data)
    return "FINDING_DANGLING_DIFF"


def sc_uncovered_by_removal(f: Path) -> str:
    inv = load(f, INVENTORY)
    fdoc = load(f, FINDINGS)
    triage = load(f, TRIAGE)
    covered_by_finding = {c for item in fdoc["findings"] for c in item.get("covers", [])}  # type: ignore[index]
    target = next(
        d["id"] for d in inv["differences"]  # type: ignore[index]
        if d["class"] != "non-wire" and d["id"] not in covered_by_finding and d["id"] in triage["entries"]  # type: ignore[index]
    )
    del triage["entries"][target]  # type: ignore[index]
    save(f, TRIAGE, triage)
    return "UNCOVERED_DIFFERENCE"


def sc_duplicate_id(f: Path) -> str:
    data = load(f, FINDINGS)
    data["findings"].append(dict(data["findings"][0]))  # type: ignore[index]
    save(f, FINDINGS, data)
    return "DUPLICATE_FINDING_ID"


def sc_missing_field(f: Path) -> str:
    data = load(f, FINDINGS)
    del data["findings"][0]["disposition"]  # type: ignore[index]
    save(f, FINDINGS, data)
    return "INCOMPLETE_FINDING"


def sc_invalid_disposition(f: Path) -> str:
    data = load(f, FINDINGS)
    data["findings"][0]["disposition"] = "unauthorized-disposition-value"  # type: ignore[index]
    save(f, FINDINGS, data)
    return "INVALID_DISPOSITION"


def sc_status_mismatch(f: Path) -> str:
    data = load(f, FINDINGS)
    for item in data["findings"]:  # type: ignore[index]
        if item["disposition"] == "equivalent":
            item["status"] = "proposed"
            break
    save(f, FINDINGS, data)
    return "STATUS_DISPOSITION_MISMATCH"


def sc_unexplained_deletion(f: Path) -> str:
    # legacy finding removed from the findings file
    data = load(f, FINDINGS)
    data["findings"] = [d for d in data["findings"] if d["id"] != "D1-800A-NAME"]  # type: ignore[index]
    save(f, FINDINGS, data)
    return "FINDING_DELETED"


def sc_nonlegacy_deletion(f: Path) -> str:
    # a post-legacy finding is deleted AND the inventory is regenerated: must still fail
    data = load(f, FINDINGS)
    data["findings"] = [d for d in data["findings"] if d["id"] != "D30-AV-OVERLAYS-800D-8010"]  # type: ignore[index]
    data["implementation_children"] = [  # type: ignore[index]
        {**c, "linked_findings": [x for x in c["linked_findings"] if x != "D30-AV-OVERLAYS-800D-8010"]}
        for c in data["implementation_children"]]  # type: ignore[index]
    save(f, FINDINGS, data)
    run(f, "--generate")
    return "FINDING_DELETED"


def sc_legacy_altered(f: Path) -> str:
    data = load(f, MATRIX)
    data["discrepancies"][9]["summary"] = "rewritten text"  # type: ignore[index]
    save(f, MATRIX, data)
    run(f, "--generate")
    return "LEGACY_ALTERED"


def sc_citation_broken(f: Path) -> str:
    edit_text(f / "src/protocol/aasdk_adapter/AvSinkAdapter.cpp",
              "wire.set_status(shared::Config::STATUS_READY);", "wire.set_status(shared::Config::STATUS_WAIT);")
    return "CITATION_MISMATCH"


def sc_absence_stale(f: Path) -> str:
    (f / "src/protocol/aasdk_adapter/Planted.cpp").write_text("// 0x8013 now handled\n", encoding="utf-8")
    return "ABSENCE_CLAIM_STALE"


def sc_crosslink_dangling(f: Path) -> str:
    (f / "docs/evidence/note.md").write_text("See D77-DOES-NOT-EXIST for details.\n", encoding="utf-8")
    return "CROSSLINK_DANGLING"


def sc_orphan_finding(f: Path) -> str:
    data = load(f, FINDINGS)
    for c in data["implementation_children"]:  # type: ignore[index]
        c["linked_findings"] = [x for x in c["linked_findings"] if x != "D5-VF-REASON"]
    save(f, FINDINGS, data)
    return "CROSSLINK_ORPHAN"


def sc_mapping_dangling(f: Path) -> str:
    data = load(f, MAPPING)
    data["pairs"][0]["oaa"] = "oaa.proto.messages.DoesNotExist"  # type: ignore[index]
    save(f, MAPPING, data)
    return "MAPPING_DANGLING"


def sc_triage_missing_diff(f: Path) -> str:
    inv = load(f, INVENTORY)
    triage = load(f, TRIAGE)
    target = next(d["id"] for d in inv["differences"] if d["class"] == "non-wire")  # type: ignore[index]
    del triage["entries"][target]  # type: ignore[index]
    save(f, TRIAGE, triage)
    return "TRIAGE_MISSING_DIFF"


def sc_triage_dangling_diff(f: Path) -> str:
    triage = load(f, TRIAGE)
    triage["entries"]["W-000000000000"] = {  # type: ignore[index]
        "impact": "dangling probe",
        "disposition": "no-action",
        "phone_evidence_status": "not-needed",
        "rationale": "dangling probe",
    }
    save(f, TRIAGE, triage)
    return "TRIAGE_DANGLING_DIFF"


def sc_triage_invalid_disposition(f: Path) -> str:
    triage = load(f, TRIAGE)
    first_id = next(iter(triage["entries"]))  # type: ignore[index]
    triage["entries"][first_id]["disposition"] = "invalid-triage-disposition"  # type: ignore[index]
    save(f, TRIAGE, triage)
    return "TRIAGE_INVALID"


def sc_triage_missing_metadata(f: Path) -> str:
    triage = load(f, TRIAGE)
    first_id = next(iter(triage["entries"]))  # type: ignore[index]
    del triage["entries"][first_id]["severity"]  # type: ignore[index]
    save(f, TRIAGE, triage)
    return "TRIAGE_INVALID"


def sc_sentinel_false_match(f: Path) -> str:
    inv = load(f, INVENTORY)
    pairs = inv["matching"]["pairs"]  # type: ignore[index]
    for p in pairs:
        if (
            p["aasdk"] == "aap_protobuf.service.navigationstatus.message.NavigationNextTurnEvent.NextTurnEnum"
            and p["oaa"] == "oaa.proto.enums.ChannelType.Enum"
        ):
            raise SystemExit("FAIL: NextTurnEnum falsely matched with ChannelType.Enum")
    diff_ids = {d["id"] for d in inv["differences"]}  # type: ignore[index]
    if "W-9c966a940eba" in diff_ids:
        raise SystemExit("FAIL: false enum diff W-9c966a940eba still present")
    sys.path.insert(0, str(f / TOOL_DIR))
    import proto_compare as pc  # type: ignore[import-not-found]
    import proto_schema as ps  # type: ignore[import-not-found]

    set_a = ps.build_schema_set({
        "service/nav/TurnKind.proto": (
            'syntax = "proto2";\n'
            "package aap_protobuf.service.nav;\n"
            "enum TurnKind {\n"
            "  UNKNOWN = 0;\n"
            "  SLIGHT_TURN = 1;\n"
            "  U_TURN = 2;\n"
            "  ROUNDABOUT = 3;\n"
            "}\n"
        ),
        "service/nav/RouteHint.proto": (
            'syntax = "proto2";\n'
            "package aap_protobuf.service.nav;\n"
            "message RouteHint {\n"
            "  optional int32 unknown = 1;\n"
            "  optional int32 road_class = 2;\n"
            "}\n"
        ),
    })
    set_b = ps.build_schema_set({
        "common/SlotKind.proto": (
            'syntax = "proto2";\n'
            "package oaa.proto.enums;\n"
            "enum SlotKind {\n"
            "  UNKNOWN = 0;\n"
            "  AUDIO_SINK_GUIDANCE = 1;\n"
            "  AUDIO_SINK_MEDIA = 2;\n"
            "  VIDEO_SINK = 3;\n"
            "}\n"
        ),
        "common/SlotMeta.proto": (
            'syntax = "proto2";\n'
            "package oaa.proto.data;\n"
            "message SlotMeta {\n"
            "  optional int32 unknown = 1;\n"
            "  optional int32 slot_rank = 2;\n"
            "}\n"
        ),
    })
    matched = pc.match_sets(set_a, set_b, {"pairs": [], "no_counterpart": []})
    if matched["pairs"]:
        raise SystemExit(f"FAIL: sentinel-only synthetic schemas matched: {matched['pairs']}")
    return ""


def sc_genuine_rename_still_matches(f: Path) -> str:
    inv = load(f, INVENTORY)
    pair_map = {(p["aasdk"], p["oaa"]) for p in inv["matching"]["pairs"]}  # type: ignore[index]
    expected_pairs = (
        (
            "aap_protobuf.service.radio.message.HdAcquisionState",
            "oaa.proto.messages.LegacyHdAcquisitionState",
        ),
        (
            "aap_protobuf.service.sensorsource.message.SensorBatch",
            "oaa.proto.messages.SensorEventIndication",
        ),
        (
            "aap_protobuf.service.navigationstatus.message.NavigationNextTurnEvent",
            "oaa.proto.messages.NavigationTurnEvent",
        ),
    )
    for pair in expected_pairs:
        if pair not in pair_map:
            raise SystemExit(f"FAIL: genuine rename pair stopped matching: {pair}")
    sys.path.insert(0, str(f / TOOL_DIR))
    import proto_compare as pc  # type: ignore[import-not-found]
    import proto_schema as ps  # type: ignore[import-not-found]

    set_a = ps.build_schema_set({
        "service/nav/GuidanceStep.proto": (
            'syntax = "proto2";\n'
            "package aap_protobuf.service.nav;\n"
            "enum GuidanceAction {\n"
            "  UNKNOWN = 0;\n"
            "  DEPART = 1;\n"
            "  TURN_LEFT = 2;\n"
            "  MERGE = 3;\n"
            "}\n"
            "message GuidanceStep {\n"
            "  optional string road_name = 1;\n"
            "  optional int32 distance_meters = 2;\n"
            "  optional uint32 turn_angle = 3;\n"
            "}\n"
        ),
    })
    set_b = ps.build_schema_set({
        "nav/RouteStep.proto": (
            'syntax = "proto3";\n'
            "package oaa.proto.messages;\n"
            "enum RouteManeuver {\n"
            "  NONE = 0;\n"
            "  DEPART = 1;\n"
            "  TURN_LEFT = 2;\n"
            "  MERGE = 3;\n"
            "  EXIT_RAMP = 4;\n"
            "}\n"
            "message RouteManeuverStep {\n"
            "  string road_name = 1;\n"
            "  int32 distance_meters = 2;\n"
            "  uint32 turn_angle = 3;\n"
            "  string street_hint = 4;\n"
            "}\n"
        ),
    })
    matched = pc.match_sets(set_a, set_b, {"pairs": [], "no_counterpart": []})
    got = {(p["aasdk"], p["oaa"]) for p in matched["pairs"]}
    if ("aap_protobuf.service.nav.GuidanceAction", "oaa.proto.messages.RouteManeuver") not in got:
        raise SystemExit(f"FAIL: genuine renamed enum failed to match: {matched}")
    if ("aap_protobuf.service.nav.GuidanceStep", "oaa.proto.messages.RouteManeuverStep") not in got:
        raise SystemExit(f"FAIL: genuine renamed message failed to match: {matched}")
    return ""


SCENARIOS: Final[dict[str, Callable[[Path], str]]] = {
    "baseline": sc_baseline,
    "drift-inventory": sc_drift_inventory,
    "report-drift": sc_report_drift,
    "missing-file": sc_missing_file,
    "tampered-proto": sc_tampered_proto,
    "unpinned-file": sc_unpinned_file,
    "listing-tampered": sc_listing_tampered,
    "repin-comment": sc_repin_comment,
    "field-number-changed": sc_field_number_changed,
    "field-type-changed": sc_field_type_changed,
    "enum-value-removed": sc_enum_value_removed,
    "new-unmatched-message": sc_new_unmatched_message,
    "vanished-difference": sc_vanished_difference,
    "unparsed-construct": sc_unparsed_construct,
    "finding-dangling-diff": sc_finding_dangling_diff,
    "uncovered-by-removal": sc_uncovered_by_removal,
    "duplicate-id": sc_duplicate_id,
    "missing-field": sc_missing_field,
    "invalid-disposition": sc_invalid_disposition,
    "status-mismatch": sc_status_mismatch,
    "unexplained-deletion": sc_unexplained_deletion,
    "nonlegacy-deletion": sc_nonlegacy_deletion,
    "legacy-altered": sc_legacy_altered,
    "citation-broken": sc_citation_broken,
    "absence-stale": sc_absence_stale,
    "crosslink-dangling": sc_crosslink_dangling,
    "orphan-finding": sc_orphan_finding,
    "mapping-dangling": sc_mapping_dangling,
    "triage-missing-diff": sc_triage_missing_diff,
    "triage-dangling-diff": sc_triage_dangling_diff,
    "triage-invalid-disposition": sc_triage_invalid_disposition,
    "triage-missing-metadata": sc_triage_missing_metadata,
    "sentinel-false-match": sc_sentinel_false_match,
    "genuine-rename-still-matches": sc_genuine_rename_still_matches,
}


def main() -> int:
    if len(sys.argv) < 3:
        print("Usage: audit_scenarios.py ROOT CASE", file=sys.stderr)
        return 2
    root = Path(sys.argv[1]).resolve()
    case = sys.argv[2]
    if case not in SCENARIOS:
        print(f"Unknown scenario case: {case}", file=sys.stderr)
        return 2
    fixture = Path(tempfile.mkdtemp(prefix=f"audit-scenario-{case}-"))
    try:
        for d in COPY_DIRS:
            shutil.copytree(root / d, fixture / d, symlinks=True,
                            ignore=shutil.ignore_patterns("__pycache__"))
        shutil.copy2(root / "PROVENANCE.md", fixture / "PROVENANCE.md")
        expected = SCENARIOS[case](fixture)
        result = run(fixture, "--check")
        print(f"case={case} exit={result.returncode}")
        if result.stdout:
            print(result.stdout, end="")
        if result.stderr:
            print("\n".join(result.stderr.splitlines()[:12]))
        if expected:
            if result.returncode != 1:
                print(f"FAIL: expected returncode 1, got {result.returncode}", file=sys.stderr)
                return 1
            if expected not in result.stderr:
                print(f"FAIL: expected violation '{expected}' not reported", file=sys.stderr)
                return 1
        elif result.returncode != 0:
            print(f"FAIL: expected a passing guard, got returncode {result.returncode}", file=sys.stderr)
            return 1
        print(f"scenario-test: PASS case={case}")
        return 0
    finally:
        shutil.rmtree(fixture, ignore_errors=True)


if __name__ == "__main__":
    raise SystemExit(main())
