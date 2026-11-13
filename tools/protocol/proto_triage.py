# SPDX-License-Identifier: GPL-3.0-or-later
"""Derived mechanical triage & receiver C++ usage scanner for protobuf audit (Issue #62).

Computes per-difference attributes directly from the pinned .proto models and
receiver C++ sources (no hand-typed counts or classifications):
  * classification: mechanical category derived from diff AST kind
  * wire_affecting: whether bytes on the wire can differ (class == 'wire-affecting')
  * channel: protocol channel/service derived from .proto path & package
  * used_by_receiver + citations: scanned from src/protocol/ and
    src/helper/bluetooth_bootstrap/ (where generated protobuf types are used)
  * oaa_metadata: confidence, retraction flag, wire msg IDs, directions, version gates
    extracted from .proto comments via proto_schema

Human judgement lives solely in docs/evidence/protobuf-audit-triage.json
(disposition, status, owner, version_applicability, evidence_tier, confidence,
impact, phone_evidence_status, rationale) keyed by stable diff ID, with
severity derived mechanically from wire_affecting x used_by_receiver x classification.
"""
from __future__ import annotations

import re
from pathlib import Path
from typing import Any, Final

import proto_compare as pc

ALLOWED_TRIAGE_DISPOSITIONS: Final[frozenset[str]] = frozenset({
    "no-action",
    "follow-up-issue-needed",
    "needs-phone-evidence",
})
ALLOWED_SEVERITIES: Final[frozenset[str]] = frozenset({
    "high", "medium-high", "medium", "low-medium", "low",
})
ALLOWED_STATUSES: Final[frozenset[str]] = frozenset({
    "resolved", "confirmed", "proposed", "deferred",
})
ALLOWED_TIERS: Final[frozenset[str]] = frozenset({
    "mechanical",
    "code-verified",
    "oaa-claim-unverified",
    "phone-trace-unverified",
})
ALLOWED_CONFIDENCES: Final[frozenset[str]] = frozenset({
    "high", "medium", "low",
})
TRIAGE_DISPOSITION_STATUS: Final[dict[str, frozenset[str]]] = {
    "no-action": frozenset({"resolved"}),
    "needs-phone-evidence": frozenset({"proposed"}),
    "follow-up-issue-needed": frozenset({"confirmed", "deferred", "proposed"}),
}
REQUIRED_TRIAGE_FIELDS: Final[tuple[str, ...]] = (
    "severity",
    "status",
    "disposition",
    "owner",
    "version_applicability",
    "evidence_tier",
    "confidence",
    "impact",
    "phone_evidence_status",
    "rationale",
)

SEVERITY_RULE_DOC: Final[str] = (
    "Derived deterministically from wire_affecting x used_by_receiver x classification: "
    "(1) wire_affecting=True & used_by_receiver=True -> 'high' for "
    "field-number-or-type-conflict / field-added-one-side / enum-value-difference, "
    "'medium-high' for enum-value-conflict or others; "
    "(2) wire_affecting=True & used_by_receiver=False -> 'medium'; "
    "(3) wire_affecting=False & used_by_receiver=True -> 'medium' for "
    "field-number-or-type-conflict / optionality-difference / undecided-match, "
    "'low-medium' for field-name-difference / message-missing-one-side / enum-missing-one-side, "
    "'low' for naming-only / metadata-or-syntax; "
    "(4) wire_affecting=False & used_by_receiver=False -> 'low-medium' for "
    "field-number-or-type-conflict / optionality-difference / undecided-match, 'low' otherwise."
)


def derive_severity(wire_affecting: bool, used_by_receiver: bool, classification: str) -> str:
    """Compute per-difference severity from wire_affecting x used_by_receiver x classification."""
    if wire_affecting and used_by_receiver:
        if classification in {
            "field-number-or-type-conflict",
            "field-added-one-side",
            "enum-value-difference",
        }:
            return "high"
        return "medium-high"
    if wire_affecting and not used_by_receiver:
        return "medium"
    if not wire_affecting and used_by_receiver:
        if classification in {
            "field-number-or-type-conflict",
            "optionality-difference",
            "undecided-match",
        }:
            return "medium"
        if classification in {
            "field-name-difference",
            "message-missing-one-side",
            "enum-missing-one-side",
        }:
            return "low-medium"
        return "low"
    if classification in {
        "field-number-or-type-conflict",
        "optionality-difference",
        "undecided-match",
    }:
        return "low-medium"
    return "low"

CLASSIFICATION_MAP: Final[dict[str, str]] = {
    "field-rename-normalized": "naming-only",
    "enum-value-rename-normalized": "naming-only",
    "field-rename": "field-name-difference",
    "syntax": "metadata-or-syntax",
    "field-deprecated": "metadata-or-syntax",
    "field-packed": "metadata-or-syntax",
    "reserved-names": "metadata-or-syntax",
    "enum-allow-alias": "metadata-or-syntax",
    "field-only-aasdk": "field-added-one-side",
    "field-only-oaa": "field-added-one-side",
    "field-wire-type": "field-number-or-type-conflict",
    "field-type-kind": "field-number-or-type-conflict",
    "field-embedded-type": "field-number-or-type-conflict",
    "field-vs-reserved": "field-number-or-type-conflict",
    "field-cardinality": "field-number-or-type-conflict",
    "field-varint-integer-interpretation": "field-number-or-type-conflict",
    "field-enum-type": "field-number-or-type-conflict",
    "field-zigzag-vs-plain-varint": "field-number-or-type-conflict",
    "field-float-width-or-kind": "field-number-or-type-conflict",
    "field-fixed-width-interpretation": "field-number-or-type-conflict",
    "field-string-vs-bytes": "field-number-or-type-conflict",
    "field-scale-exponent": "field-number-or-type-conflict",
    "reserved-ranges": "field-number-or-type-conflict",
    "extension-ranges": "field-number-or-type-conflict",
    "field-presence": "optionality-difference",
    "field-default": "optionality-difference",
    "field-oneof-membership": "optionality-difference",
    "enum-value-only-aasdk": "enum-value-difference",
    "enum-value-only-oaa": "enum-value-difference",
    "enum-value-vs-reserved": "enum-value-difference",
    "enum-value-name-conflict": "enum-value-conflict",
    "message-only-aasdk": "message-missing-one-side",
    "message-only-oaa": "message-missing-one-side",
    "enum-only-aasdk": "enum-missing-one-side",
    "enum-only-oaa": "enum-missing-one-side",
    "undecided-match": "undecided-match",
}

CHANNEL_ORDER: Final[tuple[str, ...]] = (
    "control",
    "av-video",
    "av-audio",
    "av-mic",
    "av-shared",
    "input",
    "sensor",
    "bluetooth",
    "wifi-projection",
    "wireless-bootstrap",
    "navigation",
    "media-playback",
    "media-browser",
    "phone-status",
    "radio",
    "generic-notification",
    "car-control",
    "car-intent",
    "vendor-extension",
    "shared",
)


