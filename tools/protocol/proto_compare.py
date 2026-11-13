# SPDX-License-Identifier: GPL-3.0-or-later
"""AASDK <-> OAA schema matching and wire-level diff engine (Issue #62).

Input: two resolved schema sets from ``proto_schema.build_schema_set``.
Output: a deterministic match table (every message and enum of each side is
matched, unmatched-one-side, or undecided - never dropped) and a list of
generated differences with content-derived stable IDs.

Every number produced here is computed from the .proto files; nothing is
hand-written. Human decisions enter only through the explicit mapping table.
"""
from __future__ import annotations

import hashlib
import json
import re
from typing import Any, Final

# Structural-matching thresholds. A structural match is only accepted when it is
# unambiguous (mutual best, unique best, no tie) - otherwise the pair is left
# "undecided" and the candidates are listed. Never a guess.
MIN_STRUCT_FIELDS: Final[int] = 2
MIN_STRUCT_VALUES: Final[int] = 3
MIN_STRUCT_SCORE: Final[float] = 0.75
# Messages smaller than this need at least one same-number/same-normalised-name
# field as corroboration; enums always need one same-number/same-name value.
MIN_STRUCT_STRONG: Final[int] = 4
# A same-name pair whose structures share less than this is not accepted
# silently: it is reported as undecided (possible name collision).
MIN_NAME_SCORE: Final[float] = 0.25

CLASS_WIRE: Final[str] = "wire-affecting"
CLASS_INTERP: Final[str] = "interpretation"
CLASS_NONWIRE: Final[str] = "non-wire"
CLASS_UNMATCHED: Final[str] = "unmatched"
CLASS_UNDECIDED: Final[str] = "undecided"
ALL_CLASSES: Final[tuple[str, ...]] = (
    CLASS_WIRE, CLASS_INTERP, CLASS_NONWIRE, CLASS_UNMATCHED, CLASS_UNDECIDED,
)

_INT_FAMILY: Final[frozenset[str]] = frozenset(
    {"int32", "int64", "uint32", "uint64", "bool", "enum"}
)


def norm_name(name: str, strip_prefix: str = "") -> str:
    """Normalise an identifier: drop an enum-name prefix (longest word-prefix of the
    enum's snake-case name first), a trailing ``_eN`` exponent, case and underscores."""
    n = name
    words = [w for w in strip_prefix.upper().split("_") if w]
    done = False
    for width in range(len(words), 0, -1):  # longest word window of the enum name first
        for i in range(len(words) - width + 1):
            pre = "_".join(words[i:i + width]) + "_"
            if n.upper().startswith(pre) and len(n) > len(pre):
                n = n[len(pre):]
                done = True
                break
        if done:
            break
    n = re.sub(r"_e\d+$", "", n.lower())
    return n.replace("_", "")


def _snake(name: str) -> str:
    return re.sub(r"(?<!^)(?=[A-Z])", "_", name).upper()


def scale_exponent(name: str) -> str | None:
    m = re.search(r"_e(\d+)$", name.lower())
    return m.group(1) if m else None


def stable_id(prefix: str, *parts: Any) -> str:
    blob = json.dumps(list(parts), sort_keys=True, ensure_ascii=True, separators=(",", ":"))
    return f"{prefix}-{hashlib.sha256(blob.encode('utf-8')).hexdigest()[:12]}"


# ---------------------------------------------------------------------------
# signatures / similarity
# ---------------------------------------------------------------------------

def msg_signature(m: dict[str, Any]) -> frozenset[tuple[int, str]]:
    return frozenset((f["number"], f["wire"]) for f in m["fields"])


def enum_signature(e: dict[str, Any]) -> frozenset[int]:
    return frozenset(v["number"] for v in e["values"])


def jaccard(a: frozenset[Any], b: frozenset[Any]) -> float:
    if not a and not b:
        return 1.0
    return len(a & b) / len(a | b)


def _min_size(kind: str) -> int:
    return MIN_STRUCT_FIELDS if kind == "message" else MIN_STRUCT_VALUES


def _sig(kind: str, ent: dict[str, Any]) -> frozenset[Any]:
    return msg_signature(ent) if kind == "message" else enum_signature(ent)


def _size(kind: str, ent: dict[str, Any]) -> int:
    return len(ent["fields"]) if kind == "message" else len(ent["values"])


def rel_key(kind: str, ent: dict[str, Any]) -> str:
    """Relative name path; the OAA ``Foo.Enum`` wrapper-message idiom is unwrapped."""
    rel = ent["rel"]
    if kind == "enum" and rel.endswith(".Enum") and "." in rel:
        rel = rel[: -len(".Enum")]
    return rel


