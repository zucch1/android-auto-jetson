#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# /// script
# requires-python = ">=3.12"
# dependencies = []
# ///
# Run: python3 -B tools/protocol/protobuf_audit.py [--root ROOT] [--generate | --check | --uncovered]
"""AASDK <-> OAA protobuf schema audit and CI guard (Issue #62 / parent #61).

What is mechanical here:
  * both pinned `.proto` trees are enumerated and checked against the untouched
    upstream Git listings (blob IDs), then parsed with a strict parser
    (``proto_schema``) into a structural model (messages, fields by number,
    enums, values, reserved numbers);
  * messages and enums of the two trees are paired (``proto_compare``) by an
    explicit human mapping table, then by name, then by unambiguous structure;
    every definition ends up matched, unmatched-one-side, undecided or (OAA
    ``Foo.Enum`` namespace idiom) wrapper - nothing is dropped;
  * for each pair a wire-level diff is computed; every difference has a
    content-derived stable ID.

What is human-curated (docs/evidence/protobuf-audit-findings.json): findings
that reference generated difference IDs, explicit "accepted, reason" entries,
the legacy D1-D24 review fields, implementation children. The guard fails when
a finding references a difference that no longer exists, when a wire-affecting
(or unmatched/undecided) difference is covered by nothing, when a pinned
vendored file changed without a provenance re-pin, when a finding id vanishes,
or when generated outputs drift.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import re
import sys
from pathlib import Path
from typing import Any, Final

sys.path.insert(0, str(Path(__file__).resolve().parent))
import proto_compare as pc  # noqa: E402
import proto_schema as ps  # noqa: E402
import proto_triage as pt  # noqa: E402

SCHEMA_VERSION: Final[str] = "aa-protobuf-audit-2"

AASDK_PIN: Final[str] = "9bf6adf933665dee26532201719fac14a047ccf1"
AASDK_TREE: Final[str] = "19fbb172a1f7390e4f6618018029de8b7f10c3b0"
AASDK_URL: Final[str] = "https://github.com/opencardev/aasdk.git"
AASDK_DEST: Final[str] = "third_party/aasdk"
AASDK_PREFIX: Final[str] = "protobuf/aap_protobuf/"
AASDK_EXPECTED_PROTOS: Final[int] = 254
AASDK_LS_SHA256: Final[str] = "9327631eeae914a72eee8aa59aa639d07ec655b7b2e1fd90a4aafd367fd4521a"

OAA_PIN: Final[str] = "61eab61c5f9968154ff1a80faa8c0a427b208479"
OAA_TREE: Final[str] = "332f633e14dc23bbe4227eb3bde6242d0878fb00"
OAA_URL: Final[str] = "https://github.com/mrmees/open-android-auto.git"
OAA_DEST: Final[str] = "third_party/reference/open-android-auto/61eab61c5f9968154ff1a80faa8c0a427b208479"
OAA_PREFIX: Final[str] = "oaa/"
OAA_EXPECTED_PROTOS: Final[int] = 248
OAA_LS_SHA256: Final[str] = "e5bbc9a26bf5350cdfa03e09edb831cd53fc1c78f4cd7a4030ecc5e5067f3730"

SOURCES: Final[dict[str, dict[str, Any]]] = {
    "aasdk": {"repo": AASDK_URL, "pin": AASDK_PIN, "tree": AASDK_TREE, "dest": AASDK_DEST,
              "prefix": AASDK_PREFIX, "expected": AASDK_EXPECTED_PROTOS, "ls_sha256": AASDK_LS_SHA256,
              "listing": "third_party/provenance/aasdk.ls-tree"},
    "oaa": {"repo": OAA_URL, "pin": OAA_PIN, "tree": OAA_TREE, "dest": OAA_DEST,
            "prefix": OAA_PREFIX, "expected": OAA_EXPECTED_PROTOS, "ls_sha256": OAA_LS_SHA256,
            "listing": "third_party/provenance/oaa.ls-tree"},
}

INVENTORY_PATH: Final[str] = "docs/evidence/protobuf-audit-inventory.json"
REPORT_PATH: Final[str] = "docs/evidence/protobuf-audit-report.md"
MAPPING_PATH: Final[str] = "docs/evidence/protobuf-audit-mapping.json"
FINDINGS_PATH: Final[str] = "docs/evidence/protobuf-audit-findings.json"
TRIAGE_PATH: Final[str] = "docs/evidence/protobuf-audit-triage.json"
REGISTRY_PATH: Final[str] = "docs/evidence/protobuf-audit-finding-registry.json"
LEGACY_MATRIX_PATH: Final[str] = "docs/evidence/wire-conformance-matrix.json"
PROVENANCE_INVENTORY_PATH: Final[str] = "third_party/provenance/inventory.json"

ALLOWED_DISPOSITIONS: Final[frozenset[str]] = frozenset({
    "equivalent", "fixed", "confirmed-gap", "unverified", "intentionally-unsupported",
    "not-applicable", "incorrect-reference", "deferred",
})
ALLOWED_SEVERITIES: Final[frozenset[str]] = frozenset({
    "high", "medium-high", "medium", "low-medium", "low",
})
# Status follows the disposition (issue #62: "current status" next to "recommended
# disposition"). A finding whose disposition says "no gap" cannot still be "proposed".
DISPOSITION_STATUS: Final[dict[str, frozenset[str]]] = {
    "equivalent": frozenset({"resolved"}),
    "fixed": frozenset({"resolved"}),
    "incorrect-reference": frozenset({"resolved"}),
    "not-applicable": frozenset({"resolved"}),
    "intentionally-unsupported": frozenset({"resolved"}),
    "confirmed-gap": frozenset({"confirmed"}),
    "unverified": frozenset({"proposed"}),
    "deferred": frozenset({"deferred"}),
}
ALLOWED_STATUSES: Final[frozenset[str]] = frozenset().union(*DISPOSITION_STATUS.values())
ALLOWED_TIERS: Final[frozenset[str]] = frozenset({
    "mechanical",              # computed from the vendored .proto files by this tool
    "code-verified",           # confirmed against a cited line of this repo's source
    "oaa-claim-unverified",    # repeated from OAA comments/docs; not independently verified here
    "phone-trace-unverified",  # needs a real-phone capture
})
CLAIM_RESULTS: Final[frozenset[str]] = frozenset({
    "verified", "contradicted", "partly-verified", "unverifiable", "retracted-upstream",
})
# Difference classes that a finding or an accepted entry must cover.
MUST_COVER: Final[frozenset[str]] = frozenset({
    pc.CLASS_WIRE, pc.CLASS_INTERP, pc.CLASS_UNMATCHED, pc.CLASS_UNDECIDED,
})

LEGACY_IDS: Final[tuple[str, ...]] = (
    "D1-800A-NAME", "D2-8012-MISSING", "D3-UICFG-PAYLOAD", "D4-SETUP-STATUS",
    "D5-VF-REASON", "D6-VF-FIELD1", "D7-UNDERFLOW-PAYLOAD", "D8-SDQ-45-SEMANTICS",
    "D9-SDR-F16", "D10-STATUS-EXTRA", "D11-AUDIOTYPE-NAME", "D12-ORDINAL-FORK",
    "D13-NAV14-NAME-REQ", "D14-SENTINEL-IDS", "D15-VERSION-RESP-LEN", "D16-SDP-ORDER",
    "D17-OPEN-DIR-CLAIM", "D18-CHANNELMAP-CTRL", "D19-OAA-02-FLAGS", "D20-VIDFOCUS-OAA-DOCS",
    "D21-AUDIOFOCUS-NAME", "D22-VOICE-NAME", "D23-SHUTDOWN-NAME", "D24-PING-OPTCANON",
)
FINDING_ID_RE: Final = re.compile(r"\bD\d{1,3}-(?:[A-Z0-9]+-)*[A-Z]{3,}[A-Z0-9]*(?:-[A-Z0-9]+)*\b")

REQUIRED_FINDING_FIELDS: Final[tuple[str, ...]] = (
    "id", "domain", "severity", "status", "disposition", "evidence_tier", "confidence",
    "version_gates", "receiver_impact", "current_behavior", "owner", "rationale", "claims",
)


# ---------------------------------------------------------------------------
# helpers
# ---------------------------------------------------------------------------

def git_blob_oid(data: bytes) -> str:
    return hashlib.sha1(f"blob {len(data)}\0".encode("ascii") + data).hexdigest()


def sha256_hex(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def canonical_json(obj: Any) -> str:
    return json.dumps(obj, sort_keys=True, ensure_ascii=True, separators=(",", ":"))


def dump_json(obj: Any) -> str:
    return json.dumps(obj, indent=1, sort_keys=True, ensure_ascii=True) + "\n"


def load_ls_tree(path: Path, prefix: str) -> dict[str, tuple[str, int]]:
    """Parse the untouched `git ls-tree -r -l` listing: path -> (blob oid, size)."""
    out: dict[str, tuple[str, int]] = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        meta, _, rel = line.partition("\t")
        parts = meta.split()
        if len(parts) != 4 or parts[1] != "blob":
            continue
        if rel.startswith(prefix) and rel.endswith(".proto"):
            out[rel] = (parts[2], int(parts[3]))
    return out


# ---------------------------------------------------------------------------
# source loading + pin enforcement (independent of the generated inventory)
# ---------------------------------------------------------------------------

def load_sources(root: Path) -> tuple[dict[str, dict[str, Any]], list[str]]:
    """Read both trees; return per-source file records and pin errors."""
    errors: list[str] = []
    result: dict[str, dict[str, Any]] = {}
    prov_inv: dict[str, Any] = {}
    prov_path = root / PROVENANCE_INVENTORY_PATH
    if prov_path.is_file():
        try:
            prov_inv = {c["name"]: c for c in json.loads(prov_path.read_text("utf-8"))["components"]}
        except (ValueError, KeyError, TypeError) as exc:
            errors.append(f"PROVENANCE_INVENTORY_UNREADABLE: {exc}")
    else:
        errors.append(f"PROVENANCE_INVENTORY_MISSING: {PROVENANCE_INVENTORY_PATH}")
    for name, src in SOURCES.items():
        listing_path = root / src["listing"]
        files: dict[str, dict[str, Any]] = {}
        texts: dict[str, str] = {}
        result[name] = {"files": files, "texts": texts}
        if not listing_path.is_file():
            errors.append(f"LS_TREE_MISSING: {src['listing']}")
            continue
        listing_bytes = listing_path.read_bytes()
        if sha256_hex(listing_bytes) != src["ls_sha256"]:
            errors.append(f"LS_TREE_SHA_MISMATCH: {src['listing']} no longer matches the sha256 "
                          f"pinned in tools/protocol/protobuf_audit.py")
        pinned_in_prov = prov_inv.get(name, {}).get("upstream_ls_tree_sha256")
        if pinned_in_prov is not None and pinned_in_prov != src["ls_sha256"]:
            errors.append(f"LS_TREE_SHA_MISMATCH: provenance inventory pins {name} listing "
                          f"{pinned_in_prov}, audit tool pins {src['ls_sha256']}")
        pinned = load_ls_tree(listing_path, src["prefix"])
        if len(pinned) != src["expected"]:
            errors.append(f"FILE_COUNT_MISMATCH: {name} listing has {len(pinned)} .proto entries, "
                          f"expected {src['expected']}")
        base = root / src["dest"]
        tree = base / src["prefix"].rstrip("/")
        on_disk: dict[str, Path] = {}
        if tree.is_dir():
            for p in sorted(tree.rglob("*.proto")):
                if p.is_symlink():
                    errors.append(f"SYMLINK_REJECTED: {p.relative_to(root).as_posix()}")
                    continue
                on_disk[p.relative_to(base).as_posix()] = p
        for rel in sorted(set(pinned) - set(on_disk)):
            errors.append(f"MISSING_PROTO_FILE: {name} {rel} is pinned upstream but absent on disk")
        for rel in sorted(set(on_disk) - set(pinned)):
            errors.append(f"UNPINNED_PROTO_FILE: {name} {rel} is on disk but not in the pinned upstream listing")
        for rel in sorted(set(on_disk) & set(pinned)):
            data = on_disk[rel].read_bytes()
            oid = git_blob_oid(data)
            want_oid, want_size = pinned[rel]
            if oid != want_oid or len(data) != want_size:
                errors.append(f"BLOB_PIN_MISMATCH: {name} {rel}: on-disk blob {oid} ({len(data)} bytes) "
                              f"!= pinned upstream {want_oid} ({want_size} bytes); a vendored schema "
                              f"can only change together with the provenance pin")
            tree_rel = rel[len(src["prefix"]):]
            try:
                text = data.decode("utf-8")
            except UnicodeDecodeError as exc:
                errors.append(f"PARSE_ERROR: {name} {rel}: not valid UTF-8 ({exc})")
                continue
            texts[tree_rel] = text
            files[tree_rel] = {
                "path": on_disk[rel].relative_to(root).as_posix(),
                "upstream_path": rel, "size": len(data), "blob_oid": oid,
                "sha256": sha256_hex(data), "pinned_blob_oid": want_oid,
                "ls_tree_verified": oid == want_oid and len(data) == want_size,
            }
    return result, errors


def parse_sets(sources: dict[str, dict[str, Any]]) -> tuple[dict[str, dict[str, Any]], list[str]]:
    errors: list[str] = []
    sets: dict[str, dict[str, Any]] = {}
    for name, src in sources.items():
        try:
            file_paths = {k: v["path"] for k, v in src["files"].items()}
            sets[name] = ps.build_schema_set(src["texts"], file_paths=file_paths)
        except ps.ProtoParseError as exc:
            where = SOURCES[name]["dest"] + "/" + SOURCES[name]["prefix"] + str(exc)
            errors.append(f"PARSE_ERROR: {name}: {where}")
    return sets, errors


# ---------------------------------------------------------------------------
# curated data
# ---------------------------------------------------------------------------

def read_json(root: Path, rel: str) -> tuple[Any, str | None]:
    path = root / rel
    if not path.is_file():
        return None, f"DATA_FILE_MISSING: {rel}"
    try:
        return json.loads(path.read_text(encoding="utf-8")), None
    except ValueError as exc:
        return None, f"DATA_FILE_MALFORMED: {rel}: {exc}"


def check_citation(root: Path, cite: dict[str, Any], where: str) -> list[str]:
    errs: list[str] = []
    path, lines, contains = cite.get("path"), cite.get("lines"), cite.get("contains")
    if not isinstance(path, str) or not isinstance(lines, str):
        return [f"CITATION_INVALID: {where}: citation needs string 'path' and 'lines'"]
    target = (root / path).resolve()
    try:
        target.relative_to(root.resolve())
    except ValueError:
        return [f"CITATION_INVALID: {where}: {path} escapes the repository"]
    if not target.is_file():
        return [f"CITATION_BROKEN: {where}: {path} does not exist"]
    m = re.fullmatch(r"(\d+)(?:-(\d+))?", lines)
    if not m:
        return [f"CITATION_INVALID: {where}: bad line range {lines!r}"]
    lo = int(m.group(1))
    hi = int(m.group(2) or lo)
    text_lines = target.read_text(encoding="utf-8", errors="replace").splitlines()
    if lo < 1 or hi < lo or hi > len(text_lines):
        return [f"CITATION_BROKEN: {where}: {path}:{lines} is outside the file "
                f"({len(text_lines)} lines)"]
    if contains:
        window = re.sub(r"\s+", " ", " ".join(text_lines[lo - 1:hi]))
        if re.sub(r"\s+", " ", contains) not in window:
            errs.append(f"CITATION_MISMATCH: {where}: {path}:{lines} does not contain {contains!r}")
    else:
        errs.append(f"CITATION_INVALID: {where}: {path}:{lines} has no 'contains' text to verify")
    return errs


def check_absence(root: Path, cite: dict[str, Any], where: str) -> list[str]:
    """An 'absent' claim: the regex must match nowhere under the listed paths."""
    pattern, paths = cite.get("absent_pattern"), cite.get("in")
    if not isinstance(pattern, str) or not isinstance(paths, list) or not paths:
        return [f"CITATION_INVALID: {where}: absence check needs 'absent_pattern' and 'in'"]
    rx = re.compile(pattern)
    errs: list[str] = []
    for rel in paths:
        base = root / rel
        files = [base] if base.is_file() else sorted(p for p in base.rglob("*") if p.is_file())
        if not files:
            errs.append(f"CITATION_BROKEN: {where}: nothing to search under {rel}")
        for p in files:
            for n, line in enumerate(p.read_text(encoding="utf-8", errors="replace").splitlines(), 1):
                if rx.search(line):
                    errs.append(f"ABSENCE_CLAIM_STALE: {where}: /{pattern}/ now matches "
                                f"{p.relative_to(root).as_posix()}:{n}")
    return errs


def iter_citations(finding: dict[str, Any]) -> list[tuple[str, dict[str, Any]]]:
    out: list[tuple[str, dict[str, Any]]] = []
    fid = finding.get("id", "?")
    for i, claim in enumerate(finding.get("claims", []) or []):
        if isinstance(claim, dict):
            for c in claim.get("evidence", []) or []:
                if isinstance(c, dict):
                    out.append((f"{fid} claim[{i}]", c))
    cb = finding.get("current_behavior")
    if isinstance(cb, dict):
        for c in cb.get("citations", []) or []:
            if isinstance(c, dict):
                out.append((f"{fid} current_behavior", c))
    return out


# ---------------------------------------------------------------------------
# inventory construction
# ---------------------------------------------------------------------------

def coverage_counts(match: dict[str, Any], sets: dict[str, dict[str, Any]]) -> dict[str, Any]:
    out: dict[str, Any] = {}
    und_a = {(u["kind"], x) for u in match["undecided"] for x in u["aasdk"]}
    in_a = {(p["kind"], p["aasdk"]) for p in match["pairs"]}
    in_b = {(p["kind"], p["oaa"]) for p in match["pairs"]}
    und_b = {(u["kind"], y) for u in match["undecided"] for y in u["oaa"]} - in_b
    und_a -= in_a
    for kind, key in (("message", "messages"), ("enum", "enums")):
        total_a, total_b = len(sets["aasdk"][key]), len(sets["oaa"][key])
        m = sum(1 for p in match["pairs"] if p["kind"] == kind)
        wa = len(match["wrappers"]["aasdk"]) if kind == "message" else 0
        wb = len(match["wrappers"]["oaa"]) if kind == "message" else 0
        ua = sum(1 for k, f in und_a if k == kind)
        ub = sum(1 for k, f in und_b if k == kind)
        # undecided entries on the OAA side that are also listed as only-OAA are counted once
        oa = sum(1 for e in match["aasdk_only"] if e["kind"] == kind)
        ob = sum(1 for e in match["oaa_only"] if e["kind"] == kind)
        out[kind] = {
            "aasdk_total": total_a, "oaa_total": total_b, "matched_pairs": m,
            "aasdk_only": oa, "oaa_only": ob,
            "aasdk_undecided": ua, "oaa_undecided": ub,
            "aasdk_wrappers": wa, "oaa_wrappers": wb,
            "aasdk_accounted": m + oa + ua + wa, "oaa_accounted": m + ob + ub + wb,
        }
    ev: dict[str, int] = {}
    for p in match["pairs"]:
        ev[p["evidence"]] = ev.get(p["evidence"], 0) + 1
    out["evidence_counts"] = dict(sorted(ev.items()))
    return out


def build_model_view(schema: dict[str, Any]) -> dict[str, Any]:
    msgs = {}
    for fqn, m in sorted(schema["messages"].items()):
        msgs[fqn] = {
            "file": m["file"], "line": m["line"], "syntax": m["syntax"],
            "fields": [{k: f[k] for k in ("number", "name", "cardinality", "presence", "type",
                                          "type_kind", "wire", "packed", "default", "oneof", "line")}
                       for f in m["fields"]],
            "reserved_ranges": m["reserved_ranges"], "reserved_names": m["reserved_names"],
            "extension_ranges": m["extension_ranges"],
            "annotations": m["annotations"],
        }
    enums = {}
    for fqn, e in sorted(schema["enums"].items()):
        enums[fqn] = {"file": e["file"], "line": e["line"],
                      "values": [{"name": v["name"], "number": v["number"], "line": v["line"]}
                                 for v in e["values"]],
                      "reserved_ranges": e["reserved_ranges"], "reserved_names": e["reserved_names"],
                      "annotations": e["annotations"]}
    return {"messages": msgs, "enums": enums}


def difference_summary(diffs: list[dict[str, Any]]) -> dict[str, Any]:
    by_class: dict[str, int] = {}
    by_kind: dict[str, dict[str, int]] = {}
    for d in diffs:
        by_class[d["class"]] = by_class.get(d["class"], 0) + 1
        by_kind.setdefault(d["class"], {})
        by_kind[d["class"]][d["kind"]] = by_kind[d["class"]].get(d["kind"], 0) + 1
    return {"by_class": dict(sorted(by_class.items())),
            "by_kind": {c: dict(sorted(k.items())) for c, k in sorted(by_kind.items())}}


def finding_coverage(
    diffs: list[dict[str, Any]], findings: list[dict[str, Any]], accepted: list[dict[str, Any]],
    triage_doc: dict[str, Any] | None = None,
) -> dict[str, Any]:
    cover: dict[str, list[str]] = {}
    for f in findings:
        for did in f.get("covers", []) or []:
            cover.setdefault(did, []).append(f["id"])
    for a in accepted:
        for did in a.get("covers", []) or []:
            cover.setdefault(did, []).append(a["id"])
    triage_entries = (triage_doc or {}).get("entries", {})
    if not isinstance(triage_entries, dict):
        triage_entries = {}
    must = [d for d in diffs if d["class"] in MUST_COVER]
    uncovered = [
        d["id"] for d in must
        if d["id"] not in cover and d["id"] not in triage_entries
    ]
    out: dict[str, Any] = {
        "must_cover_total": len(must),
        "covered_by_finding": sum(1 for d in must if any(not i.startswith("ACC-") for i in cover.get(d["id"], []))),
        "covered_only_by_triage": sum(
            1 for d in must
            if d["id"] in triage_entries and not any(not i.startswith("ACC-") for i in cover.get(d["id"], []))
        ),
        "covered_only_by_accepted": sum(
            1 for d in must if cover.get(d["id"]) and all(i.startswith("ACC-") for i in cover[d["id"]])),
        "uncovered": len(uncovered),
        "by_class": {},
    }
    for cls in sorted(MUST_COVER):
        sub = [d for d in must if d["class"] == cls]
        out["by_class"][cls] = {
            "total": len(sub),
            "covered_by_finding": sum(1 for d in sub if any(not i.startswith("ACC-") for i in cover.get(d["id"], []))),
            "covered_only_by_triage": sum(
                1 for d in sub
                if d["id"] in triage_entries and not any(not i.startswith("ACC-") for i in cover.get(d["id"], []))
            ),
            "covered_only_by_accepted": sum(
                1 for d in sub if cover.get(d["id"]) and all(i.startswith("ACC-") for i in cover[d["id"]])),
        }
    return out


def build_inventory(
    root: Path, sources: dict[str, dict[str, Any]], sets: dict[str, dict[str, Any]],
    mapping: dict[str, Any], findings_doc: dict[str, Any], legacy_doc: dict[str, Any],
    triage_doc: dict[str, Any] | None = None,
) -> tuple[dict[str, Any], dict[str, Any]]:
    match = pc.compare(sets["aasdk"], sets["oaa"], mapping)
    schema_view = {n: build_model_view(sets[n]) for n in sets}
    for n in sets:
        schema_view[n]["stats"] = {
            "files": len(sets[n]["files"]), "messages": len(sets[n]["messages"]),
            "enums": len(sets[n]["enums"]),
            "fields": sum(len(m["fields"]) for m in sets[n]["messages"].values()),
            "enum_values": sum(len(e["values"]) for e in sets[n]["enums"].values()),
        }
    diffs, triage_summary = pt.enrich_differences(
        root, match["diffs"], sets, triage_doc, match=match,
        findings_doc=findings_doc, schema_view=schema_view,
    )
    legacy_by_id = {d["id"]: d for d in legacy_doc.get("discrepancies", [])}
    findings = []
    for f in findings_doc.get("findings", []):
        item = dict(f)
        if f.get("legacy"):
            item["legacy_record"] = legacy_by_id.get(f["id"])
        findings.append(item)
    accepted = findings_doc.get("accepted", [])
    cov = coverage_counts(match, sets)
    sources_out: dict[str, Any] = {}
    for name, src in SOURCES.items():
        files = sources[name]["files"]
        sources_out[name] = {
            "repo": src["repo"], "pin": src["pin"], "tree": src["tree"],
            "destination": src["dest"], "path_prefix": src["prefix"],
            "listing": src["listing"], "listing_sha256": src["ls_sha256"],
            "proto_count": len(files), "expected_proto_count": src["expected"],
            "all_blob_ids_match_pinned_listing": all(f["ls_tree_verified"] for f in files.values()),
            "files": {k: {**v, **{"syntax": sets[name]["files"][k]["syntax"],
                                  "package": sets[name]["files"][k]["package"],
                                  "imports": sets[name]["files"][k]["imports"]}}
                      for k, v in sorted(files.items()) if name in sets},
        }
    inv = {
        "schema": SCHEMA_VERSION,
        "title": "AASDK-OAA protobuf schema audit: structural model, matching, generated differences, findings, derived triage",
        "issue": "https://github.com/zucch1/android-auto-jetson/issues/62",
        "parent_issue": "https://github.com/zucch1/android-auto-jetson/issues/61",
        "sources": sources_out,
        "schema_sets": schema_view,
        "matching": {
            "evidence_order": ["explicit-mapping", "name:relative-path", "name:simple",
                               "name:normalized", "propagated:field-position", "structure"],
            "thresholds": {
                "min_struct_fields": pc.MIN_STRUCT_FIELDS, "min_struct_values": pc.MIN_STRUCT_VALUES,
                "min_struct_score": pc.MIN_STRUCT_SCORE, "min_name_score": pc.MIN_NAME_SCORE,
                "min_fields_without_name_corroboration": pc.MIN_STRUCT_STRONG,
            },
            "coverage": cov,
            "pairs": match["pairs"], "undecided": match["undecided"],
            "aasdk_only": match["aasdk_only"], "oaa_only": match["oaa_only"],
            "wrappers": match["wrappers"],
        },
        "differences": diffs,
        "difference_summary": difference_summary(diffs),
        "triage_summary": triage_summary,
        "findings": findings,
        "accepted": accepted,
        "findings_coverage": finding_coverage(diffs, findings_doc.get("findings", []), accepted, triage_doc),
        "implementation_children": findings_doc.get("implementation_children", []),
    }
    return inv, match


# ---------------------------------------------------------------------------
# guard
# ---------------------------------------------------------------------------

def check_findings(
    root: Path, findings_doc: dict[str, Any], diffs: list[dict[str, Any]],
    legacy_doc: dict[str, Any], registry: dict[str, Any] | None,
    triage_doc: dict[str, Any] | None = None,
) -> list[str]:
    errors: list[str] = []
    diff_ids = {d["id"] for d in diffs}
    findings = findings_doc.get("findings", [])
    accepted = findings_doc.get("accepted", [])
    tombstones = findings_doc.get("retired", [])
    seen: set[str] = set()
    for f in findings:
        fid = f.get("id")
        if not fid:
            errors.append("INCOMPLETE_FINDING: finding without id")
            continue
        if fid in seen:
            errors.append(f"DUPLICATE_FINDING_ID: duplicate finding id '{fid}'")
        seen.add(fid)
        for req in REQUIRED_FINDING_FIELDS:
            if not f.get(req):
                errors.append(f"INCOMPLETE_FINDING: finding '{fid}' missing required field '{req}'")
        disp, sev, st = f.get("disposition"), f.get("severity"), f.get("status")
        if disp not in ALLOWED_DISPOSITIONS:
            errors.append(f"INVALID_DISPOSITION: finding '{fid}' has invalid disposition '{disp}'")
        if sev not in ALLOWED_SEVERITIES:
            errors.append(f"INVALID_SEVERITY: finding '{fid}' has invalid severity '{sev}'")
        if st not in ALLOWED_STATUSES:
            errors.append(f"INVALID_STATUS: finding '{fid}' has invalid status '{st}'")
        elif disp in DISPOSITION_STATUS and st not in DISPOSITION_STATUS[disp]:
            errors.append(f"STATUS_DISPOSITION_MISMATCH: finding '{fid}' has status '{st}' but "
                          f"disposition '{disp}' requires one of {sorted(DISPOSITION_STATUS[disp])}")
        if f.get("evidence_tier") not in ALLOWED_TIERS:
            errors.append(f"INVALID_EVIDENCE_TIER: finding '{fid}' has tier '{f.get('evidence_tier')}'")
        covers = f.get("covers", [])
        if not isinstance(covers, list):
            errors.append(f"INCOMPLETE_FINDING: finding '{fid}' covers must be a list")
            covers = []
        if not covers and not f.get("no_generated_difference_reason"):
            errors.append(f"INCOMPLETE_FINDING: finding '{fid}' covers no generated difference and "
                          f"gives no 'no_generated_difference_reason'")
        for did in covers:
            if did not in diff_ids:
                errors.append(f"FINDING_DANGLING_DIFF: finding '{fid}' references difference {did} "
                              f"which no longer exists")
        for i, claim in enumerate(f.get("claims", []) or []):
            if not isinstance(claim, dict) or not claim.get("text") or claim.get("result") not in CLAIM_RESULTS:
                errors.append(f"INCOMPLETE_FINDING: finding '{fid}' claim[{i}] needs 'text' and a valid 'result'")
                continue
            if claim["result"] in ("verified", "contradicted", "partly-verified") and not claim.get("evidence"):
                errors.append(f"INCOMPLETE_FINDING: finding '{fid}' claim[{i}] is '{claim['result']}' "
                              f"but cites no evidence")
        for where, cite in iter_citations(f):
            if "diff" in cite:
                if cite["diff"] not in diff_ids:
                    errors.append(f"FINDING_DANGLING_DIFF: {where} cites generated difference "
                                  f"{cite['diff']} which no longer exists")
            elif "absent_pattern" in cite:
                errors.extend(check_absence(root, cite, where))
            else:
                errors.extend(check_citation(root, cite, where))
        for rel in f.get("related", []) or []:
            if rel not in {x.get("id") for x in findings}:
                errors.append(f"CROSSLINK_DANGLING: finding '{fid}' relates to unknown finding '{rel}'")
    # accepted entries are prohibited once per-difference triage is complete
    if accepted:
        errors.append(
            f"ACCEPTED_BUCKET_NONEMPTY: {len(accepted)} broad accepted bucket(s) remain in "
            f"{FINDINGS_PATH}; every difference must be individually triaged in {TRIAGE_PATH}"
        )
    acc_seen: set[str] = set()
    for a in accepted:
        aid = a.get("id", "")
        if not aid.startswith("ACC-") or aid in acc_seen:
            errors.append(f"ACCEPTED_INVALID: accepted entry id {aid!r} must be unique and start with 'ACC-'")
        acc_seen.add(aid)
        if not a.get("reason") or not a.get("category") or not a.get("covers"):
            errors.append(f"ACCEPTED_INVALID: accepted entry {aid} needs 'category', 'reason' and 'covers'")
        for did in a.get("covers", []) or []:
            if did not in diff_ids:
                errors.append(f"FINDING_DANGLING_DIFF: accepted entry {aid} references difference "
                              f"{did} which no longer exists")
    # coverage: every wire-affecting / interpretation / unmatched / undecided difference is covered
    # by a finding and/or a triage entry
    triage_entries = (triage_doc or {}).get("entries", {})
    if not isinstance(triage_entries, dict):
        triage_entries = {}
    covered: set[str] = set()
    for f in findings:
        covered.update(f.get("covers", []) or [])
    for a in accepted:
        covered.update(a.get("covers", []) or [])
    for d in diffs:
        if d["class"] in MUST_COVER and d["id"] not in covered and d["id"] not in triage_entries:
            errors.append(f"UNCOVERED_DIFFERENCE: {d['id']} [{d['class']}/{d['kind']}] "
                          f"{d['aasdk'] or '-'} <-> {d['oaa'] or '-'} {d['key']}: covered by no finding "
                          f"and no triage entry")
    # legacy integrity: ids present, verbatim, pinned
    legacy_pins = findings_doc.get("legacy_pins", {})
    legacy_by_id = {d.get("id"): d for d in legacy_doc.get("discrepancies", [])}
    for lid in LEGACY_IDS:
        if lid not in seen:
            errors.append(f"FINDING_DELETED: legacy finding '{lid}' is missing from the findings")
        rec = legacy_by_id.get(lid)
        if rec is None:
            errors.append(f"LEGACY_SOURCE_MISSING: {lid} is missing from {LEGACY_MATRIX_PATH}")
            continue
        pin = legacy_pins.get(lid)
        if pin is None or sha256_hex(canonical_json(rec).encode("utf-8")) != pin:
            errors.append(f"LEGACY_ALTERED: {lid} in {LEGACY_MATRIX_PATH} no longer matches the pinned "
                          f"original text (sha256 pin in the findings file)")
    for f in findings:
        if f.get("id") in LEGACY_IDS and not f.get("legacy"):
            errors.append(f"LEGACY_FLAG_MISSING: {f['id']} must have legacy=true")
    # deletion check for ALL findings
    retired_ids = set()
    for t in tombstones:
        if not t.get("id") or not t.get("reason"):
            errors.append("RETIRED_INVALID: a retired-finding entry needs 'id' and 'reason'")
        else:
            retired_ids.add(t["id"])
        if t.get("id") in seen:
            errors.append(f"RETIRED_INVALID: '{t.get('id')}' is both live and retired")
    if registry is None:
        errors.append(f"REGISTRY_MISSING: {REGISTRY_PATH}")
    else:
        for rid in registry.get("finding_ids", []):
            if rid not in seen and rid not in retired_ids:
                errors.append(f"FINDING_DELETED: published finding '{rid}' is neither present nor "
                              f"retired with a reason")
        for fid in sorted(seen - set(registry.get("finding_ids", []))):
            errors.append(f"REGISTRY_STALE: finding '{fid}' is not in {REGISTRY_PATH}")
    # cross-links
    known = seen | retired_ids
    children = findings_doc.get("implementation_children", [])
    child_ids = [c.get("id") for c in children]
    if len(child_ids) != len(set(child_ids)):
        errors.append("DUPLICATE_CHILD_ID: implementation_children ids are not unique")
    linked: set[str] = set()
    for c in children:
        for fid in c.get("linked_findings", []):
            linked.add(fid)
            if fid not in seen:
                errors.append(f"CROSSLINK_DANGLING: child '{c.get('id')}' links unknown finding '{fid}'")
    for f in findings:
        if f.get("disposition") in ("confirmed-gap", "unverified", "deferred") and f["id"] not in linked:
            errors.append(f"CROSSLINK_ORPHAN: open finding '{f['id']}' ({f['disposition']}) is "
                          f"linked from no implementation child")
        for owner in [f.get("owner")]:
            if owner and owner not in child_ids and not str(owner).startswith("triage:"):
                errors.append(f"CROSSLINK_DANGLING: finding '{f['id']}' owner '{owner}' is neither a "
                              f"child id nor 'triage:<name>'")
    # every D-ID mentioned in curated data or docs must exist
    for rel in iter_text_files(root):
        text = (root / rel).read_text(encoding="utf-8", errors="replace")
        for m in set(FINDING_ID_RE.findall(text)):
            if m not in known and m not in LEGACY_IDS:
                errors.append(f"CROSSLINK_DANGLING: {rel} references finding id '{m}' which does not exist")
    return errors


def iter_text_files(root: Path) -> list[str]:
    out: list[str] = []
    for rel in (FINDINGS_PATH, MAPPING_PATH):
        if (root / rel).is_file():
            out.append(rel)
    docs = root / "docs"
    if docs.is_dir():
        for p in sorted(docs.rglob("*")):
            if p.is_file() and p.suffix in (".md", ".json") and "protobuf-audit-" not in p.name \
                    and p.name != "wire-conformance-matrix.json":
                out.append(p.relative_to(root).as_posix())
    return out


def _valid_disk_annotation_obj(ann: Any) -> bool:
    if not isinstance(ann, dict):
        return False
    for key in ("confidence", "wire_msg_ids", "directions", "version_gates"):
        if key not in ann:
            continue
        field = ann[key]
        if isinstance(field, dict):
            vals = field.get("values", [])
            if not isinstance(vals, list) or not all(isinstance(item, dict) and "value" in item for item in vals):
                return False
        elif not isinstance(field, list):
            return False
    return True


def _check_inventory_annotations(on_disk: Any, fresh: dict[str, Any]) -> list[str]:
    errs: list[str] = []
    if not isinstance(on_disk, dict):
        return ["INVENTORY_DRIFT: inventory root must be a JSON object"]
    disk_sets = on_disk.get("schema_sets")
    fresh_sets = fresh["schema_sets"]
    catalog_valid = True
    if not isinstance(disk_sets, dict):
        errs.append("ENTITY_ANNOTATION_MISMATCH: inventory schema_sets is missing or not an object")
        catalog_valid = False
    else:
        for side in ("aasdk", "oaa"):
            ds = disk_sets.get(side)
            fs = fresh_sets[side]
            if not isinstance(ds, dict):
                errs.append(f"ENTITY_ANNOTATION_MISMATCH: schema_sets.{side} is missing or not an object")
                catalog_valid = False
                continue
            for col in ("messages", "enums"):
                dc = ds.get(col)
                fc = fs[col]
                if not isinstance(dc, dict):
                    errs.append(f"ENTITY_ANNOTATION_MISMATCH: schema_sets.{side}.{col} is missing or not an object")
                    catalog_valid = False
                    continue
                for fqn, d_ent in dc.items():
                    if not isinstance(d_ent, dict) or not isinstance(d_ent.get("file"), str):
                        errs.append(
                            f"ENTITY_ANNOTATION_MISMATCH: {side}.{col}.{fqn} is malformed or missing string 'file'"
                        )
                        catalog_valid = False
                        continue
                    if "annotations" in d_ent and not _valid_disk_annotation_obj(d_ent["annotations"]):
                        errs.append(
                            f"ENTITY_ANNOTATION_MISMATCH: {side}.{col}.{fqn} has malformed 'annotations'"
                        )
                        catalog_valid = False
                    if col == "enums":
                        vals = d_ent.get("values")
                        if not isinstance(vals, list) or not all(
                            isinstance(v, dict)
                            and isinstance(v.get("number"), int)
                            and not isinstance(v.get("number"), bool)
                            and isinstance(v.get("name"), str)
                            for v in vals
                        ):
                            errs.append(
                                f"ENTITY_ANNOTATION_MISMATCH: {side}.enums.{fqn} has malformed 'values'"
                            )
                            catalog_valid = False
                for fqn, f_ent in fc.items():
                    d_ent = dc.get(fqn)
                    if not isinstance(d_ent, dict) or "annotations" not in d_ent:
                        errs.append(f"ENTITY_ANNOTATION_MISMATCH: {side}.{col}.{fqn} is missing 'annotations'")
                        catalog_valid = False
                        continue
                    if not isinstance(d_ent["annotations"], dict):
                        errs.append(f"ENTITY_ANNOTATION_MISMATCH: {side}.{col}.{fqn} 'annotations' is not an object")
                        catalog_valid = False
                        continue
                    if d_ent["annotations"] != f_ent["annotations"]:
                        errs.append(
                            f"ENTITY_ANNOTATION_MISMATCH: {side}.{col}.{fqn} annotations in inventory "
                            f"differ from parser output"
                        )

    derived_cat: dict[str, dict[str, Any]] | None = None
    if catalog_valid and isinstance(disk_sets, dict):
        try:
            derived_cat = pt._derive_channel_catalog(disk_sets)
        except (ValueError, KeyError, TypeError, AttributeError) as exc:
            errs.append(f"CHANNEL_ANNOTATION_MISMATCH: malformed schema_sets for channel catalog ({exc})")

    triage_summary = on_disk.get("triage_summary")
    if not isinstance(triage_summary, dict):
        errs.append("CHANNEL_ANNOTATION_MISMATCH: triage_summary is missing or not an object")
        return errs
    disk_pc = triage_summary.get("per_channel")
    if not isinstance(disk_pc, list):
        errs.append("CHANNEL_ANNOTATION_MISMATCH: triage_summary.per_channel is missing or not a list")
        return errs

    seen_channels: set[str] = set()
    for row in disk_pc:
        if not isinstance(row, dict) or not isinstance(row.get("channel"), str) or not row["channel"]:
            errs.append("CHANNEL_ANNOTATION_MISMATCH: malformed per_channel row")
            continue
        ch = row["channel"]
        if ch in seen_channels:
            errs.append(f"CHANNEL_ANNOTATION_MISMATCH: duplicate per_channel row for '{ch}'")
            continue
        seen_channels.add(ch)
        if derived_cat is not None:
            crec = derived_cat.get(ch, {})
            checks = (
                ("aasdk_entities", crec.get("aasdk_entities", 0)),
                ("oaa_entities", crec.get("oaa_entities", 0)),
                ("wire_msg_ids", sorted(crec.get("wire_msg_ids", set()))),
                ("directions", sorted(crec.get("directions", set()))),
                ("version_gates", sorted(crec.get("version_gates", set()))),
                ("oaa_confidence", sorted(crec.get("oaa_confidence", set()))),
            )
            for key, expected_val in checks:
                if row.get(key) != expected_val:
                    errs.append(
                        f"CHANNEL_ANNOTATION_MISMATCH: channel '{ch}' {key}={row.get(key)!r} "
                        f"disagrees with per-entity aggregation {expected_val!r}"
                    )
    if derived_cat is not None:
        for missing_ch in sorted(set(derived_cat) - seen_channels):
            errs.append(
                f"CHANNEL_ANNOTATION_MISMATCH: channel '{missing_ch}' present in per-entity data "
                f"is missing from per_channel summary"
            )
    return errs


def check_integrity(root: Path) -> list[str]:
    errors: list[str] = []
    sources, e1 = load_sources(root)
    errors += e1
    sets, e2 = parse_sets(sources)
    errors += e2
    mapping, e = read_json(root, MAPPING_PATH)
    if e:
        errors.append(e)
    findings_doc, e = read_json(root, FINDINGS_PATH)
    if e:
        errors.append(e)
    legacy_doc, e = read_json(root, LEGACY_MATRIX_PATH)
    if e:
        errors.append(e)
    triage_doc, e = read_json(root, TRIAGE_PATH)
    if e:
        errors.append(e)
    registry, _ = read_json(root, REGISTRY_PATH)
    if len(sets) != 2 or mapping is None or findings_doc is None or legacy_doc is None or triage_doc is None:
        return errors
    inv, match = build_inventory(root, sources, sets, mapping, findings_doc, legacy_doc, triage_doc)
    errors += match["errors"]
    errors += check_findings(root, findings_doc, match["diffs"], legacy_doc, registry, triage_doc)
    valid_child_ids = {c.get("id") for c in findings_doc.get("implementation_children", []) if c.get("id")}
    errors += pt.check_triage(
        triage_doc, match["diffs"], valid_child_ids=valid_child_ids, enriched=inv["differences"],
    )
    inv_path, rep_path = root / INVENTORY_PATH, root / REPORT_PATH
    if not inv_path.is_file():
        errors.append(f"INVENTORY_FILE_MISSING: {INVENTORY_PATH}")
    else:
        inv_text = inv_path.read_text(encoding="utf-8")
        try:
            on_disk_inv = json.loads(inv_text)
        except ValueError as exc:
            errors.append(f"INVENTORY_DRIFT: {INVENTORY_PATH} is not valid JSON ({exc})")
        else:
            errors += _check_inventory_annotations(on_disk_inv, inv)
        if inv_text != dump_json(inv):
            errors.append("INVENTORY_DRIFT: on-disk inventory does not match freshly regenerated audit "
                          "output (run --generate)")
    if not rep_path.is_file():
        errors.append(f"REPORT_FILE_MISSING: {REPORT_PATH}")
    else:
        try:
            fresh_report = render_report(inv)
        except (KeyError, TypeError, AttributeError) as exc:
            errors.append(f"REPORT_RENDER_FAILED: curated data is malformed ({exc!r})")
        else:
            if rep_path.read_text(encoding="utf-8") != fresh_report:
                errors.append("REPORT_DRIFT: on-disk report does not match freshly regenerated "
                              "report (run --generate)")
    return errors


# ---------------------------------------------------------------------------
# report
# ---------------------------------------------------------------------------

def _n(x: Any) -> str:
    return f"{x:,}"


def short(fqn: str) -> str:
    return (fqn.replace("aap_protobuf.", "").replace("oaa.proto.", "")) or "-"


def render_report(inv: dict[str, Any]) -> str:
    cov = inv["matching"]["coverage"]
    ds = inv["difference_summary"]
    fc = inv["findings_coverage"]
    ts = inv["triage_summary"]
    L: list[str] = []
    w = L.append
    w("# AASDK ↔ OAA protobuf schema audit (Issue #62, parent #61)\n")
    w("Generated by `python3 -B tools/protocol/protobuf_audit.py --generate`; checked by `--check` "
      "(CI job `ci/provenance-lint`). Do not edit by hand: every count, classification, channel mapping, "
      "wire-impact flag, and C++ usage citation below is derived from the vendored `.proto` files and "
      "receiver sources. Human decisions live in `protobuf-audit-mapping.json`, "
      "`protobuf-audit-findings.json`, and `protobuf-audit-triage.json`.\n")
    w("## Sources\n")
    w("| Set | Upstream pin | `.proto` files | Blob IDs match pinned `git ls-tree` |")
    w("|---|---|---:|---|")
    for name, s in inv["sources"].items():
        w(f"| {name.upper()} | `{s['repo']}` @ `{s['pin']}` | {s['proto_count']} / {s['expected_proto_count']} | "
          f"{'yes' if s['all_blob_ids_match_pinned_listing'] else '**NO**'} |")
    w("")
    w("## Structural model\n")
    w("| Set | Files | Messages (incl. nested) | Enums | Fields | Enum values |")
    w("|---|---:|---:|---:|---:|---:|")
    for name in ("aasdk", "oaa"):
        st = inv["schema_sets"][name]["stats"]
        w(f"| {name.upper()} | {st['files']} | {st['messages']} | {st['enums']} | {st['fields']} | {st['enum_values']} |")
    w("")
    w("## Matching coverage\n")
    w("Every message and enum of each set is in exactly one bucket. Evidence order: explicit human "
      "mapping, relative name, simple name, normalised name, field position inside an already "
      "matched parent, unambiguous structure (see the thresholds in the inventory). Pairs the "
      "evidence cannot decide are listed as *undecided*; none is guessed.\n")
    w("| Kind | Set | Total | Matched | Only here | Undecided | Wrapper | Accounted |")
    w("|---|---|---:|---:|---:|---:|---:|---:|")
    for kind in ("message", "enum"):
        c = cov[kind]
        w(f"| {kind} | AASDK | {c['aasdk_total']} | {c['matched_pairs']} | {c['aasdk_only']} | "
          f"{c['aasdk_undecided']} | {c['aasdk_wrappers']} | {c['aasdk_accounted']} |")
        w(f"| {kind} | OAA | {c['oaa_total']} | {c['matched_pairs']} | {c['oaa_only']} | "
          f"{c['oaa_undecided']} | {c['oaa_wrappers']} | {c['oaa_accounted']} |")
    w("")
    w("Matched pairs by evidence: " + ", ".join(f"`{k}` {v}" for k, v in cov["evidence_counts"].items()) + ".\n")
    w("*Wrapper* = the OAA `message Foo { enum Enum {…} }` namespace idiom: an empty message that is "
      "never used as a field type; its nested enum is matched like any other enum.\n")
    w("### Matcher heuristic boundaries & exceptions\n")
    w("- **Enums require non-zero, non-sentinel value-name corroboration (intended conservatism)**: step 4 "
      "(`structure`) rejects any enum candidate with zero shared non-zero, non-sentinel value names (`nso < 1`). "
      "Value `0` entries (regardless of name) and named sentinels (`UNKNOWN`, `NONE`, `UNSPECIFIED`, `INVALID`, etc.) "
      "never corroborate, and path/package similarity alone cannot pair renamed enums; genuine renames with zero "
      "shared non-zero names require `explicit-mapping` or `propagated:field-position`.")
    w("- **Small messages (`< 4` fields) require field-name corroboration**: step 4 rejects `< 4`-field messages "
      "unless they share at least one normalized non-sentinel field name at the same tag number (`nso >= 1`).")
    w("- **Large messages (`>= 4` fields) allow path+shape fallback (accepted heuristic limit)**: messages with "
      "`>= 4` fields, mutual-unique-best `(tag, wire)` Jaccard `>= 0.75`, and at least one shared normalized "
      "path/package token (`path_package_overlap >= 1`) match even when `nso == 0`. Accepted because genuine "
      "refactored renames (`NavigationNextTurnEvent <-> NavigationTurnEvent`, 6 fields; `SensorBatch <-> "
      "SensorEventIndication`, 22/26 `len` fields — the only 2 production pairs using this path) rely on it; "
      "unrelated same-domain messages with identical `(tag, wire)` shapes remain a documented false-positive "
      "boundary managed via `protobuf-audit-mapping.json`.")
    w("- **Name-collision floor (`MIN_NAME_SCORE = 0.25`)**: same-name pairs with structural Jaccard `< 0.25` "
      "are held in `undecided` rather than matched.")
    w("- **Field-position propagation floors**: `propagated:field-position` pairs unmatched child types at the "
      "same tag of an already-matched parent only when structural Jaccard is `>= 0.6` (messages) or `>= 0.5` (enums).\n")
    w("## Generated differences\n")
    w("| Class | Count | Meaning |")
    w("|---|---:|---|")
    meaning = {
        pc.CLASS_WIRE: "field/value present on one side only, wire type or cardinality differs, enum number "
                       "named differently (rename vs semantic change not mechanically decidable)",
        pc.CLASS_INTERP: "same bytes for in-range values, different interpretation (presence, default, "
                         "integer family, enum type, oneof, scale exponent)",
        pc.CLASS_NONWIRE: "renames, packed/unpacked, deprecated flag, syntax; no wire effect",
        pc.CLASS_UNMATCHED: "definition has no counterpart by mapping, name or structure",
        pc.CLASS_UNDECIDED: "candidate counterparts exist but evidence is tied or contradictory",
    }
    for cls in pc.ALL_CLASSES:
        w(f"| {cls} | {ds['by_class'].get(cls, 0)} | {meaning[cls]} |")
    w("")
    w("Differences by kind:\n")
    for cls in pc.ALL_CLASSES:
        kinds = ds["by_kind"].get(cls)
        if kinds:
            w(f"- **{cls}**: " + ", ".join(f"`{k}` {v}" for k, v in kinds.items()))
    w("")
    w("## Per-difference triage summary\n")
    w(f"Every difference across all classes (including `non-wire`) is individually triaged in "
      f"`protobuf-audit-triage.json` with mechanically derived classification, wire-impact flag, "
      f"severity, channel, source file/line/field, match evidence, and receiver C++ usage citations.\n")
    w(f"- **Total differences**: **{ts['total_differences']}** (triaged: **{ts['triaged']}**, untriaged: **{ts['untriaged']}**)")
    w(f"- **Wire-affecting**: **{ts['wire_affecting']}** (`class == wire-affecting`)")
    w(f"- **Used by receiver**: **{ts['used_by_receiver']}** (direct field/enum usage, strict-policy unknown-tag/enum rejection, or active entity)")
    w(f"- **Wire-affecting AND used by receiver**: **{ts['wire_and_used']}**")
    w("- **Dispositions**: " + ", ".join(f"`{k}` {v}" for k, v in ts["by_disposition"].items()))
    w("- **Statuses**: " + ", ".join(f"`{k}` {v}" for k, v in ts["by_status"].items()))
    w("- **Severities**: " + ", ".join(f"`{k}` {v}" for k, v in ts["by_severity"].items()))
    w("- **Owners**: " + ", ".join(f"`{k}` {v}" for k, v in ts["by_owner"].items()) + "\n")
    w(f"> **Severity derivation rule**: {ts['severity_rule']}\n")
    w("| Derived classification | Count | Wire-affecting | Used by receiver | Wire & used |")
    w("|---|---:|---:|---:|---:|")
    for cls_name, cnt in ts["by_classification"].items():
        sub = [d for d in inv["differences"] if d["classification"] == cls_name]
        w_cnt = sum(1 for d in sub if d["wire_affecting"])
        u_cnt = sum(1 for d in sub if d["used_by_receiver"])
        wu_cnt = sum(1 for d in sub if d["wire_affecting"] and d["used_by_receiver"])
        w(f"| `{cls_name}` | {cnt} | {w_cnt} | {u_cnt} | {wu_cnt} |")
    w("")
    w("## Receiver-usage scanner methodology & known limitations\n")
    w("`scan_receiver_usage()` in `tools/protocol/proto_triage.py` uses regex and alias matching "
      "(no C++ AST or compiler symbol resolution) over `src/protocol/` and `src/helper/bluetooth_bootstrap/` "
      "(plus `Bootstrap.proto`). Known approximation modes:\n")
    w("- **Shared accessor names across messages (false positive)**: field accessors are matched with "
      "`\\b(?:has_|set_|mutable_|add_|clear_)?<field>\\s*\\(` per translation unit once a message type is "
      "used, so identically named fields on two different used messages in the same file (for example "
      "`status`, `config`, or `channel_id`) can both be marked `direct-field` if one is accessed.")
    w("- **Block comments, `#if 0`, and dead helpers (false positive)**: `//` line comments are stripped "
      "before scanning, but `/* ... */` multi-line comments, disabled `#if`/`#ifdef` blocks, and uncalled "
      "helper functions within scanned `.cpp`/`.hpp` files are still matched as code.")
    w("- **Other C++ alias styles and indirect callers (false negative)**: only `namespace X = ...;` and "
      "single-target `using X = ...;` aliases in files with direct `.pb.h` includes are expanded; `using "
      "namespace ...;`, `typedef` aliases, deduced `auto`/template helpers, and transitive callers without "
      "a `.pb.h` include are not resolved.")
    w("- **Generated accessors outside the scanned prefixes (false negative)**: the field regex omits "
      "`*_size()`, `release_*()`, `allocated_*()`, and `oneof_case()` accessors, so a repeated field "
      "inspected only via `<field>_size()` would not match `direct-field`.")
    w("- **Files outside the scanned roots (false negative)**: only `src/protocol/` and "
      "`src/helper/bluetooth_bootstrap/` are scanned; proto usages elsewhere (for example `src/core/` or "
      "`tests/`) are intentionally excluded from runtime receiver-usage counts.")
    w("")
    w("## Finding coverage\n")
    w(f"Differences in `MUST_COVER` classes (`wire-affecting`, `interpretation`, `unmatched`, `undecided`): **{fc['must_cover_total']}**; "
      f"covered by a curated finding (`D1`–`D39`): **{fc['covered_by_finding']}**; covered individually by "
      f"`protobuf-audit-triage.json`: **{fc.get('covered_only_by_triage', 0)}**; covered only by legacy "
      f"broad `accepted` buckets: **{fc['covered_only_by_accepted']}** (retired; must be 0); "
      f"uncovered: **{fc['uncovered']}** (the guard fails if this is not 0).\n")
    w("| Class | Total | By finding | Individually triaged | Accepted buckets |")
    w("|---|---:|---:|---:|---:|")
    for cls, c in fc["by_class"].items():
        w(f"| {cls} | {c['total']} | {c['covered_by_finding']} | "
          f"{c.get('covered_only_by_triage', 0)} | {c['covered_only_by_accepted']} |")
    w("")
    w("## Per-channel summary\n")
    w("Derived mechanically from `.proto` paths/packages, receiver C++ sources (`src/protocol/`, "
      "`src/helper/bluetooth_bootstrap/`), and `protobuf-audit-triage.json`.\n")
    cls_cols = [
        ("naming-only", "naming"),
        ("field-name-difference", "fld-name"),
        ("field-added-one-side", "fld-add"),
        ("field-number-or-type-conflict", "fld-num/type"),
        ("optionality-difference", "opt"),
        ("enum-value-difference", "enum-val"),
        ("enum-value-conflict", "enum-conf"),
        ("message-missing-one-side", "msg-miss"),
        ("enum-missing-one-side", "enum-miss"),
        ("metadata-or-syntax", "meta/syn"),
        ("undecided-match", "undec"),
    ]
    w("| Channel | Diffs | " + " | ".join(lbl for _, lbl in cls_cols) +
      " | Wire-aff | Used | Wire&Used | no-action | follow-up | phone-ev | OAA-ret |")
    w("|---|---:|" + "|".join("---:" for _ in cls_cols) + "|---:|---:|---:|---:|---:|---:|---:|")
    for ch in ts["per_channel"]:
        bc = ch["by_classification"]
        bd = ch["by_disposition"]
        cls_cells = " | ".join(str(bc.get(k, 0)) for k, _ in cls_cols)
        w(f"| `{ch['channel']}` | {ch['total']} | {cls_cells} | "
          f"{ch['wire_affecting']} | {ch['used_by_receiver']} | {ch['wire_and_used']} | "
          f"{bd.get('no-action', 0)} | {bd.get('follow-up-issue-needed', 0)} | "
          f"{bd.get('needs-phone-evidence', 0)} | {ch['oaa_retracted_diffs']} |")
    w("")
    w("### Per-channel protocol metadata & receiver coverage\n")
    w("Channel-level metadata below is aggregated strictly from per-entity `annotations` in "
      "`schema_sets.<side>.messages|enums.<fqn>` (where each value records `proto_comment` "
      "`file:line` provenance and missing attributes are marked `status: \"unknown\"`).\n")
    w("| Channel | AASDK / OAA entities | Wire msg IDs (from .proto) | Directions | Version gates | OAA confidence | Active C++ files |")
    w("|---|---:|---|---|---|---|---|")
    for ch in ts["per_channel"]:
        ids = ", ".join(ch["wire_msg_ids"][:6]) + (" …" if len(ch["wire_msg_ids"]) > 6 else "")
        dirs = ", ".join(ch["directions"]) or "-"
        gates = ", ".join(ch["version_gates"][:4]) + (" …" if len(ch["version_gates"]) > 4 else "")
        conf = ", ".join(ch["oaa_confidence"]) or "-"
        files = ", ".join(f"`{Path(p).name}`" for p in ch["receiver_files"][:4]) + (
            f" (+{len(ch['receiver_files']) - 4})" if len(ch["receiver_files"]) > 4 else ""
        )
        w(f"| `{ch['channel']}` | {ch['aasdk_entities']} / {ch['oaa_entities']} | {ids or '-'} | "
          f"{dirs} | {gates or '-'} | {conf} | {files or '-'} |")
    w("")
    w("## Wire-affecting differences used by receiver\n")
    wu_diffs = sorted(
        (d for d in inv["differences"] if d["wire_affecting"] and d["used_by_receiver"]),
        key=lambda x: (pt.CHANNEL_ORDER.index(x["channel"]) if x["channel"] in pt.CHANNEL_ORDER else 999, x["id"]),
    )
    w(f"All **{len(wu_diffs)}** differences where `wire_affecting == true` AND `used_by_receiver == true` "
      f"(including strict `kStrictExtensionPolicy` unknown-tag/enum rejections and compile-time/runtime usages):\n")
    w("| ID | Channel | Severity | Classification | Usage mode | AASDK ↔ OAA | Key | Receiver citations | Disposition | Status | Owner | Phone evidence |")
    w("|---|---|---|---|---|---|---|---|---|---|---|---|")
    for d in wu_diffs:
        cites = ", ".join(f"`{Path(p).name}`" for p in d["receiver_citations"]) or "-"
        tr = d.get("triage") or {}
        disp = tr.get("disposition", "-")
        st = tr.get("status", "-")
        ow = tr.get("owner", "-")
        pev = tr.get("phone_evidence_status", "-")
        w(f"| `{d['id']}` | `{d['channel']}` | `{d['severity']}` | `{d['classification']}` | `{d['usage_mode']}` | "
          f"`{short(d['aasdk'])}` ↔ `{short(d['oaa'])}` | `{d['key']}` | {cites} | `{disp}` | `{st}` | `{ow}` | {pev} |")
    w("")
    w("## Wireless modes comparison\n")
    wm = ts["wireless_modes"]
    w("Wireless Android Auto uses two distinct protocol layers on top of or alongside Wired USB/TLS:\n")
    w(f"1. **BT RFCOMM Wireless Bootstrap (`wireless-bootstrap`)**: Out-of-band pairing/Wi-Fi credential "
      f"setup via `aap_protobuf.aaw.*` (`src/helper/bluetooth_bootstrap/Bootstrap.proto` & `Bootstrap.cpp`). "
      f"AASDK defines **{wm['aasdk_bootstrap_message_count']}** messages; OAA documents **{wm['oaa_bootstrap_message_count']}** "
      f"counterpart messages under `oaa/wifi/` (`WifiStartRequest`/`Response`, `WifiVersionRequest`/`Response`, "
      f"`WifiInfoRequest`/`Response`, `WifiConnectStatus`, `WifiPing*`, `WifiSetupInfo`). Generated diffs: "
      f"**{wm['wireless_bootstrap_diffs']}** (used by receiver: **{wm['wireless_bootstrap_used']}**). "
      f"**Critical unverified discrepancy**: in `aaw.WifiInfoResponse`, AASDK has tag 2=`password` / tag 3=`bssid` "
      f"whereas OAA (`apk_static`) has tag 2=`bssid` / tag 3=`passphrase` (`W-f24dea872f3b`, `W-749177e5f357` — "
      f"same string wire type, semantic swap; unknown, needs phone evidence); tag 4 `WifiSecurityMode` also differs "
      f"(AASDK dense `{', '.join(wm['aasdk_security_mode_values'][:6])} …` vs OAA `WifiSecurityMode.Enum` bitmask "
      f"`{', '.join(wm['oaa_security_mode_values'][:5])} …`, `W-7cb6ace88645`).")
    w(f"2. **Wi-Fi Projection GAL Channel (`wifi-projection`)**: In-band GAL service (`0x8001` `WifiCredentialsRequest`, "
      f"`0x8002` `WifiCredentialsResponse`) exercised in `src/protocol/aasdk_adapter/WifiProjectionAdapter.cpp` and "
      f"`MediaConfiguration.cpp`. Generated diffs: **{wm['wifi_projection_diffs']}** (used by receiver: "
      f"**{wm['wifi_projection_used']}**). See `D27-WIFI-CRED-STATUS` for `WifiCredentialsResponse` field 5 "
      f"(`access_point_type` vs `WifiCredentialStatus`) and security-mode encoding differences.")
    w(f"3. **Wired USB/TLS**: Does not use `wireless-bootstrap`; negotiates GAL control + active service channels directly over USB AoA + TLS. "
      f"OAA `WifiChannelType.Enum`: `{', '.join(wm['oaa_wifi_channel_types']) or '-'}`.\n")
    w("## OAA retracted & contradictory entities\n")
    retracted = ts["oaa_retracted_entities"]
    w(f"OAA explicitly marks **{len(retracted)}** schema entities `confidence: retracted` in its `.proto` comments "
      f"(e.g., wrong channel attribution such as `oaa/video/VideoFocusNotificationMessage.proto` mapped to `0x800E` "
      f"instead of `0x8008` `VideoFocusIndicationMessage.proto`, or unconfirmed `ConnectionConfiguration`). "
      f"Audited diffs touching retracted OAA definitions cannot be treated as normative without phone capture:\n")
    w("| Kind | Channel | OAA FQN | `.proto` file | Retraction note |")
    w("|---|---|---|---|---|")
    for r in retracted:
        note = r["note"].replace("|", "\\|")
        w(f"| {r['kind']} | `{r['channel']}` | `{short(r['fqn'])}` | `{r['file']}` | {note or '-'} |")
    w("")
    w("## Findings\n")
    disp: dict[str, int] = {}
    for f in inv["findings"]:
        d = f.get("disposition", "(missing)")
        disp[d] = disp.get(d, 0) + 1
    w(f"{len(inv['findings'])} findings (24 legacy `D1`–`D24` preserved verbatim in `legacy_record`; "
      f"review fields added separately). Dispositions: "
      + ", ".join(f"{k} {v}" for k, v in sorted(disp.items())) + ".\n")
    w("| ID | Legacy | Severity | Status | Disposition | Tier | Covers | Owner |")
    w("|---|---|---|---|---|---|---:|---|")
    for f in inv["findings"]:
        w(f"| `{f.get('id', '?')}` | {'yes' if f.get('legacy') else ''} | {f.get('severity', '?')} | "
          f"{f.get('status', '?')} | {f.get('disposition', '?')} | {f.get('evidence_tier', '?')} | "
          f"{len(f.get('covers', []))} | {f.get('owner', '?')} |")
    w("")
    w("## Undecided matches\n")
    if not inv["matching"]["undecided"]:
        w("None.\n")
    for u in inv["matching"]["undecided"]:
        w(f"- {u['kind']} `{', '.join(short(x) for x in u['aasdk'])}` ↔ "
          f"`{', '.join(short(x) for x in u['oaa'][:4])}{' …' if len(u['oaa']) > 4 else ''}`: {u['reason']}")
    w("")
    w("## Follow-up grouping (5 domain groups: confirmed fix needed vs research needed)\n")
    w("All actionable / pending differences (`follow-up-issue-needed` + `needs-phone-evidence`) are grouped into "
      "the five domain groups below rather than creating one issue per difference. Within each group, items "
      "with **confirmed fix needed** (`status = confirmed`) are separated from **research needed** "
      "(`status = deferred | proposed`, spanning schema/capability research and real-phone evidence).\n")
    w("| Domain group | Confirmed fix needed | Research needed (schema + phone) | Total follow-up/research |")
    w("|---|---:|---:|---:|")
    for grp in ts["follow_up_groups"]:
        cf = grp["confirmed_fix_needed"]
        rn = grp["research_needed"]
        w(f"| `{grp['child_id']}` | {cf['count']} | {rn['count']} ({rn['schema_research_count']} schema + {rn['phone_evidence_count']} phone) | {grp['total_diffs']} |")
    w("")
    for grp in ts["follow_up_groups"]:
        w(f"### {grp['child_id']}: {grp['title']}\n")
        cf = grp["confirmed_fix_needed"]
        rn = grp["research_needed"]
        if cf["count"] == 0:
            w("- **Confirmed fix needed (`0` diffs)**: none yet — all open items in this domain require research or phone verification first.")
        else:
            f_str = ", ".join(f"`{r['id']}` ({r['diff_count']})" for r in cf["findings"])
            u_str = ", ".join(f"`{x}`" for x in cf["unfound_diff_ids"])
            parts = [p for p in (f"findings: {f_str}" if f_str else "", f"strict receiver diffs: {u_str}" if u_str else "") if p]
            w(f"- **Confirmed fix needed (`{cf['count']}` diffs)**: " + "; ".join(parts) + ".")
        rf_str = ", ".join(f"`{r['id']}` ({r['status']}, {r['diff_count']})" for r in rn["findings"])
        ru_str = ", ".join(f"`{ch}` ({cnt})" for ch, cnt in rn["unfound_by_channel"].items())
        r_parts = [p for p in (f"findings: {rf_str}" if rf_str else "", f"channel clusters: {ru_str}" if ru_str else "") if p]
        w(f"- **Research needed (`{rn['count']}` diffs = `{rn['schema_research_count']}` schema/deferred + `{rn['phone_evidence_count']}` needs-phone-evidence)**: "
          + ("; ".join(r_parts) if r_parts else "none") + ".\n")
    w("## Implementation children (prioritized, checked against open issues)\n")
    for c in inv["implementation_children"]:
        w(f"### {c['id']}: {c['title']}\n")
        if c.get("priority"):
            w(f"- **Priority**: {c['priority']}")
        if c.get("existing_issue_overlap"):
            w(f"- **Existing open-issue check**: {c['existing_issue_overlap']}")
        w(f"- **Scope**: {c['scope']}")
        w("- **Linked findings**: " + ", ".join(f"`{x}`" for x in c["linked_findings"]) + "\n")
        w("Acceptance criteria:\n")
        for a in c["acceptance_criteria"]:
            w(f"- {a}")
        w("")
    return "\n".join(L) + "\n"


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--root", type=Path, default=None, help="repository root")
    g = ap.add_mutually_exclusive_group()
    g.add_argument("--generate", action="store_true", help="regenerate inventory JSON and report")
    g.add_argument("--check", action="store_true", help="verify pins, parse, matching, findings, triage, drift (default)")
    g.add_argument("--uncovered", action="store_true", help="list differences no finding/triage/accepted entry covers")
    args = ap.parse_args(argv)
    root = (args.root or Path.cwd()).resolve()
    if not (root / "PROVENANCE.md").is_file():
        for parent in root.parents:
            if (parent / "PROVENANCE.md").is_file():
                root = parent
                break

    if args.generate or args.uncovered:
        sources, e1 = load_sources(root)
        sets, e2 = parse_sets(sources)
        pin_errors = e1 + e2
        if pin_errors:
            print("protobuf-audit: refusing to generate from a tree that fails its pins:", file=sys.stderr)
            for e in pin_errors:
                print(f"  {e}", file=sys.stderr)
            return 1
        mapping, em = read_json(root, MAPPING_PATH)
        fdoc, ef = read_json(root, FINDINGS_PATH)
        ldoc, el = read_json(root, LEGACY_MATRIX_PATH)
        tdoc, et = read_json(root, TRIAGE_PATH)
        for e in (em, ef, el, et):
            if e:
                print(f"protobuf-audit: {e}", file=sys.stderr)
                return 1
        inv, match = build_inventory(root, sources, sets, mapping, fdoc, ldoc, tdoc)
        if match["errors"]:
            for e in match["errors"]:
                print(f"  {e}", file=sys.stderr)
            return 1
        if args.uncovered:
            cov = set()
            for f in fdoc["findings"]:
                cov.update(f.get("covers", []))
            for a in fdoc.get("accepted", []):
                cov.update(a.get("covers", []))
            triage_keys = set((tdoc or {}).get("entries", {}).keys())
            n = 0
            for d in match["diffs"]:
                if (d["class"] in MUST_COVER and d["id"] not in cov and d["id"] not in triage_keys) or d["id"] not in triage_keys:
                    n += 1
                    print(f"{d['id']} {d['class']}/{d['kind']} {short(d['aasdk'])} <-> {short(d['oaa'])} {d['key']}")
            print(f"uncovered={n}")
            return 0
        (root / INVENTORY_PATH).write_text(dump_json(inv), encoding="utf-8")
        (root / REPORT_PATH).write_text(render_report(inv), encoding="utf-8")
        print(f"protobuf-audit: generated {INVENTORY_PATH} and {REPORT_PATH}")
        ts = inv["triage_summary"]
        print(f"  differences={len(match['diffs'])} {inv['difference_summary']['by_class']} "
              f"triaged={ts['triaged']}/{ts['total_differences']} wire={ts['wire_affecting']} "
              f"used={ts['used_by_receiver']} wire_used={ts['wire_and_used']} "
              f"dispositions={ts['by_disposition']} statuses={ts['by_status']} "
              f"severities={ts['by_severity']} uncovered={inv['findings_coverage']['uncovered']}")
        return 0

    errors = check_integrity(root)
    if errors:
        print(f"protobuf-audit-guard: FAIL violations={len(errors)}", file=sys.stderr)
        for err in errors[:200]:
            print(f"  {err}", file=sys.stderr)
        if len(errors) > 200:
            print(f"  ... {len(errors) - 200} more", file=sys.stderr)
        return 1
    inv = json.loads((root / INVENTORY_PATH).read_text(encoding="utf-8"))
    cov = inv["matching"]["coverage"]
    ts = inv["triage_summary"]
    print(
        f"protobuf-audit-guard: PASS files={inv['sources']['aasdk']['proto_count'] + inv['sources']['oaa']['proto_count']} "
        f"pairs={cov['message']['matched_pairs']}m+{cov['enum']['matched_pairs']}e "
        f"differences={len(inv['differences'])} triaged={ts['triaged']}/{ts['total_differences']} "
        f"wire={ts['wire_affecting']} used={ts['used_by_receiver']} wire_used={ts['wire_and_used']} "
        f"dispositions={ts['by_disposition']} statuses={ts['by_status']} severities={ts['by_severity']} "
        f"findings={len(inv['findings'])} uncovered={inv['findings_coverage']['uncovered']}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