def derive_classification(diff: dict[str, Any]) -> str:
    kind = diff["kind"]
    if kind not in CLASSIFICATION_MAP:
        raise ValueError(f"Unmapped diff kind {kind!r} for {diff.get('id')}")
    return CLASSIFICATION_MAP[kind]


def _entity_record(
    side: str, fqn_csv: str, sets: dict[str, dict[str, Any]],
) -> dict[str, Any] | None:
    if not fqn_csv:
        return None
    first = fqn_csv.split(",")[0].strip()
    return sets[side]["messages"].get(first) or sets[side]["enums"].get(first)


def _channel_from_aasdk_file(path: str) -> str:
    if path.startswith(("channel/control/", "service/control/", "service/Service.proto")):
        return "control"
    if path.startswith("aaw/"):
        return "wireless-bootstrap"
    if path.startswith("shared/"):
        return "shared"
    if path.startswith("service/media/video/"):
        return "av-video"
    if path.startswith("service/media/source/"):
        return "av-mic"
    if path.startswith("service/media/sink/message/Key") or "KeyBinding" in path or "KeyCode" in path:
        return "input"
    if path.startswith("service/media/sink/message/Video"):
        return "av-video"
    if path.startswith(("service/media/sink/message/Audio", "service/media/shared/message/Audio")):
        return "av-audio"
    if path.startswith(("service/media/shared/", "service/media/sink/")):
        return "av-shared"
    if path.startswith("service/inputsource/"):
        return "input"
    if path.startswith("service/sensorsource/"):
        return "sensor"
    if path.startswith("service/bluetooth/"):
        return "bluetooth"
    if path.startswith("service/wifiprojection/"):
        return "wifi-projection"
    if path.startswith("service/navigationstatus/"):
        return "navigation"
    if path.startswith("service/mediaplayback/"):
        return "media-playback"
    if path.startswith("service/mediabrowser/"):
        return "media-browser"
    if path.startswith("service/phonestatus/"):
        return "phone-status"
    if path.startswith("service/radio/"):
        return "radio"
    if path.startswith("service/genericnotification/"):
        return "generic-notification"
    if path.startswith("service/vendorextension/"):
        return "vendor-extension"
    raise ValueError(f"Cannot derive channel from AASDK proto path {path!r}")


def _channel_from_oaa_file(path: str) -> str:
    if path.startswith(("control/", "verification/")):
        return "control"
    if path.startswith("common/"):
        return "shared"
    if path.startswith("generic/"):
        return "shared"
    if path.startswith("video/"):
        return "av-video"
    if path.startswith("mic/"):
        return "av-mic"
    if "AudioFocus" in path:
        return "control"
    if path.startswith("audio/"):
        return "av-audio"
    if "AndroidKeycode" in path:
        return "input"
    if "AVStreamType" in path:
        return "av-audio"
    if path.startswith("av/"):
        return "av-shared"
    if path.startswith("input/"):
        return "input"
    if path.startswith("sensor/"):
        return "sensor"
    if path.startswith("bluetooth/"):
        return "bluetooth"
    if path.startswith("wifi/"):
        if any(
            k in path
            for k in (
                "WifiStart", "WifiVersion", "WifiInfo", "WifiPing",
                "WifiConnect", "WifiSetup", "WifiChannelType", "WifiAccessPointType",
            )
        ):
            return "wireless-bootstrap"
        return "wifi-projection"
    if path.startswith("navigation/"):
        return "navigation"
    if path.startswith("media/"):
        if "BufferedMedia" in path:
            return "av-audio"
        return "media-playback"
    if path.startswith("mediabrowser/"):
        return "media-browser"
    if path.startswith("phone/"):
        return "phone-status"
    if path.startswith("radio/"):
        return "radio"
    if path.startswith("notification/"):
        return "generic-notification"
    if path.startswith("carcontrol/"):
        return "car-control"
    if path.startswith("carintent/"):
        return "car-intent"
    raise ValueError(f"Cannot derive channel from OAA proto path {path!r}")


def derive_channel(diff: dict[str, Any], sets: dict[str, dict[str, Any]]) -> str:
    aa_ent = _entity_record("aasdk", diff.get("aasdk", ""), sets)
    if aa_ent is not None:
        return _channel_from_aasdk_file(aa_ent["file"])
    oa_ent = _entity_record("oaa", diff.get("oaa", ""), sets)
    if oa_ent is not None:
        return _channel_from_oaa_file(oa_ent["file"])
    raise ValueError(f"Difference {diff.get('id')} has neither AASDK nor OAA entity")


def _field_or_value_line(
    diff: dict[str, Any], ent: dict[str, Any] | None,
) -> int | None:
    if ent is None:
        return None
    key = diff.get("key", "")
    if key.startswith("field "):
        try:
            num = int(key.split()[1])
        except (IndexError, ValueError):
            return ent.get("line")
        for f in ent.get("fields", []):
            if f.get("number") == num:
                return f.get("line") or ent.get("line")
    elif key.startswith("value "):
        try:
            num = int(key.split()[1])
        except (IndexError, ValueError):
            return ent.get("line")
        for v in ent.get("values", []):
            if v.get("number") == num:
                return v.get("line") or ent.get("line")
    return ent.get("line")


def derive_source_loc(diff: dict[str, Any], sets: dict[str, dict[str, Any]]) -> dict[str, Any]:
    aa_ent = _entity_record("aasdk", diff.get("aasdk", ""), sets)
    oa_ent = _entity_record("oaa", diff.get("oaa", ""), sets)
    return {
        "aasdk_file": aa_ent["file"] if aa_ent else "",
        "aasdk_line": _field_or_value_line(diff, aa_ent),
        "oaa_file": oa_ent["file"] if oa_ent else "",
        "oaa_line": _field_or_value_line(diff, oa_ent),
        "field_or_value": diff.get("key") or diff.get("entity_kind") or "",
    }


def derive_annotations(diff: dict[str, Any], sets: dict[str, dict[str, Any]]) -> dict[str, Any]:
    aa_ent = _entity_record("aasdk", diff.get("aasdk", ""), sets)
    oa_ent = _entity_record("oaa", diff.get("oaa", ""), sets)
    aa_ann = (aa_ent or {}).get("annotations", {})
    oa_ann = (oa_ent or {}).get("annotations", {})
    return {
        "aasdk_file": aa_ent["file"] if aa_ent else "",
        "aasdk_line": _field_or_value_line(diff, aa_ent),
        "oaa_file": oa_ent["file"] if oa_ent else "",
        "oaa_line": _field_or_value_line(diff, oa_ent),
        "oaa_confidence": oa_ann.get("confidence", []),
        "oaa_retracted": bool(oa_ann.get("retracted", False)),
        "oaa_retraction_note": oa_ann.get("retraction_note", ""),
        "wire_msg_ids": sorted(set(aa_ann.get("wire_msg_ids", [])) | set(oa_ann.get("wire_msg_ids", []))),
        "directions": sorted(set(aa_ann.get("directions", [])) | set(oa_ann.get("directions", []))),
        "version_gates": sorted(set(aa_ann.get("version_gates", [])) | set(oa_ann.get("version_gates", []))),
    }