def simple_key(kind: str, ent: dict[str, Any]) -> str:
    return rel_key(kind, ent).rsplit(".", 1)[-1]


def normalized_key(kind: str, ent: dict[str, Any]) -> str:
    """Case-insensitive simple name without a trailing Data/Message suffix."""
    k = simple_key(kind, ent).lower()
    for suffix in ("data", "message"):
        if k.endswith(suffix) and len(k) > len(suffix) + 2:
            k = k[: -len(suffix)]
            break
    return k


_SENTINEL_NAMES: Final[frozenset[str]] = frozenset({
    "unknown", "none", "unspecified", "invalid", "unset", "undefined",
    "default", "empty", "notset", "novalue", "uninitialized", "null",
})
_SENTINEL_SUFFIXES: Final[tuple[str, ...]] = (
    "_unknown", "_none", "_unspecified", "_invalid", "_unset", "_undefined",
    "_default", "_empty", "_not_set", "_no_value", "_uninitialized", "_null",
)
_PKG_STOPWORDS: Final[frozenset[str]] = frozenset({
    "aap", "protobuf", "oaa", "proto", "service", "channel", "message", "messages",
    "data", "enums", "enum", "ids", "common", "shared", "generic", "legacy",
})
_DOMAIN_SYNONYMS: Final[dict[str, str]] = {
    "sensorsource": "sensor",
    "navigationstatus": "navigation",
    "inputsource": "input",
    "phonestatus": "phone",
    "wifiprojection": "wifi",
    "aaw": "wifi",
    "mediaplayback": "media",
    "mediabrowser": "media",
    "sink": "av",
    "source": "av",
    "genericnotification": "notification",
    "vendorextension": "vendor",
}


def is_sentinel_identifier(raw: str, normed: str) -> bool:
    """Return True if an identifier is a generic sentinel (UNKNOWN/NONE/UNSPECIFIED/INVALID/etc.)."""
    low = raw.lower()
    return normed in _SENTINEL_NAMES or low in _SENTINEL_NAMES or low.endswith(_SENTINEL_SUFFIXES)


def path_package_tokens(ent: dict[str, Any]) -> set[str]:
    """Extract normalized domain/channel tokens from an entity's file directory and package scope."""
    rel_dir = ent["file"].rsplit("/", 1)[0] if "/" in ent["file"] else ""
    pkg = ent["fqn"].rsplit(".", 1)[0] if "." in ent["fqn"] else ""
    tokens: set[str] = set()
    for part in re.split(r"[/._]", f"{rel_dir}.{pkg}"):
        for tok in re.sub(r"([a-z0-9])([A-Z])", r"\1_\2", part).lower().split("_"):
            tok = _DOMAIN_SYNONYMS.get(tok, tok)
            if len(tok) >= 2 and tok not in _PKG_STOPWORDS:
                tokens.add(tok)
    return tokens


def path_package_overlap(a: dict[str, Any], b: dict[str, Any]) -> int:
    """Count shared domain/channel tokens between two entities' paths and packages."""
    return len(path_package_tokens(a) & path_package_tokens(b))


def non_sentinel_name_overlap(
    kind: str, a: dict[str, Any], b: dict[str, Any], *, include_zero: bool = False,
) -> int:
    """Count shared numbers with matching normalized non-sentinel names.

    Sentinels (``UNKNOWN``, ``NONE``, ``UNSPECIFIED``, ``INVALID``, etc.) and, by
    default, enum value ``0`` are excluded so generic placeholders cannot carry a
    structural match.
    """
    if kind == "message":
        nb = {f["number"]: (f["name"], norm_name(f["name"])) for f in b["fields"]}
        cnt = 0
        for f in a["fields"]:
            n = f["number"]
            if n not in nb:
                continue
            na = norm_name(f["name"])
            rb_raw, rb_norm = nb[n]
            if (na == rb_norm
                    and not is_sentinel_identifier(f["name"], na)
                    and not is_sentinel_identifier(rb_raw, rb_norm)):
                cnt += 1
        return cnt
    pa = _snake(simple_key("enum", a))
    pb = _snake(simple_key("enum", b))
    nb2 = {v["number"]: (v["name"], norm_name(v["name"], pb)) for v in b["values"]}
    cnt = 0
    for v in a["values"]:
        n = v["number"]
        if (not include_zero and n == 0) or n not in nb2:
            continue
        na = norm_name(v["name"], pa)
        rb_raw, rb_norm = nb2[n]
        if (na == rb_norm
                and not is_sentinel_identifier(v["name"], na)
                and not is_sentinel_identifier(rb_raw, rb_norm)):
            cnt += 1
    return cnt