# ---------------------------------------------------------------------------
# C++ receiver source scanner
# ---------------------------------------------------------------------------

_INCLUDE_PB_RE: Final = re.compile(r'#include\s+[<"]((?:aap_protobuf/[^>"]+|Bootstrap\.pb\.h))[>"]')
_NS_ALIAS_RE: Final = re.compile(r"\bnamespace\s+(\w+)\s*=\s*([A-Za-z0-9_:]+)\s*;")
_USING_ALIAS_RE: Final = re.compile(r"\busing\s+(\w+)\s*=\s*([A-Za-z0-9_:]+)\s*;")
_TAGS_ARRAY_RE: Final = re.compile(
    r"(?:constexpr\s+int\s+(\w+Tags|tags)\s*\[\s*\]\s*=\s*\{([^}]*)\}|parse\s*\(\s*(\w+)\s*,\s*payload\s*,\s*[^,]+,\s*(\{[^}]*\}|\w+)\s*\))"
)


def _expand_aliases(aliases: dict[str, str]) -> dict[str, str]:
    out = dict(aliases)
    for _ in range(5):
        changed = False
        for k, v in list(out.items()):
            head, sep, tail = v.partition("::")
            if head in out and head != k:
                nv = out[head] + (sep + tail if sep else "")
                if nv != v:
                    out[k] = nv
                    changed = True
        if not changed:
            break
    return out


def _cxx_to_dot(sym: str) -> str:
    return sym.replace("::", ".").lstrip(".")


def _parse_int_set(text: str) -> set[int]:
    vals: set[int] = set()
    for tok in re.findall(r"-?\d+", text):
        vals.add(int(tok))
    return vals