def name_overlap(kind: str, a: dict[str, Any], b: dict[str, Any]) -> int:
    """Count of shared numbers whose non-sentinel names are equal after normalisation."""
    return non_sentinel_name_overlap(kind, a, b)


# ---------------------------------------------------------------------------
# matching
# ---------------------------------------------------------------------------

def enum_wrappers(schema: dict[str, Any]) -> set[str]:
    """Empty messages that only namespace one nested enum (the ``Foo.Enum`` idiom).

    They carry no wire data and are never used as a field type, so they are
    reported as ``wrapper`` rather than as unmatched wire entities.
    """
    referenced = {f["type"] for m in schema["messages"].values() for f in m["fields"]
                  if f["type_kind"] == "message"}
    out: set[str] = set()
    for fqn, m in schema["messages"].items():
        if (not m["fields"] and not m["nested_messages"] and len(m["nested_enums"]) == 1
                and not m["reserved_ranges"] and fqn not in referenced):
            out.add(fqn)
    return out


def match_sets(
    aasdk: dict[str, Any], oaa: dict[str, Any], mapping: dict[str, Any],
) -> dict[str, Any]:
    """Pair messages and enums. Returns the match table and unresolved lists."""
    errors: list[str] = []
    wrappers = {"aasdk": enum_wrappers(aasdk), "oaa": enum_wrappers(oaa)}
    pairs: list[dict[str, Any]] = []
    undecided: list[dict[str, Any]] = []
    used_a: set[tuple[str, str]] = set()
    used_b: set[tuple[str, str]] = set()
    kinds = ("message", "enum")
    sets_a = {"message": aasdk["messages"], "enum": aasdk["enums"]}
    sets_b = {"message": oaa["messages"], "enum": oaa["enums"]}

    def add(kind: str, fa: str, fb: str, evidence: str, extra: dict[str, Any] | None = None) -> None:
        score = jaccard(_sig(kind, sets_a[kind][fa]), _sig(kind, sets_b[kind][fb]))
        p = {"kind": kind, "aasdk": fa, "oaa": fb, "evidence": evidence,
             "structural_score": round(score, 4)}
        if extra:
            p.update(extra)
        pairs.append(p)
        used_a.add((kind, fa))
        used_b.add((kind, fb))

    for fqn in wrappers["aasdk"]:
        used_a.add(("message", fqn))
    for fqn in wrappers["oaa"]:
        used_b.add(("message", fqn))

    def propagate() -> None:
        """Pair unmatched same-numbered, same-kind children of already matched pairs."""
        changed = True
        while changed:
            changed = False
            for p in sorted(pairs, key=lambda q: (q["kind"], q["aasdk"], q["oaa"])):
                if p["kind"] != "message":
                    continue
                ma, mb = sets_a["message"][p["aasdk"]], sets_b["message"][p["oaa"]]
                fb_by = {f["number"]: f for f in mb["fields"]}
                for f in ma["fields"]:
                    g = fb_by.get(f["number"])
                    if not g or f["type_kind"] != g["type_kind"] or f["type_kind"] not in ("message", "enum"):
                        continue
                    kind = f["type_kind"]
                    ca, cb = f["type"], g["type"]
                    if (kind, ca) in used_a or (kind, cb) in used_b:
                        continue
                    sc = jaccard(_sig(kind, sets_a[kind][ca]), _sig(kind, sets_b[kind][cb]))
                    if sc < (0.5 if kind == "enum" else 0.6):
                        continue
                    add(kind, ca, cb, "propagated:field-position",
                        {"via": f"{p['aasdk']} field {f['number']}"})
                    changed = True

    # 1. explicit human-decided mapping --------------------------------------
    for entry in mapping.get("pairs", []):
        kind, fa, fb = entry.get("kind"), entry.get("aasdk"), entry.get("oaa")
        if kind not in kinds or fa not in sets_a.get(kind, {}) or fb not in sets_b.get(kind, {}):
            errors.append(f"MAPPING_DANGLING: explicit pair {kind} {fa} <-> {fb} names a "
                          f"definition that does not exist")
            continue
        if (kind, fa) in used_a or (kind, fb) in used_b:
            errors.append(f"MAPPING_DUPLICATE: {kind} {fa} <-> {fb} reuses a mapped definition")
            continue
        # An explicit pair must not silently steal a definition that an identical
        # name would have paired with something else (that is how wrong mappings hide).
        for side_set, other_set, mine, theirs in (
            (sets_a, sets_b, fa, fb), (sets_b, sets_a, fb, fa),
        ):
            key = rel_key(kind, side_set[kind][mine])
            clash = sorted(f for f, e in other_set[kind].items() if rel_key(kind, e) == key and f != theirs)
            if clash and not entry.get("overrides_name_match"):
                errors.append(f"MAPPING_SHADOWS_NAME_MATCH: explicit pair {kind} {fa} <-> {fb} takes "
                              f"{mine} away from same-named {clash[0]}; delete the pair or set "
                              f"'overrides_name_match' with a reason")
        add(kind, fa, fb, "explicit-mapping",
            {"decided_by": entry.get("decided_by", ""), "reason": entry.get("reason", "")})
    declared_none: dict[tuple[str, str, str], str] = {}
    for entry in mapping.get("no_counterpart", []):
        side, kind, fqn = entry.get("side"), entry.get("kind"), entry.get("fqn")
        sets = sets_a if side == "aasdk" else sets_b
        if side not in ("aasdk", "oaa") or kind not in kinds or fqn not in sets[kind]:
            errors.append(f"MAPPING_DANGLING: no_counterpart {side} {kind} {fqn} does not exist")
            continue
        declared_none[(side, kind, fqn)] = entry.get("reason", "")

    # 2/3. identical relative name path, then identical simple name -----------
    for keyf, label in (
        (rel_key, "name:relative-path"),
        (simple_key, "name:simple"),
        (normalized_key, "name:normalized"),
    ):
        for kind in kinds:
            ga: dict[str, list[str]] = {}
            gb: dict[str, list[str]] = {}
            for fqn, e in sets_a[kind].items():
                if (kind, fqn) not in used_a:
                    ga.setdefault(keyf(kind, e), []).append(fqn)
            for fqn, e in sets_b[kind].items():
                if (kind, fqn) not in used_b:
                    gb.setdefault(keyf(kind, e), []).append(fqn)
            for key in sorted(set(ga) & set(gb)):
                la, lb = sorted(ga[key]), sorted(gb[key])
                if len(la) == 1 and len(lb) == 1:
                    sc = jaccard(_sig(kind, sets_a[kind][la[0]]), _sig(kind, sets_b[kind][lb[0]]))
                    if sc < MIN_NAME_SCORE:
                        undecided.append({
                            "kind": kind, "side": "both", "aasdk": [la[0]], "oaa": [lb[0]],
                            "reason": f"same name {key!r} but structural overlap {sc:.2f} < "
                                      f"{MIN_NAME_SCORE}: possible name collision"})
                        continue
                    add(kind, la[0], lb[0], label)
                    continue
                # several candidates with the same name: disambiguate structurally
                cand = [(jaccard(_sig(kind, sets_a[kind][x]), _sig(kind, sets_b[kind][y])), x, y)
                        for x in la for y in lb]
                cand.sort(key=lambda t: (-t[0], t[1], t[2]))
                taken_x: set[str] = set()
                taken_y: set[str] = set()
                for sc, x, y in cand:
                    if x in taken_x or y in taken_y or sc < MIN_STRUCT_SCORE:
                        continue
                    # unique best for both x and y among the group
                    best_x = [c for c in cand if c[1] == x and c[2] not in taken_y]
                    best_y = [c for c in cand if c[2] == y and c[1] not in taken_x]
                    if (len(best_x) > 1 and best_x[1][0] == sc) or (len(best_y) > 1 and best_y[1][0] == sc):
                        continue
                    add(kind, x, y, label + "+structure")
                    taken_x.add(x)
                    taken_y.add(y)
                rest_a = [x for x in la if x not in taken_x]
                rest_b = [y for y in lb if y not in taken_y]
                if rest_a and rest_b:
                    undecided.append({
                        "kind": kind, "side": "both", "aasdk": rest_a, "oaa": rest_b,
                        "reason": f"{len(la)} AASDK and {len(lb)} OAA definitions share the name "
                                  f"{key!r}; structure does not pair them unambiguously"})

    propagate()

    # 4. unambiguous structural similarity ------------------------------------
    for kind in kinds:
        ra = {f: _sig(kind, e) for f, e in sets_a[kind].items()
              if (kind, f) not in used_a and _size(kind, e) >= _min_size(kind)}
        rb = {f: _sig(kind, e) for f, e in sets_b[kind].items()
              if (kind, f) not in used_b and _size(kind, e) >= _min_size(kind)}
        by_a: dict[str, list[tuple[tuple[float, int], str]]] = {}
        by_b: dict[str, list[tuple[tuple[float, int], str]]] = {}
        inv_b: dict[Any, list[str]] = {}
        for fb, sb in rb.items():
            for el in sb:
                inv_b.setdefault(el, []).append(fb)
        for fa, sa in ra.items():
            seen: set[str] = set()
            for el in sa:
                seen.update(inv_b.get(el, ()))
            for fb in sorted(seen):
                sc = jaccard(sa, rb[fb])
                if sc >= MIN_STRUCT_SCORE * 0.6:  # weaker candidates kept for tie detection
                    key = (sc, name_overlap(kind, sets_a[kind][fa], sets_b[kind][fb]))
                    by_a.setdefault(fa, []).append((key, fb))
                    by_b.setdefault(fb, []).append((key, fa))
        for d in (by_a, by_b):
            for k in d:
                d[k].sort(key=lambda t: (-t[0][0], -t[0][1], t[1]))
        accepted: list[tuple[str, str]] = []
        for fa in sorted(by_a):
            key, fb = by_a[fa][0]
            if key[0] < MIN_STRUCT_SCORE:
                continue
            if len(by_a[fa]) > 1 and by_a[fa][1][0] == key:
                continue
            if by_b[fb][0][1] != fa:
                continue
            if len(by_b[fb]) > 1 and by_b[fb][1][0] == key:
                continue
            # evidence strength: sentinels / value 0 / numeric-only overlap cannot carry a match
            ea, eb = sets_a[kind][fa], sets_b[kind][fb]
            nso = key[1]
            pkg_sim = path_package_overlap(ea, eb)
            if nso < 1 and pkg_sim < 1:
                continue
            if kind == "enum" and nso < 1 and non_sentinel_name_overlap(kind, ea, eb, include_zero=True) < 1:
                continue
            if kind == "message" and len(ea["fields"]) < MIN_STRUCT_STRONG and nso < 1:
                continue
            accepted.append((fa, fb))
        for fa, fb in accepted:
            add(kind, fa, fb, "structure")
        for fa in sorted(by_a):
            if (kind, fa) in used_a:
                continue
            cands = [(k, fb) for k, fb in by_a[fa] if (kind, fb) not in used_b]
            if not cands or cands[0][0][0] < MIN_STRUCT_SCORE:
                continue
            top = [fb for k, fb in cands if k == cands[0][0]]
            undecided.append({
                "kind": kind, "side": "aasdk", "aasdk": [fa], "oaa": sorted(top)[:8],
                "reason": f"best structural overlap {cands[0][0][0]:.2f} (same-name field/value "
                          f"matches {cands[0][0][1]}) is tied or not mutual "
                          f"({len(top)} OAA candidate(s)); not guessed"})

    propagate()

    # An undecided entry only stands while its definitions are still unmatched.
    matched_a = {(p["kind"], p["aasdk"]) for p in pairs}
    matched_b = {(p["kind"], p["oaa"]) for p in pairs}
    merged: dict[tuple[str, tuple[str, ...], tuple[str, ...]], dict[str, Any]] = {}
    for u in undecided:
        ua = sorted(x for x in u["aasdk"] if (u["kind"], x) not in matched_a)
        ub = sorted(y for y in u["oaa"] if (u["kind"], y) not in matched_b)
        if ua and ub:
            key = (u["kind"], tuple(ua), tuple(ub))
            if key in merged:
                if u["reason"] not in merged[key]["reason"]:
                    merged[key]["reason"] += "; " + u["reason"]
            else:
                merged[key] = {**u, "aasdk": ua, "oaa": ub}
    undecided = list(merged.values())

    pairs.sort(key=lambda p: (p["kind"], p["aasdk"], p["oaa"]))
    undecided.sort(key=lambda u: (u["kind"], u["side"], u["aasdk"], u["oaa"]))
    # nothing may be dropped: every definition is in exactly one bucket
    in_pair_a = {(p["kind"], p["aasdk"]) for p in pairs}
    in_pair_b = {(p["kind"], p["oaa"]) for p in pairs}
    und_a = {(u["kind"], x) for u in undecided for x in u["aasdk"]}
    und_b = {(u["kind"], y) for u in undecided for y in u["oaa"]}
    only_a = sorted((k, f) for k in kinds for f in sets_a[k]
                    if (k, f) not in in_pair_a and (k, f) not in und_a
                    and not (k == "message" and f in wrappers["aasdk"]))
    only_b = sorted((k, f) for k in kinds for f in sets_b[k]
                    if (k, f) not in in_pair_b and (k, f) not in und_b
                    and not (k == "message" and f in wrappers["oaa"]))
    return {
        "pairs": pairs, "undecided": undecided,
        "aasdk_only": [{"kind": k, "fqn": f,
                        "declared_reason": declared_none.get(("aasdk", k, f), "")} for k, f in only_a],
        "oaa_only": [{"kind": k, "fqn": f,
                      "declared_reason": declared_none.get(("oaa", k, f), "")} for k, f in only_b],
        "wrappers": {"aasdk": sorted(wrappers["aasdk"]), "oaa": sorted(wrappers["oaa"])},
        "errors": errors,
    }