def scan_receiver_usage(root: Path, sets: dict[str, dict[str, Any]]) -> dict[str, Any]:
    """Scan receiver C++ adapter & bootstrap sources for generated proto usage."""
    scan_paths: list[Path] = []
    for sub in ("src/protocol", "src/helper/bluetooth_bootstrap"):
        base = root / sub
        if base.is_dir():
            for p in sorted(base.rglob("*")):
                if p.is_file() and p.suffix in (".cpp", ".hpp", ".h"):
                    scan_paths.append(p)

    # Index AASDK entities by .proto file path and by FQN
    entities_by_file: dict[str, list[str]] = {}
    for fqn, m in sets["aasdk"]["messages"].items():
        entities_by_file.setdefault(m["file"], []).append(fqn)
    for fqn, e in sets["aasdk"]["enums"].items():
        entities_by_file.setdefault(e["file"], []).append(fqn)

    entity_files: dict[str, set[str]] = {}
    field_files: dict[tuple[str, str], set[str]] = {}
    enum_val_files: dict[tuple[str, str], set[str]] = {}
    strict_msg_tags: dict[str, tuple[set[int], set[str]]] = {}  # fqn -> (allowed_tags, files)
    bootstrap_entities: set[str] = set()

    bootstrap_proto = root / "src/helper/bluetooth_bootstrap/Bootstrap.proto"
    if bootstrap_proto.is_file():
        btxt = bootstrap_proto.read_text(encoding="utf-8", errors="replace")
        for fqn in (*sets["aasdk"]["messages"], *sets["aasdk"]["enums"]):
            if fqn.startswith("aap_protobuf.aaw."):
                short_name = fqn.rsplit(".", 1)[-1]
                if re.search(rf"\b(?:message|enum)\s+{re.escape(short_name)}\b", btxt):
                    bootstrap_entities.add(fqn)

    for path in scan_paths:
        rel = path.relative_to(root).as_posix()
        txt = path.read_text(encoding="utf-8", errors="replace")
        # Strip // line comments before symbol scanning so comment mentions don't create false positives
        code_lines = []
        for line in txt.splitlines():
            code_lines.append(re.sub(r"//.*$", "", line))
        code = "\n".join(code_lines)

        includes = _INCLUDE_PB_RE.findall(txt)
        if not includes:
            continue

        raw_aliases: dict[str, str] = {}
        for name, target in _NS_ALIAS_RE.findall(code):
            raw_aliases[name] = target
        for name, target in _USING_ALIAS_RE.findall(code):
            raw_aliases[name] = target
        aliases = _expand_aliases(raw_aliases)

        included_protos: set[str] = set()
        uses_bootstrap = False
        for inc in includes:
            if inc == "Bootstrap.pb.h":
                uses_bootstrap = True
            elif inc.startswith("aap_protobuf/") and inc.endswith(".pb.h"):
                included_protos.add(inc[len("aap_protobuf/"):-len(".pb.h")] + ".proto")

        # Resolve all C++ qualified names (foo::bar or foo::bar::BAZ) into dotted FQNs
        resolved_tokens: list[str] = []
        for tok in re.findall(r"\b[A-Za-z_][A-Za-z0-9_]*(?:::[A-Za-z_][A-Za-z0-9_]*)+\b", code):
            head, sep, rest = tok.partition("::")
            if head in aliases:
                resolved_tokens.append(_cxx_to_dot(aliases[head] + sep + rest))
            else:
                resolved_tokens.append(_cxx_to_dot(tok))
        # Handle 'aaw::Foo' in Bootstrap.cpp -> 'aap_protobuf.aaw.Foo'
        if uses_bootstrap:
            for m in re.finditer(r"\baaw::([A-Za-z0-9_:]+)\b", code):
                resolved_tokens.append("aap_protobuf.aaw." + _cxx_to_dot(m.group(1)))

        resolved_set = set(resolved_tokens)

        # Match candidate entities from included protos (and transitive sub-messages) or resolved namespaces
        cand_entities: set[str] = set()
        for pfile in included_protos:
            cand_entities.update(entities_by_file.get(pfile, ()))
        if uses_bootstrap:
            cand_entities.update(bootstrap_entities)
        # Expand to any AASDK entity referenced by resolved C++ qualified token (e.g. MediaSinkService via Service.h or Enum values)
        for tok in resolved_set:
            if tok in sets["aasdk"]["messages"] or tok in sets["aasdk"]["enums"]:
                cand_entities.add(tok)
            parent = tok.rsplit(".", 1)[0] if "." in tok else ""
            if parent in sets["aasdk"]["messages"] or parent in sets["aasdk"]["enums"]:
                cand_entities.add(parent)

        # Check which candidate messages/enums are actually referenced in code
        file_used_entities: set[str] = set()
        for fqn in sorted(cand_entities):
            short = fqn.rsplit(".", 1)[-1]
            parent_short = ".".join(fqn.split(".")[-2:]) if "." in fqn else short
            # Check full resolved FQN OR alias+short OR parent+short
            matched = fqn in resolved_set or any(t.endswith("." + parent_short) for t in resolved_set)
            # Also match member type of a nested enum like Config::Status -> Config::STATUS_READY
            if not matched and fqn in sets["aasdk"]["enums"]:
                ent = sets["aasdk"]["enums"][fqn]
                pkg_scope = fqn.rsplit(".", 1)[0]
                for v in ent["values"]:
                    if f"{pkg_scope}.{v['name']}" in resolved_set or f"{fqn}.{v['name']}" in resolved_set:
                        matched = True
                        break
            # Also match submessage accessed purely via parent getter, e.g., entry.has_media_sink_service() / .mutable_video()
            if not matched and fqn in sets["aasdk"]["messages"]:
                for p_fqn, pm in sets["aasdk"]["messages"].items():
                    if p_fqn in file_used_entities or p_fqn in cand_entities:
                        for pf in pm["fields"]:
                            if pf["type"] == fqn and re.search(rf"\b(?:has_|set_|mutable_|add_)?{re.escape(pf['name'])}\b", code):
                                matched = True
                                break
                    if matched:
                        break
            if matched:
                file_used_entities.add(fqn)
                entity_files.setdefault(fqn, set()).add(rel)

        # Second pass for field-type entities accessed via used parent message getters
        changed = True
        while changed:
            changed = False
            for fqn in list(file_used_entities):
                msg = sets["aasdk"]["messages"].get(fqn)
                if not msg:
                    continue
                for f in msg["fields"]:
                    fname = f["name"]
                    if re.search(rf"\b(?:has_|set_|mutable_|add_|clear_)?{re.escape(fname)}\s*\(", code):
                        field_files.setdefault((fqn, fname), set()).add(rel)
                        if f["type_kind"] in ("message", "enum") and f["type"] not in file_used_entities:
                            file_used_entities.add(f["type"])
                            entity_files.setdefault(f["type"], set()).add(rel)
                            changed = True

        # Detect enum value references in this file
        for fqn in sorted(file_used_entities):
            enum_ent = sets["aasdk"]["enums"].get(fqn)
            if not enum_ent:
                continue
            for v in enum_ent["values"]:
                vname = v["name"]
                if re.search(rf"\b{re.escape(vname)}\b", code):
                    enum_val_files.setdefault((fqn, vname), set()).add(rel)

        # Detect strict ExtensionPolicy unknown-field checks and known tag sets per message
        lines = code.splitlines()
        tag_vars: dict[str, set[int]] = {}
        for m in re.finditer(r"constexpr\s+int\s+(\w+)\s*\[\s*\]\s*=\s*\{([^}]*)\}", code):
            tag_vars[m.group(1)] = _parse_int_set(m.group(2))

        if "kStrictExtensionPolicy" in code or "rejects_unknown(" in code:
            # Pattern 1: MediaConfiguration.cpp `rejects_unknown(msg_expr, tag_array)`
            for m in re.finditer(r"rejects_unknown\s*\(\s*([^,]+?)\s*,\s*(\w+)\s*\)", code):
                expr, tname = m.group(1), m.group(2)
                if tname not in tag_vars:
                    continue
                tags = tag_vars[tname]
                low = expr.lower()
                if "entry" in low and tname == "kServiceTags":
                    target_msg = "aap_protobuf.service.Service"
                elif "media_sink" in low or tname == "kMediaSinkServiceTags":
                    target_msg = "aap_protobuf.service.media.sink.MediaSinkService"
                elif "input" in low or tname == "kInputSourceServiceTags":
                    target_msg = "aap_protobuf.service.inputsource.InputSourceService"
                elif "video" in low or tname == "kVideoConfigurationTags":
                    target_msg = "aap_protobuf.service.media.sink.message.VideoConfiguration"
                elif "audio" in low or tname == "kAudioConfigurationTags":
                    target_msg = "aap_protobuf.service.media.shared.message.AudioConfiguration"
                elif tname == "service_tags" and "wire" in low:
                    target_msg = "aap_protobuf.service.sensorsource.SensorSourceService"
                elif tname == "sensor_tags" and "item" in low:
                    target_msg = "aap_protobuf.service.sensorsource.message.Sensor"
                elif tname == "source_tags" and "source" in low:
                    target_msg = "aap_protobuf.service.media.source.MediaSourceService"
                elif tname == "wifi_tags" and "wire" in low:
                    target_msg = "aap_protobuf.service.wifiprojection.WifiProjectionService"
                else:
                    target_msg = ""
                if target_msg:
                    cur_tags, cur_files = strict_msg_tags.setdefault(target_msg, (set(), set()))
                    cur_tags.update(tags)
                    cur_files.add(rel)

            # Pattern 2: AvSinkAdapter / MicrophoneAdapter / WifiProjectionAdapter `parse(wire, ..., tags)` or `classify_unknown_fields(wire, {})`
            for idx, line in enumerate(lines):
                if "classify_unknown_fields(" in line and ("{}" in line or "tags" in line):
                    fn_slice = "\n".join(lines[max(0, idx - 15):idx + 2])
                    dms = re.findall(r"\b([A-Za-z0-9_:]+)\s+wire\s*;", fn_slice)
                    if dms:
                        raw_type = dms[-1]
                        head, sep, rest = raw_type.partition("::")
                        fqn = _cxx_to_dot((aliases[head] + sep + rest) if head in aliases else raw_type)
                        if fqn in sets["aasdk"]["messages"]:
                            inline_m = re.search(r"classify_unknown_fields\s*\(\s*wire\s*,\s*\{([^}]*)\}", line)
                            tags = _parse_int_set(inline_m.group(1)) if inline_m else set()
                            cur_tags, cur_files = strict_msg_tags.setdefault(fqn, (set(), set()))
                            cur_tags.update(tags)
                            cur_files.add(rel)
                m_parse = re.search(r"\bparse\s*\(\s*wire\s*,\s*payload\s*,\s*[^,]+,\s*(\w+|\{[^}]*\})\s*\)", line)
                if m_parse:
                    arg = m_parse.group(1)
                    fn_slice = "\n".join(lines[max(0, idx - 12):idx + 1])
                    dms = re.findall(r"\b([A-Za-z0-9_:]+)\s+wire\s*;", fn_slice)
                    if dms:
                        raw_type = dms[-1]
                        head, sep, rest = raw_type.partition("::")
                        fqn = _cxx_to_dot((aliases[head] + sep + rest) if head in aliases else raw_type)
                        if fqn in sets["aasdk"]["messages"]:
                            if arg.startswith("{"):
                                tags = _parse_int_set(arg)
                            else:
                                tms = re.findall(rf"constexpr\s+int\s+{re.escape(arg)}\s*\[\s*\]\s*=\s*\{{([^}}]*)\}}", fn_slice)
                                tags = _parse_int_set(tms[-1]) if tms else set()
                            cur_tags, cur_files = strict_msg_tags.setdefault(fqn, (set(), set()))
                            cur_tags.update(tags)
                            cur_files.add(rel)

    # Map which enums are fields of strictly-parsed messages (proto2 unknown enum -> UnknownFieldSet -> rejected!)
    strict_enum_files: dict[str, set[str]] = {}
    for msg_fqn, (allowed_tags, s_files) in strict_msg_tags.items():
        msg = sets["aasdk"]["messages"].get(msg_fqn)
        if not msg:
            continue
        for f in msg["fields"]:
            if f["number"] in allowed_tags and f["type_kind"] == "enum":
                strict_enum_files.setdefault(f["type"], set()).update(s_files)

    # Also treat ServiceAdapter.cpp present_service_fields / classify_service checks on Service as direct Service usage
    if "aap_protobuf.service.Service" in entity_files:
        svc_msg = sets["aasdk"]["messages"].get("aap_protobuf.service.Service", {})
        for f in svc_msg.get("fields", []):
            if f["number"] == 1:
                field_files.setdefault(("aap_protobuf.service.Service", "id"), set()).add(
                    "src/protocol/aasdk_adapter/ServiceAdapter.cpp"
                )

    return {
        "entity_files": {k: sorted(v) for k, v in entity_files.items()},
        "field_files": {k: sorted(v) for k, v in field_files.items()},
        "enum_val_files": {k: sorted(v) for k, v in enum_val_files.items()},
        "strict_msg_tags": {k: (sorted(v[0]), sorted(v[1])) for k, v in strict_msg_tags.items()},
        "strict_enum_files": {k: sorted(v) for k, v in strict_enum_files.items()},
    }


def derive_usage(
    diff: dict[str, Any], scan: dict[str, Any], sets: dict[str, dict[str, Any]],
) -> dict[str, Any]:
    """Compute whether the receiver actually uses the diff's entity/field/enum-value."""
    aa_raw = diff.get("aasdk", "")
    aa_list = [x.strip() for x in aa_raw.split(",") if x.strip()]
    ent_files: list[str] = sorted({
        f for aa in aa_list for f in scan["entity_files"].get(aa, ())
    })
    entity_used = bool(ent_files)

    if not entity_used or not aa_list:
        return {
            "used_by_receiver": False,
            "entity_used_by_receiver": False,
            "usage_mode": "unused",
            "citations": [],
            "entity_citations": [],
        }

    aa = aa_list[0]
    kind = diff["kind"]
    key = diff.get("key", "")

    # Field-level checks
    if kind.startswith("field-") and key.startswith("field "):
        num = int(key.split()[1])
        msg = sets["aasdk"]["messages"].get(aa, {})
        fname = ""
        for f in msg.get("fields", []):
            if f["number"] == num:
                fname = f["name"]
                break
        if not fname and isinstance(diff.get("aasdk_value"), dict):
            fname = diff["aasdk_value"].get("name", "")
        direct = scan["field_files"].get((aa, fname), []) if fname else []
        strict_info = scan["strict_msg_tags"].get(aa)

        if kind in ("field-only-oaa", "field-vs-reserved"):
            # Rejected at decode if message is parsed under kStrictExtensionPolicy without this tag
            if strict_info and num not in strict_info[0]:
                cites = sorted(set(direct) | set(strict_info[1]))
                return {
                    "used_by_receiver": True,
                    "entity_used_by_receiver": True,
                    "usage_mode": "strict-unknown-tag-reject",
                    "citations": cites,
                    "entity_citations": ent_files,
                }
            return {
                "used_by_receiver": False,
                "entity_used_by_receiver": True,
                "usage_mode": "entity-active-field-absent",
                "citations": [],
                "entity_citations": ent_files,
            }

        if direct:
            return {
                "used_by_receiver": True,
                "entity_used_by_receiver": True,
                "usage_mode": "direct-field",
                "citations": direct,
                "entity_citations": ent_files,
            }
        # Also if strict parser validates known tag even without extracting value
        if strict_info and num in strict_info[0]:
            return {
                "used_by_receiver": True,
                "entity_used_by_receiver": True,
                "usage_mode": "strict-tag-checked",
                "citations": strict_info[1],
                "entity_citations": ent_files,
            }
        return {
            "used_by_receiver": False,
            "entity_used_by_receiver": True,
            "usage_mode": "entity-active-field-unused",
            "citations": [],
            "entity_citations": ent_files,
        }

    # Enum-value-level checks
    if kind.startswith("enum-value-") and key.startswith("value "):
        vnames = diff.get("aasdk_value") or []
        direct_cites: list[str] = sorted({
            f for vn in vnames for f in scan["enum_val_files"].get((aa, vn), ())
        })
        if direct_cites:
            return {
                "used_by_receiver": True,
                "entity_used_by_receiver": True,
                "usage_mode": "direct-enum-value",
                "citations": direct_cites,
                "entity_citations": ent_files,
            }
        strict_efiles = scan["strict_enum_files"].get(aa, [])
        if kind in ("enum-value-only-oaa", "enum-value-vs-reserved") and strict_efiles:
            return {
                "used_by_receiver": True,
                "entity_used_by_receiver": True,
                "usage_mode": "strict-unknown-enum-reject",
                "citations": strict_efiles,
                "entity_citations": ent_files,
            }
        return {
            "used_by_receiver": False,
            "entity_used_by_receiver": True,
            "usage_mode": "entity-active-value-unused",
            "citations": [],
            "entity_citations": ent_files,
        }

    # Entity-level diffs (syntax, reserved-ranges, extension-ranges, message-only-aasdk, enum-only-aasdk, undecided-match)
    return {
        "used_by_receiver": True,
        "entity_used_by_receiver": True,
        "usage_mode": "entity-used",
        "citations": ent_files,
        "entity_citations": ent_files,
    }


def _derive_channel_catalog(sets: dict[str, dict[str, Any]]) -> dict[str, dict[str, Any]]:
    """Aggregate msg IDs, directions, version gates, and OAA confidence across ALL entities per channel."""
    cat: dict[str, dict[str, Any]] = {}

    def ensure(ch: str) -> dict[str, Any]:
        return cat.setdefault(ch, {
            "aasdk_entities": 0,
            "oaa_entities": 0,
            "wire_msg_ids": set(),
            "directions": set(),
            "version_gates": set(),
            "oaa_confidence": set(),
            "msg_id_enums": {},
        })

    for col in ("messages", "enums"):
        for fqn, ent in sets["aasdk"][col].items():
            ch = _channel_from_aasdk_file(ent["file"])
            rec = ensure(ch)
            rec["aasdk_entities"] += 1
            ann = ent.get("annotations", {})
            rec["wire_msg_ids"].update(ann.get("wire_msg_ids", []))
            rec["directions"].update(ann.get("directions", []))
            rec["version_gates"].update(ann.get("version_gates", []))
            if col == "enums" and ("MessageId" in fqn or "ControlMessageType" in fqn):
                rec["msg_id_enums"][fqn] = [f"0x{v['number']:04X}={v['name']}" for v in ent["values"][:12]]
        for fqn, ent in sets["oaa"][col].items():
            ch = _channel_from_oaa_file(ent["file"])
            rec = ensure(ch)
            rec["oaa_entities"] += 1
            ann = ent.get("annotations", {})
            rec["wire_msg_ids"].update(ann.get("wire_msg_ids", []))
            rec["directions"].update(ann.get("directions", []))
            rec["version_gates"].update(ann.get("version_gates", []))
            rec["oaa_confidence"].update(ann.get("confidence", []))
    return cat