# ---------------------------------------------------------------------------
# diffing
# ---------------------------------------------------------------------------

def _field_desc(f: dict[str, Any]) -> dict[str, Any]:
    return {"number": f["number"], "name": f["name"], "type": f["type"], "wire": f["wire"],
            "cardinality": f["cardinality"], "presence": f["presence"]}


def _scalar_class(ta: str, tb: str) -> tuple[str, str]:
    """(class, subkind) for two scalar/enum type names that share a wire kind."""
    fam_a = "enum" if ta == "enum" else ta
    fam_b = "enum" if tb == "enum" else tb
    if fam_a == fam_b:
        return CLASS_NONWIRE, "same-type"
    zig = {"sint32", "sint64"}
    if (fam_a in zig) != (fam_b in zig):
        return CLASS_WIRE, "zigzag-vs-plain-varint"
    if fam_a in _INT_FAMILY and fam_b in _INT_FAMILY:
        return CLASS_INTERP, "varint-integer-interpretation"
    if {fam_a, fam_b} <= {"string", "bytes"}:
        return CLASS_INTERP, "string-vs-bytes"
    if fam_a in ("double",) or fam_b in ("double",) or fam_a in ("float",) or fam_b in ("float",):
        return CLASS_WIRE, "float-width-or-kind"
    return CLASS_INTERP, "fixed-width-interpretation"


def diff_pair(
    pair: dict[str, Any], aasdk: dict[str, Any], oaa: dict[str, Any],
    pair_of_a: dict[tuple[str, str], str],
) -> list[dict[str, Any]]:
    kind, fa, fb = pair["kind"], pair["aasdk"], pair["oaa"]
    out: list[dict[str, Any]] = []

    def emit(cls: str, sub: str, key: str, va: Any, vb: Any, detail: str) -> None:
        out.append({
            "id": stable_id("W", kind, fa, fb, sub, key, va, vb),
            "class": cls, "kind": sub, "entity_kind": kind, "aasdk": fa, "oaa": fb,
            "key": key, "aasdk_value": va, "oaa_value": vb, "detail": detail,
        })

    if kind == "message":
        ma, mb = aasdk["messages"][fa], oaa["messages"][fb]
        fa_by = {f["number"]: f for f in ma["fields"]}
        fb_by = {f["number"]: f for f in mb["fields"]}
        for n in sorted(set(fa_by) | set(fb_by)):
            a, b = fa_by.get(n), fb_by.get(n)
            key = f"field {n}"
            if a and not b:
                cls, sub = CLASS_WIRE, "field-only-aasdk"
                if any(r[0] <= n <= r[1] for r in mb["reserved_ranges"]):
                    sub = "field-vs-reserved"
                emit(cls, sub, key, _field_desc(a), None,
                     f"field {n} '{a['name']}' exists only in AASDK")
                continue
            if b and not a:
                sub = "field-only-oaa"
                if any(r[0] <= n <= r[1] for r in ma["reserved_ranges"]):
                    sub = "field-vs-reserved"
                emit(CLASS_WIRE, sub, key, None, _field_desc(b),
                     f"field {n} '{b['name']}' exists only in OAA")
                continue
            assert a and b
            if a["wire"] != b["wire"]:
                emit(CLASS_WIRE, "field-wire-type", key, _field_desc(a), _field_desc(b),
                     f"wire type {a['wire']} vs {b['wire']}")
                continue  # further comparisons are meaningless across wire types
            if a["cardinality"] != b["cardinality"]:
                emit(CLASS_WIRE, "field-cardinality", key, a["cardinality"], b["cardinality"],
                     "repeated vs singular")
            if a["presence"] != b["presence"] and a["cardinality"] == b["cardinality"] == "singular":
                emit(CLASS_INTERP, "field-presence", key, a["presence"], b["presence"],
                     "required/optional/implicit presence differs (bytes identical when present)")
            # type
            ta, tb = a["type"], b["type"]
            if a["type_kind"] == "message" and b["type_kind"] == "message":
                if pair_of_a.get(("message", ta)) != tb:
                    emit(CLASS_WIRE, "field-embedded-type", key, ta, tb,
                         "embedded message types are not a matched pair")
            elif a["type_kind"] == "enum" and b["type_kind"] == "enum":
                if pair_of_a.get(("enum", ta)) != tb:
                    emit(CLASS_INTERP, "field-enum-type", key, ta, tb,
                         "enum types are not a matched pair")
            elif a["type_kind"] != b["type_kind"]:
                sa = "enum" if a["type_kind"] == "enum" else a["type"]
                sb = "enum" if b["type_kind"] == "enum" else b["type"]
                if "message" in (a["type_kind"], b["type_kind"]) or "map" in (a["type_kind"], b["type_kind"]):
                    emit(CLASS_WIRE, "field-type-kind", key, a["type"], b["type"],
                         f"{a['type_kind']} vs {b['type_kind']}")
                else:
                    cls, sub = _scalar_class(sa, sb)
                    if cls != CLASS_NONWIRE:
                        emit(cls, "field-" + sub, key, a["type"], b["type"],
                             f"{a['type']} vs {b['type']} share wire type {a['wire']}")
            else:  # both scalar
                cls, sub = _scalar_class(ta, tb)
                if cls != CLASS_NONWIRE:
                    emit(cls, "field-" + sub, key, ta, tb, f"{ta} vs {tb} share wire type {a['wire']}")
            if a["default"] != b["default"]:
                emit(CLASS_INTERP, "field-default", key, a["default"], b["default"],
                     "declared default differs")
            if a["packed"] != b["packed"]:
                emit(CLASS_NONWIRE, "field-packed", key, a["packed"], b["packed"],
                     "packed vs unpacked: protobuf parsers accept both forms")
            if (a["oneof"] is None) != (b["oneof"] is None):
                emit(CLASS_INTERP, "field-oneof-membership", key, a["oneof"], b["oneof"],
                     "oneof membership differs (last-one-wins semantics)")
            if a["deprecated"] != b["deprecated"]:
                emit(CLASS_NONWIRE, "field-deprecated", key, a["deprecated"], b["deprecated"],
                     "deprecated flag differs")
            if a["name"] != b["name"]:
                ea, eb = scale_exponent(a["name"]), scale_exponent(b["name"])
                if ea and eb and ea != eb and norm_name(a["name"]) == norm_name(b["name"]):
                    emit(CLASS_INTERP, "field-scale-exponent", key, a["name"], b["name"],
                         f"fixed-point exponent _e{ea} vs _e{eb}")
                elif norm_name(a["name"]) == norm_name(b["name"]):
                    emit(CLASS_NONWIRE, "field-rename-normalized", key, a["name"], b["name"],
                         "names equal after case/underscore/_eN-suffix normalisation")
                else:
                    emit(CLASS_INTERP, "field-rename", key, a["name"], b["name"],
                         "field name differs and is not a normalised rename: same number and wire "
                         "type, meaning not mechanically comparable (rename or semantic change)")
        # reserved conflicts (a reserved number that is a live field on the other
        # side) are emitted above as field-vs-reserved
        if sorted(ma["reserved_ranges"]) != sorted(mb["reserved_ranges"]):
            emit(CLASS_INTERP, "reserved-ranges", "reserved", ma["reserved_ranges"],
                 mb["reserved_ranges"], "reserved number ranges differ")
        if sorted(ma["reserved_names"]) != sorted(mb["reserved_names"]):
            emit(CLASS_NONWIRE, "reserved-names", "reserved-names",
                 sorted(ma["reserved_names"]), sorted(mb["reserved_names"]), "reserved names differ")
        if ma["extension_ranges"] != mb["extension_ranges"]:
            emit(CLASS_INTERP, "extension-ranges", "extensions", ma["extension_ranges"],
                 mb["extension_ranges"], "extension ranges differ")
        if ma["syntax"] != mb["syntax"]:
            emit(CLASS_NONWIRE, "syntax", "syntax", ma["syntax"], mb["syntax"],
                 "proto2 vs proto3 (analysis input, not itself a wire difference)")
    else:
        ea, eb = aasdk["enums"][fa], oaa["enums"][fb]
        pa, pb = _snake(ea["rel"].rsplit(".", 1)[-1]), _snake(eb["rel"].rsplit(".", 1)[-1])
        va_by: dict[int, list[dict[str, Any]]] = {}
        vb_by: dict[int, list[dict[str, Any]]] = {}
        for v in ea["values"]:
            va_by.setdefault(v["number"], []).append(v)
        for v in eb["values"]:
            vb_by.setdefault(v["number"], []).append(v)
        for n in sorted(set(va_by) | set(vb_by)):
            la, lb = va_by.get(n), vb_by.get(n)
            key = f"value {n}"
            if la and not lb:
                reserved = any(r[0] <= n <= r[1] for r in eb["reserved_ranges"])
                emit(CLASS_WIRE, "enum-value-vs-reserved" if reserved else "enum-value-only-aasdk",
                     key, [v["name"] for v in la], None,
                     f"enum value {n} exists only in AASDK")
            elif lb and not la:
                reserved = any(r[0] <= n <= r[1] for r in ea["reserved_ranges"])
                emit(CLASS_WIRE, "enum-value-vs-reserved" if reserved else "enum-value-only-oaa",
                     key, None, [v["name"] for v in lb], f"enum value {n} exists only in OAA")
            else:
                na = sorted(v["name"] for v in la)
                nb = sorted(v["name"] for v in lb)
                if na != nb:
                    norm_a = sorted(norm_name(x, pa) for x in na)
                    norm_b = sorted(norm_name(x, pb) for x in nb)
                    ex_a = {scale_exponent(x) for x in na} - {None}
                    ex_b = {scale_exponent(x) for x in nb} - {None}
                    if norm_a == norm_b and not (ex_a and ex_b and ex_a != ex_b):
                        emit(CLASS_NONWIRE, "enum-value-rename-normalized", key, na, nb,
                             "names equal after case/underscore/prefix normalisation")
                    else:
                        emit(CLASS_WIRE, "enum-value-name-conflict", key, na, nb,
                             f"value {n} is named differently; may be a rename or a semantic "
                             f"conflict - not mechanically decidable")
        if sorted(ea["reserved_ranges"]) != sorted(eb["reserved_ranges"]):
            emit(CLASS_INTERP, "reserved-ranges", "reserved", ea["reserved_ranges"],
                 eb["reserved_ranges"], "reserved ranges differ")
        if ea["allow_alias"] != eb["allow_alias"]:
            emit(CLASS_NONWIRE, "enum-allow-alias", "allow_alias", ea["allow_alias"], eb["allow_alias"],
                 "allow_alias differs")
    out.sort(key=lambda d: d["id"])
    return out