def _derive_wireless_modes(
    sets: dict[str, dict[str, Any]],
    enriched: list[dict[str, Any]],
) -> dict[str, Any]:
    """Derive structural comparison of wireless bootstrap vs Wi-Fi projection modes."""
    aa_bootstrap_msgs = sorted(
        fqn for fqn, m in sets["aasdk"]["messages"].items() if m["file"].startswith("aaw/")
    )
    oa_bootstrap_msgs = sorted(
        fqn for fqn, m in sets["oaa"]["messages"].items() if _channel_from_oaa_file(m["file"]) == "wireless-bootstrap"
    )
    wifi_channel_types = []
    wct = sets["oaa"]["enums"].get("oaa.proto.enums.WifiChannelType.Enum")
    if wct:
        wifi_channel_types = [f"{v['number']}={v['name']}" for v in wct["values"]]
    aa_sec = sets["aasdk"]["enums"].get("aap_protobuf.service.wifiprojection.message.WifiSecurityMode")
    oa_sec = sets["oaa"]["enums"].get("oaa.proto.enums.WifiSecurityMode.Enum")
    oa_cred_sec = sets["oaa"]["enums"].get("oaa.proto.messages.WifiCredentialsResponse.WifiCredentialSecurityMode")
    return {
        "aasdk_bootstrap_message_count": len(aa_bootstrap_msgs),
        "oaa_bootstrap_message_count": len(oa_bootstrap_msgs),
        "oaa_wifi_channel_types": wifi_channel_types,
        "aasdk_security_mode_values": [f"{v['number']}={v['name']}" for v in (aa_sec["values"] if aa_sec else [])],
        "oaa_security_mode_values": [f"{v['number']}={v['name']}" for v in (oa_sec["values"] if oa_sec else [])],
        "oaa_cred_security_mode_values": [f"{v['number']}={v['name']}" for v in (oa_cred_sec["values"] if oa_cred_sec else [])],
        "wireless_bootstrap_diffs": sum(1 for d in enriched if d["channel"] == "wireless-bootstrap"),
        "wireless_bootstrap_used": sum(1 for d in enriched if d["channel"] == "wireless-bootstrap" and d["used_by_receiver"]),
        "wifi_projection_diffs": sum(1 for d in enriched if d["channel"] == "wifi-projection"),
        "wifi_projection_used": sum(1 for d in enriched if d["channel"] == "wifi-projection" and d["used_by_receiver"]),
    }


def _derive_follow_up_groups(
    enriched: list[dict[str, Any]],
    findings_doc: dict[str, Any] | None,
) -> list[dict[str, Any]]:
    children = (findings_doc or {}).get("implementation_children", [])
    findings_by_id = {f["id"]: f for f in (findings_doc or {}).get("findings", []) if f.get("id")}
    groups: list[dict[str, Any]] = []
    for child in children:
        cid = child.get("id", "")
        owned = [
            d for d in enriched
            if d.get("triage") and d["triage"].get("owner") == cid
            and d["triage"].get("disposition") in ("follow-up-issue-needed", "needs-phone-evidence")
        ]
        confirmed = [
            d for d in owned
            if d["triage"].get("disposition") == "follow-up-issue-needed"
            and d["triage"].get("status") == "confirmed"
        ]
        research = [
            d for d in owned
            if d["triage"].get("status") in ("deferred", "proposed")
        ]
        schema_research = [
            d for d in research
            if d["triage"].get("disposition") == "follow-up-issue-needed"
        ]
        phone_ev = [
            d for d in research
            if d["triage"].get("disposition") == "needs-phone-evidence"
        ]

        def _summarize_bucket(subset: list[dict[str, Any]]) -> tuple[list[dict[str, Any]], dict[str, int]]:
            subset_ids = {d["id"] for d in subset}
            f_rows: list[dict[str, Any]] = []
            covered_in_bucket: set[str] = set()
            for fid in child.get("linked_findings", []):
                f = findings_by_id.get(fid)
                if not f:
                    continue
                matched_ids = [did for did in f.get("covers", []) if did in subset_ids]
                # Include finding without covers (e.g. D16-SDP-ORDER) in research_needed
                if matched_ids or (not f.get("covers") and f.get("status") in ("deferred", "proposed") and subset is research):
                    covered_in_bucket.update(matched_ids)
                    f_rows.append({
                        "id": fid,
                        "disposition": f.get("disposition", ""),
                        "status": f.get("status", ""),
                        "severity": f.get("severity", ""),
                        "diff_count": len(matched_ids),
                    })
            unfound_by_ch: dict[str, int] = {}
            for d in subset:
                if d["id"] not in covered_in_bucket:
                    unfound_by_ch[d["channel"]] = unfound_by_ch.get(d["channel"], 0) + 1
            return f_rows, dict(sorted(unfound_by_ch.items()))

        conf_findings, conf_unfound_ch = _summarize_bucket(confirmed)
        res_findings, res_unfound_ch = _summarize_bucket(research)
        conf_covered = {
            did
            for row in conf_findings
            for did in findings_by_id.get(row["id"], {}).get("covers", [])
        }
        conf_unfound_ids = sorted(d["id"] for d in confirmed if d["id"] not in conf_covered)
        res_covered = {
            did
            for row in res_findings
            for did in findings_by_id.get(row["id"], {}).get("covers", [])
        }
        phone_unfound_ids = sorted(d["id"] for d in phone_ev if d["id"] not in res_covered)

        groups.append({
            "child_id": cid,
            "title": child.get("title", ""),
            "priority": child.get("priority", ""),
            "total_diffs": len(owned),
            "confirmed_fix_needed": {
                "count": len(confirmed),
                "channels": sorted({d["channel"] for d in confirmed}),
                "findings": conf_findings,
                "unfound_by_channel": conf_unfound_ch,
                "unfound_diff_ids": conf_unfound_ids,
            },
            "research_needed": {
                "count": len(research),
                "schema_research_count": len(schema_research),
                "phone_evidence_count": len(phone_ev),
                "channels": sorted({d["channel"] for d in research}),
                "findings": res_findings,
                "unfound_by_channel": res_unfound_ch,
                "unfound_phone_evidence_ids": phone_unfound_ids,
            },
        })
    return groups