def compare(aasdk: dict[str, Any], oaa: dict[str, Any], mapping: dict[str, Any]) -> dict[str, Any]:
    m = match_sets(aasdk, oaa, mapping)
    pair_of_a = {(p["kind"], p["aasdk"]): p["oaa"] for p in m["pairs"]}
    diffs: list[dict[str, Any]] = []
    for p in m["pairs"]:
        diffs.extend(diff_pair(p, aasdk, oaa, pair_of_a))
    for u in m["undecided"]:
        diffs.append({
            "id": stable_id("U", u["kind"], "undecided", u["aasdk"], u["oaa"]),
            "class": CLASS_UNDECIDED, "kind": "undecided-match", "entity_kind": u["kind"],
            "aasdk": ",".join(u["aasdk"]), "oaa": ",".join(u["oaa"]), "key": "",
            "aasdk_value": None, "oaa_value": None, "detail": u["reason"],
        })
    for side, lst in (("aasdk", m["aasdk_only"]), ("oaa", m["oaa_only"])):
        for e in lst:
            diffs.append({
                "id": stable_id("U", e["kind"], f"only-{side}", e["fqn"]),
                "class": CLASS_UNMATCHED, "kind": f"{e['kind']}-only-{side}",
                "entity_kind": e["kind"],
                "aasdk": e["fqn"] if side == "aasdk" else "",
                "oaa": e["fqn"] if side == "oaa" else "", "key": "",
                "aasdk_value": None, "oaa_value": None,
                "detail": f"{e['kind']} has no counterpart by explicit mapping, name or structure",
            })
    diffs.sort(key=lambda d: d["id"])
    ids = [d["id"] for d in diffs]
    if len(ids) != len(set(ids)):
        dup = sorted({i for i in ids if ids.count(i) > 1})
        m["errors"].append(f"DIFF_ID_COLLISION: {dup}")
    m["diffs"] = diffs
    return m