def enrich_differences(
    root: Path,
    diffs: list[dict[str, Any]],
    sets: dict[str, dict[str, Any]],
    triage_doc: dict[str, Any] | None,
    match: dict[str, Any] | None = None,
    findings_doc: dict[str, Any] | None = None,
) -> tuple[list[dict[str, Any]], dict[str, Any]]:
    """Compute derived triage attributes for every difference and merge human judgement."""
    scan = scan_receiver_usage(root, sets)
    entries = (triage_doc or {}).get("entries", {})
    if not isinstance(entries, dict):
        entries = {}

    pair_lookup: dict[tuple[str, str, str], dict[str, Any]] = {
        (p["kind"], p["aasdk"], p["oaa"]): p
        for p in (match or {}).get("pairs", [])
    }
    findings_by_diff: dict[str, list[str]] = {}
    for f in (findings_doc or {}).get("findings", []):
        fid = f.get("id")
        if not fid:
            continue
        for did in f.get("covers", []) or []:
            findings_by_diff.setdefault(did, []).append(fid)

    enriched: list[dict[str, Any]] = []
    for d in diffs:
        did = d["id"]
        cls = derive_classification(d)
        wire_aff = d["class"] == pc.CLASS_WIRE
        channel = derive_channel(d, sets)
        ann = derive_annotations(d, sets)
        src_loc = derive_source_loc(d, sets)
        usage = derive_usage(d, scan, sets)
        sev = derive_severity(wire_aff, usage["used_by_receiver"], cls)
        pair = pair_lookup.get((d["entity_kind"], d["aasdk"], d["oaa"]))
        match_evidence = {
            "evidence": pair["evidence"] if pair else d["kind"],
            "structural_score": pair["structural_score"] if pair else None,
        }
        hand = entries.get(did) if isinstance(entries.get(did), dict) else {}
        enriched.append({
            **d,
            "classification": cls,
            "wire_affecting": wire_aff,
            "severity": sev,
            "channel": channel,
            "used_by_receiver": usage["used_by_receiver"],
            "entity_used_by_receiver": usage["entity_used_by_receiver"],
            "usage_mode": usage["usage_mode"],
            "receiver_citations": usage["citations"],
            "entity_citations": usage["entity_citations"],
            "source": src_loc,
            "match_evidence": match_evidence,
            "covering_findings": sorted(findings_by_diff.get(did, [])),
            "annotations": ann,
            "triage": {
                "severity": hand.get("severity", ""),
                "status": hand.get("status", ""),
                "disposition": hand.get("disposition", ""),
                "owner": hand.get("owner", ""),
                "version_applicability": hand.get("version_applicability", ""),
                "evidence_tier": hand.get("evidence_tier", ""),
                "confidence": hand.get("confidence", ""),
                "impact": hand.get("impact", ""),
                "phone_evidence_status": hand.get("phone_evidence_status", ""),
                "rationale": hand.get("rationale", ""),
            } if hand else None,
        })

    # Per-channel summary table (100% derived)
    cat = _derive_channel_catalog(sets)
    channels_present = set(d["channel"] for d in enriched) | set(cat)
    ordered_channels = [c for c in CHANNEL_ORDER if c in channels_present]
    ordered_channels.extend(sorted(channels_present - set(ordered_channels)))

    per_channel: list[dict[str, Any]] = []
    all_classifications = sorted(set(CLASSIFICATION_MAP.values()))
    for ch in ordered_channels:
        sub = [d for d in enriched if d["channel"] == ch]
        crec = cat.get(ch, {})
        by_cls = {c: 0 for c in all_classifications}
        by_disp = {dp: 0 for dp in sorted(ALLOWED_TRIAGE_DISPOSITIONS)}
        retracted = 0
        cites_set: set[str] = set()
        for d in sub:
            by_cls[d["classification"]] = by_cls.get(d["classification"], 0) + 1
            t = d.get("triage")
            if t and t.get("disposition") in by_disp:
                by_disp[t["disposition"]] += 1
            if d["annotations"].get("oaa_retracted"):
                retracted += 1
            cites_set.update(d["receiver_citations"])
        per_channel.append({
            "channel": ch,
            "aasdk_entities": crec.get("aasdk_entities", 0),
            "oaa_entities": crec.get("oaa_entities", 0),
            "total": len(sub),
            "by_classification": {k: v for k, v in by_cls.items() if v > 0},
            "wire_affecting": sum(1 for d in sub if d["wire_affecting"]),
            "used_by_receiver": sum(1 for d in sub if d["used_by_receiver"]),
            "wire_and_used": sum(1 for d in sub if d["wire_affecting"] and d["used_by_receiver"]),
            "by_disposition": by_disp,
            "oaa_retracted_diffs": retracted,
            "wire_msg_ids": sorted(crec.get("wire_msg_ids", set())),
            "directions": sorted(crec.get("directions", set())),
            "version_gates": sorted(crec.get("version_gates", set())),
            "oaa_confidence": sorted(crec.get("oaa_confidence", set())),
            "receiver_files": sorted(cites_set),
        })

    # Overall triage summary
    tot_cls: dict[str, int] = {}
    tot_disp: dict[str, int] = {dp: 0 for dp in sorted(ALLOWED_TRIAGE_DISPOSITIONS)}
    tot_sev: dict[str, int] = {sv: 0 for sv in ("high", "medium-high", "medium", "low-medium", "low")}
    tot_status: dict[str, int] = {st: 0 for st in ("resolved", "confirmed", "deferred", "proposed")}
    tot_owner: dict[str, int] = {}
    triaged_count = 0
    for d in enriched:
        c = d["classification"]
        tot_cls[c] = tot_cls.get(c, 0) + 1
        sv = d["severity"]
        tot_sev[sv] = tot_sev.get(sv, 0) + 1
        if d["triage"] is not None:
            triaged_count += 1
            dp = d["triage"]["disposition"]
            if dp in tot_disp:
                tot_disp[dp] += 1
            st = d["triage"]["status"]
            if st in tot_status:
                tot_status[st] += 1
            ow = d["triage"]["owner"]
            if ow:
                tot_owner[ow] = tot_owner.get(ow, 0) + 1

    # Retracted OAA entities inventory (derived from .proto annotations)
    oaa_retracted_entities: list[dict[str, Any]] = []
    for kind, col in (("message", "messages"), ("enum", "enums")):
        for fqn, ent in sorted(sets["oaa"][col].items()):
            ann = ent.get("annotations", {})
            if ann.get("retracted"):
                oaa_retracted_entities.append({
                    "kind": kind,
                    "fqn": fqn,
                    "file": ent["file"],
                    "channel": _channel_from_oaa_file(ent["file"]),
                    "note": ann.get("retraction_note", ""),
                })

    summary = {
        "total_differences": len(enriched),
        "triaged": triaged_count,
        "untriaged": len(enriched) - triaged_count,
        "wire_affecting": sum(1 for d in enriched if d["wire_affecting"]),
        "used_by_receiver": sum(1 for d in enriched if d["used_by_receiver"]),
        "wire_and_used": sum(1 for d in enriched if d["wire_affecting"] and d["used_by_receiver"]),
        "by_classification": dict(sorted(tot_cls.items())),
        "by_severity": tot_sev,
        "severity_rule": SEVERITY_RULE_DOC,
        "by_disposition": tot_disp,
        "by_status": tot_status,
        "by_owner": dict(sorted(tot_owner.items())),
        "follow_up_groups": _derive_follow_up_groups(enriched, findings_doc),
        "per_channel": per_channel,
        "oaa_retracted_entities": oaa_retracted_entities,
        "wireless_modes": _derive_wireless_modes(sets, enriched),
    }
    return enriched, summary


_FIELD_TAG_REF_RE: Final = re.compile(
    r"\b(?:fields?|tags?)\s*[-:]?\s*(\d+(?:\s*[-/]\s*\d+)*)\b", re.IGNORECASE
)
_ENUM_VAL_REF_RE: Final = re.compile(
    r"\bvalues?\s+(-?\d+(?:\s*/\s*-?\d+)*)\b", re.IGNORECASE
)


def _extract_ref_nums(rx: re.Pattern[str], text: str) -> set[int]:
    out: set[int] = set()
    for m in rx.finditer(text):
        for part in re.split(r"\s*/\s*", m.group(1)):
            rng = re.fullmatch(r"(\d+)\s*-\s*(\d+)", part.strip())
            if rng:
                lo, hi = int(rng.group(1)), int(rng.group(2))
                if lo <= hi and hi - lo <= 20:
                    out.update(range(lo, hi + 1))
                else:
                    out.update((lo, hi))
            else:
                out.add(int(part.strip()))
    return out


def check_triage(
    triage_doc: dict[str, Any] | None,
    diffs: list[dict[str, Any]],
    valid_child_ids: set[str] | frozenset[str] | None = None,
    enriched: list[dict[str, Any]] | None = None,
) -> list[str]:
    """Validate completeness and referential integrity of protobuf-audit-triage.json."""
    errors: list[str] = []
    if triage_doc is None:
        return errors
    entries = triage_doc.get("entries")
    if not isinstance(entries, dict):
        return ["TRIAGE_INVALID: 'entries' must be an object mapping difference IDs to triage records"]

    diff_ids = {d["id"] for d in diffs}
    diffs_by_id = {d["id"]: d for d in diffs}
    enriched_by_id = {d["id"]: d for d in enriched} if enriched else {}
    for did in sorted(diff_ids - set(entries)):
        errors.append(f"TRIAGE_MISSING_DIFF: difference {did} has no triage entry in protobuf-audit-triage.json")
    for tid in sorted(set(entries) - diff_ids):
        errors.append(f"TRIAGE_DANGLING_DIFF: triage entry {tid} references a difference that no longer exists")

    for tid in sorted(set(entries) & diff_ids):
        rec = entries[tid]
        if not isinstance(rec, dict):
            errors.append(f"TRIAGE_INVALID: triage entry {tid} must be an object")
            continue
        for req in REQUIRED_TRIAGE_FIELDS:
            val = rec.get(req)
            if not isinstance(val, str) or not val.strip():
                errors.append(f"TRIAGE_INVALID: triage entry {tid} missing required field '{req}'")
        dkey = diffs_by_id[tid].get("key", "")
        rat = rec.get("rationale")
        if isinstance(rat, str):
            if dkey.startswith("field "):
                own_tag = int(dkey.split()[1])
                refs = _extract_ref_nums(_FIELD_TAG_REF_RE, rat)
                if refs and own_tag not in refs:
                    errors.append(
                        f"TRIAGE_INVALID: triage entry {tid} rationale references field/tag "
                        f"{sorted(refs)} instead of its own {dkey}"
                    )
            elif dkey.startswith("value "):
                own_val = int(dkey.split()[1])
                refs = _extract_ref_nums(_ENUM_VAL_REF_RE, rat)
                if refs and own_val not in refs:
                    errors.append(
                        f"TRIAGE_INVALID: triage entry {tid} rationale references value "
                        f"{sorted(refs)} instead of its own {dkey}"
                    )
        disp = rec.get("disposition")
        if disp not in ALLOWED_TRIAGE_DISPOSITIONS:
            errors.append(
                f"TRIAGE_INVALID: triage entry {tid} has invalid disposition {disp!r}; "
                f"expected one of {sorted(ALLOWED_TRIAGE_DISPOSITIONS)}"
            )
        sev = rec.get("severity")
        if sev not in ALLOWED_SEVERITIES:
            errors.append(
                f"TRIAGE_INVALID: triage entry {tid} has invalid severity {sev!r}; "
                f"expected one of {sorted(ALLOWED_SEVERITIES)}"
            )
        elif tid in enriched_by_id:
            ed = enriched_by_id[tid]
            expected_sev = derive_severity(ed["wire_affecting"], ed["used_by_receiver"], ed["classification"])
            if sev != expected_sev:
                errors.append(
                    f"TRIAGE_INVALID: triage entry {tid} has severity {sev!r}, expected derived {expected_sev!r}"
                )
        st = rec.get("status")
        if st not in ALLOWED_STATUSES:
            errors.append(
                f"TRIAGE_INVALID: triage entry {tid} has invalid status {st!r}; "
                f"expected one of {sorted(ALLOWED_STATUSES)}"
            )
        elif disp in TRIAGE_DISPOSITION_STATUS and st not in TRIAGE_DISPOSITION_STATUS[disp]:
            errors.append(
                f"TRIAGE_INVALID: triage entry {tid} has status {st!r} which is incompatible with "
                f"disposition {disp!r}; expected one of {sorted(TRIAGE_DISPOSITION_STATUS[disp])}"
            )
        tier = rec.get("evidence_tier")
        if tier not in ALLOWED_TIERS:
            errors.append(
                f"TRIAGE_INVALID: triage entry {tid} has invalid evidence_tier {tier!r}; "
                f"expected one of {sorted(ALLOWED_TIERS)}"
            )
        conf = rec.get("confidence")
        if conf not in ALLOWED_CONFIDENCES:
            errors.append(
                f"TRIAGE_INVALID: triage entry {tid} has invalid confidence {conf!r}; "
                f"expected one of {sorted(ALLOWED_CONFIDENCES)}"
            )
        owner = rec.get("owner")
        if isinstance(owner, str) and owner.strip():
            if disp == "no-action":
                if not owner.startswith("triage:"):
                    errors.append(
                        f"TRIAGE_INVALID: triage entry {tid} has disposition 'no-action' so owner must "
                        f"start with 'triage:' (got {owner!r})"
                    )
            elif disp in ("follow-up-issue-needed", "needs-phone-evidence"):
                if valid_child_ids is not None:
                    if owner not in valid_child_ids:
                        errors.append(
                            f"TRIAGE_INVALID: triage entry {tid} owner {owner!r} is not a known "
                            f"implementation child id ({sorted(valid_child_ids)})"
                        )
                elif not owner.startswith("child-"):
                    errors.append(
                        f"TRIAGE_INVALID: triage entry {tid} owner {owner!r} must reference an "
                        f"implementation child id ('child-*')"
                    )
    return errors
