"""Consumer-side manifest validation and before/after protected-path diff.

Validates "ppic/1" snapshot manifests written by the producer
(snapshot_envelope.SnapshotManifest) and compares two manifests into a
machine-readable diff artifact plus an evidence receipt. Filesystem
equivalence is exact per-entry field comparison; timing/diagnostic headers
are never equivalence evidence. The physical inventory is authoritative;
the git supplement is compared as evidence content (see "Git section" below)
but the physical inventory locates the changes.

PRODUCER GROUND TRUTH (verified against tools.target_safety.snapshot by
writing a real manifest and checking the trailer): manifest_digest is
SHA-256 over json.dumps(body, sort_keys=True, separators=(",", ":"),
ensure_ascii=True).encode("utf-8") where body is the manifest object with
the completion-trailer key REMOVED ENTIRELY -- the producer hashes
everything except the trailer (snapshot_envelope.canonical_body); it does
NOT null a digest field inside a retained trailer. This module recomputes
exactly that rule (any trailer key, "trailer" or "completion_trailer", is
dropped before hashing). The digest is self-consistency only, never a MAC
(F10 accepted limitation): it detects corruption/truncation, not forgery;
an actor who can rewrite a manifest can recompute its trailer digest and
must never be cited as tamper evidence for the evidence store.

Key-name accommodation (the consumer must accept the manifest JSON the
producer writes): the producer writes "trailer", "task_id", "start_ns" and
"end_ns"; the contract names these "completion_trailer", "task_window_id",
"started_ns" and "ended_ns". Either name is accepted and normalized to the
contract name in the returned copy. Two spellings of one alias pair present
in the same manifest are rejected (manifest_malformed) unless the values
are identical -- contradictory spellings are evidence ambiguity, not a
normalization opportunity. The producer's capabilities section is a list of
per-root probe records and its git supplement key is "git"; both are
accepted (a capabilities dict is also accepted), since the producer's
manifest must validate as written. Name-encoded values must use the
producer's canonical spelling (tag "u" iff the bytes are valid UTF-8, tag
"b" otherwise): a "b"-tagged payload that decodes to valid UTF-8 is
rejected as manifest_malformed so one byte string can never have two JSON
spellings.

Validation order (first failure wins): manifest_malformed (not a JSON
object; entries not a list; trailer/capabilities wrong container; wrong
primitive types on required fields; conflicting alias-key spellings;
non-canonical name encodings), manifest_missing_field, then
manifest_schema_version != "ppic/1", manifest_incomplete (trailer complete
is not True), manifest_entry_count_mismatch, manifest_entry_order (entries
strictly increasing by the producer's exact sort key (root, raw path
bytes); duplicates violate), manifest_traversal_errors (any error present),
manifest_digest_mismatch. Entry "root" is a required integer field: a
missing or non-integer root is a validation failure, never a silently
narrowed comparison.

Compared entry fields: root (part of the entry key), dev, ino, type, mode,
uid, gid, nlink, size, mtime_ns, ctime_ns, sha256, rdev (absent means None),
link_target, external_target (symlink reclassification is compared) and the
capability-conditioned fields xattrs, acls, inode_flags (absent means None,
exact equality). Entries are keyed by (root, encoded path) -- never by path
alone -- so same-relative-path entries under different roots cannot shadow
each other. A capability-conditioned field is compared only when both
manifests' capabilities report that capability "supported" for the entry's
root; any disagreement between the manifests' capability values is
gate_failed/capability_mismatch. fields_compared counts the total number of
per-entry field comparisons performed (sum over compared entries).

Cross-manifest header/evidence equality (matching the producer's
equivalence() semantics): target_identity, boot_id, tool_digest,
tool_version, roots, link_exceptions, entry_count and bytes_hashed must be
equal across the pair, and so must the git section content (below).
schema_version equality is already enforced by validation. A mismatch in
any of these fields makes the verdict "different" (exit 1) with one
{"field": <name>, "kind": "changed"} difference record per mismatching
field -- graded "different" rather than gate_failed because both manifests
are complete and valid and the producer's equivalence() likewise refuses
equality (header_mismatch / body_differs) without declaring either
manifest invalid; only "equivalent" (exit 0) is ever accepted, so the
grading is fail-closed. Timing fields (started_ns, ended_ns) and the
window id (task_window_id) stay diagnostics-only: they vary per run and are
never equivalence evidence.

Git section (documented choice): the producer's equivalence() treats the
git section as evidence (EVIDENCE_KEYS), so this consumer matches that and
grades a git content drift "different"; the comparison is the per-repository
git state content (result, reason, git_kind, gitdir, status, tracked,
untracked lines and repository presence) while the probe-command metadata
(name, returncode, stderr digest) is diagnostic-only -- it is
environment-sensitive (git config and inherited environment, F12) and must
not flip verdicts. A supplement absent from either side is "no git
evidence" and is not compared. The artifact's git_supplement section still
records the detailed per-repository drift either way.

Accepted limitations (documented per review): (F9) ctime-based
write-then-revert detection is cross-tick only -- a transient write fully
reverted in content and compared metadata within one filesystem ctime tick
leaves no compared-field difference and is undetectable by design; window
serialization/quiescence is the mitigation, and persistent changes always
alter a non-ctime compared field. (F10) the manifest digest is
self-consistency, not tamper evidence. (F12) the target identity is an
unauthenticated caller-provided string; git output is environment-sensitive;
the evidence receipt does not bind task/window ids to the certified window
(defense-in-depth left to the runner harness).
"""
from __future__ import annotations

import base64
import copy
import hashlib
import json
import time
from collections.abc import Mapping
from dataclasses import dataclass
from pathlib import Path
from typing import Final

from .kernel import Blocked
from .snapshot_codec import decode_name

# allow: SIZE_OK — spec mandates one module owning the full consumer contract
# (validate + diff + artifact + receipt); splitting would break the required API surface.

SCHEMA_VERSION: Final = "ppic/1"
DIFF_SCHEMA_VERSION: Final = "ppic-diff/1"
RECEIPT_KIND: Final = "protected-path-window/1"

_KEY_ALIASES: Final[tuple[tuple[str, tuple[str, ...]], ...]] = (
    ("task_window_id", ("task_window_id", "task_id")),
    ("started_ns", ("started_ns", "start_ns")),
    ("ended_ns", ("ended_ns", "end_ns")),
    ("completion_trailer", ("completion_trailer", "trailer")),
)

# Required header fields in contract order; each entry lists accepted keys.
_REQUIRED_HEADERS: Final[tuple[tuple[str, tuple[str, ...]], ...]] = (
    ("schema_version", ("schema_version",)),
    _KEY_ALIASES[0],
    ("target_identity", ("target_identity",)),
    ("boot_id", ("boot_id",)),
    ("tool_digest", ("tool_digest",)),
    ("tool_version", ("tool_version",)),
    ("roots", ("roots",)),
    ("link_exceptions", ("link_exceptions",)),
    _KEY_ALIASES[1],
    _KEY_ALIASES[2],
    ("entry_count", ("entry_count",)),
    ("bytes_hashed", ("bytes_hashed",)),
    ("traversal_errors", ("traversal_errors",)),
    ("capabilities", ("capabilities",)),
    ("entries", ("entries",)),
    _KEY_ALIASES[3],
)

_REQUIRED_ENTRY_FIELDS: Final[tuple[str, ...]] = (
    "root", "path", "type", "dev", "ino", "mode", "uid", "gid", "nlink", "size",
    "mtime_ns", "ctime_ns", "sha256",
)

_BASE_FIELDS: Final[tuple[str, ...]] = (
    "dev", "ino", "type", "mode", "uid", "gid", "nlink", "size",
    "mtime_ns", "ctime_ns", "sha256", "rdev", "link_target", "external_target",
)
_CAP_FIELDS: Final[tuple[str, ...]] = ("xattrs", "acls", "inode_flags")

# Must-match cross-manifest fields (F4 grading: mismatch -> verdict "different";
# git via _git_supplement content). See module docstring for the full contract.
_COMPARED_HEADERS: Final[tuple[str, ...]] = (
    "target_identity", "boot_id", "tool_digest", "tool_version",
    "roots", "link_exceptions", "entry_count", "bytes_hashed",
)

_DIAG_FIELDS: Final[tuple[str, ...]] = (
    "started_ns", "ended_ns", "task_window_id", "tool_digest", "boot_id",
    "entry_count", "bytes_hashed", "manifest_digest",
)

_INT_HEADERS: Final = frozenset({"started_ns", "ended_ns", "entry_count", "bytes_hashed"})
_STR_HEADERS: Final = frozenset({
    "schema_version", "task_window_id", "target_identity", "boot_id", "tool_digest",
    "tool_version"})
_LIST_HEADERS: Final = frozenset({"roots", "link_exceptions", "traversal_errors", "entries"})


@dataclass(frozen=True, slots=True)
class DiffResult:
    """One before/after comparison outcome; also the artifact's summary row."""

    verdict: str
    exit_status: int
    before_digest: str | None
    after_digest: str | None
    entries_compared: int
    fields_compared: int
    reason_codes: tuple[str, ...]
    differences: tuple[dict[str, object], ...]
    artifact_path: str


def _canonical_digest(data: Mapping[str, object]) -> str:
    """Producer rule: SHA-256 of the canonical body with the trailer key removed."""
    body = {key: value for key, value in data.items()
            if key not in ("completion_trailer", "trailer")}
    return hashlib.sha256(json.dumps(
        body, sort_keys=True, separators=(",", ":"), ensure_ascii=True,
    ).encode("utf-8")).hexdigest()


def _present_key(data: Mapping[str, object], keys: tuple[str, ...]) -> str | None:
    for key in keys:
        if key in data:
            return key
    return None


def _check_header_types(data: Mapping[str, object]) -> None:
    for canonical, keys in _REQUIRED_HEADERS:
        key = _present_key(data, keys)
        if key is None:
            continue
        value = data[key]
        if canonical in _INT_HEADERS:
            ok = type(value) is int
        elif canonical in _STR_HEADERS:
            ok = type(value) is str
        elif canonical in _LIST_HEADERS:
            ok = isinstance(value, list)
        elif canonical == "completion_trailer":
            ok = isinstance(value, dict)
        else:  # capabilities: dict (contract) or list of records (producer form)
            ok = isinstance(value, dict) or (
                isinstance(value, list) and all(isinstance(item, dict) for item in value))
        if not ok:
            raise Blocked("manifest_malformed", f"{key} type {type(value).__name__}")


def _check_name_pair(value: object, where: str) -> None:
    """Shape plus canonical spelling of one ['u'|'b', payload] name pair."""
    if not (isinstance(value, list) and len(value) == 2
            and all(type(item) is str for item in value)):
        raise Blocked("manifest_malformed", f"{where} name shape {type(value).__name__}")
    tag, payload = value[0], value[1]
    if tag == "u":
        return
    if tag != "b":
        raise Blocked("manifest_malformed", f"{where} name tag {tag!r}")
    try:
        raw = base64.b64decode(payload.encode("ascii"), validate=True)
    except ValueError as error:
        raise Blocked("manifest_malformed", f"{where} name base64 {error}") from error
    try:
        raw.decode("utf-8")
    except UnicodeDecodeError:
        return
    raise Blocked("manifest_malformed", f"{where} name encoding not canonical")


def _check_alias_conflicts(data: Mapping[str, object]) -> None:
    for _, keys in _KEY_ALIASES:
        present = [key for key in keys if key in data]
        first = data[present[0]] if present else None
        for key in present[1:]:
            if data[key] != first:
                raise Blocked("manifest_malformed",
                              f"conflicting keys {present[0]}/{key}")


def _check_entry_types(data: Mapping[str, object]) -> list[dict[str, object]]:
    """Malformed phase for entry items; returns the entries narrowed to dicts."""
    if "entries" not in data:
        return []
    raw = data["entries"]
    if not isinstance(raw, list):
        raise Blocked("manifest_malformed", f"entries type {type(raw).__name__}")
    entries: list[dict[str, object]] = []
    for index, entry in enumerate(raw):
        if not isinstance(entry, dict):
            raise Blocked("manifest_malformed", f"entries[{index}] type {type(entry).__name__}")
        for field in _REQUIRED_ENTRY_FIELDS:
            if field not in entry:
                continue
            value = entry[field]
            if field == "path":
                ok = (isinstance(value, list) and len(value) == 2
                      and all(type(item) is str for item in value))
            elif field == "type":
                ok = type(value) is str
            elif field == "size":
                ok = value is None or type(value) is int
            elif field == "sha256":
                ok = value is None or type(value) is str
            else:
                ok = type(value) is int
            if not ok:
                raise Blocked("manifest_malformed", f"entries[{index}].{field} "
                                                    f"type {type(value).__name__}")
        if "path" in entry:
            _check_name_pair(entry["path"], f"entries[{index}].path")
        if entry.get("link_target") is not None:
            _check_name_pair(entry["link_target"], f"entries[{index}].link_target")
        entries.append(entry)
    return entries


def _check_header_name_pairs(data: Mapping[str, object]) -> None:
    roots = data.get("roots")
    if isinstance(roots, list):
        for index, item in enumerate(roots):
            _check_name_pair(item, f"roots[{index}]")
    exceptions = data.get("link_exceptions")
    if isinstance(exceptions, list):
        for index, item in enumerate(exceptions):
            if not isinstance(item, dict):
                raise Blocked("manifest_malformed",
                              f"link_exceptions[{index}] type {type(item).__name__}")
            for key in ("path", "expected_target"):
                if key in item:
                    _check_name_pair(item[key], f"link_exceptions[{index}].{key}")


def _check_required_headers(data: Mapping[str, object]) -> None:
    for canonical, keys in _REQUIRED_HEADERS:
        if _present_key(data, keys) is None:
            raise Blocked("manifest_missing_field", canonical)


def _check_required_entries(entries: list[dict[str, object]]) -> None:
    for index, entry in enumerate(entries):
        for field in _REQUIRED_ENTRY_FIELDS:
            if field not in entry:
                raise Blocked("manifest_missing_field", f"entries[{index}].{field}")


def _check_entry_order(entries: list[dict[str, object]]) -> None:
    """Strict increase by the producer's exact sort key (root, raw path bytes).

    The JSON-encoded spelling is NOT the sort key: '["b",...]' sorts before
    '["u",...]' while the raw bytes of a non-UTF-8 name can sort either side
    of a UTF-8 name, so the encoded paths are decoded first and compared as
    bytes, exactly matching the producer's (entry.root, entry.path) sort.
    """
    keys: list[tuple[int, bytes]] = []
    for index, entry in enumerate(entries):
        try:
            keys.append((entry["root"], decode_name(entry["path"])))
        except (KeyError, TypeError, ValueError) as error:
            raise Blocked("manifest_malformed",
                          f"entries[{index}].path {error}") from error
    for index in range(len(keys) - 1):
        if not keys[index] < keys[index + 1]:
            raise Blocked("manifest_entry_order", f"entries[{index}] >= entries[{index + 1}]")


def _normalized(data: dict[str, object]) -> dict[str, object]:
    result = copy.deepcopy(data)
    for canonical, keys in _KEY_ALIASES:
        if canonical not in result:
            for key in keys:
                if key in result:
                    result[canonical] = result.pop(key)
                    break
        for key in keys:
            if key != canonical:
                result.pop(key, None)
    return result


def validate_manifest(data: object) -> dict[str, object]:
    """Return the normalized manifest dict if valid, else raise Blocked(code, detail)."""
    if not isinstance(data, dict):
        raise Blocked("manifest_malformed", f"top-level type {type(data).__name__}")
    _check_header_types(data)
    _check_alias_conflicts(data)
    entries = _check_entry_types(data)
    _check_header_name_pairs(data)
    _check_required_headers(data)
    _check_required_entries(entries)
    if data["schema_version"] != SCHEMA_VERSION:
        raise Blocked("manifest_schema_version", str(data["schema_version"]))
    trailer_key = _present_key(data, _KEY_ALIASES[3][1])
    trailer = data[trailer_key] if trailer_key is not None else {}
    if not isinstance(trailer, dict) or trailer.get("complete") is not True:
        raise Blocked("manifest_incomplete", repr(trailer.get("complete")))
    if data["entry_count"] != len(entries):
        raise Blocked("manifest_entry_count_mismatch",
                      f"{data['entry_count']} != {len(entries)}")
    _check_entry_order(entries)
    if data["traversal_errors"]:
        raise Blocked("manifest_traversal_errors", str(len(data["traversal_errors"])))
    if _canonical_digest(data) != trailer.get("manifest_digest"):
        raise Blocked("manifest_digest_mismatch", str(trailer.get("manifest_digest")))
    return _normalized(data)


def load_manifest(path: Path) -> dict[str, object]:
    """Read one retained manifest file and validate its content."""
    try:
        raw = path.read_bytes()
    except OSError as error:
        raise Blocked("manifest_malformed", str(error)) from error
    try:
        data = json.loads(raw)
    except json.JSONDecodeError as error:
        raise Blocked("manifest_malformed", str(error)) from error
    return validate_manifest(data)


def _path_key(path: object) -> str:
    return json.dumps(path, sort_keys=True, separators=(",", ":"), ensure_ascii=True)


def _capability_map(manifest: Mapping[str, object]) -> dict[tuple[int | None, str], object]:
    caps = manifest["capabilities"]
    result: dict[tuple[int | None, str], object] = {}
    if isinstance(caps, dict):
        for name in _CAP_FIELDS:
            result[(None, name)] = caps.get(name)
        return result
    for record in caps:  # producer form: one record per root
        if not isinstance(record, dict):
            continue
        root = record.get("root")
        root_key = root if type(root) is int else None
        for name in _CAP_FIELDS:
            result[(root_key, name)] = record.get(name)
    return result


def _cap_supported(caps: Mapping[tuple[int | None, str], object], name: str,
                   entry: Mapping[str, object]) -> bool:
    # entry["root"] is a required int (validation); missing root is a loud
    # failure here, never a silently skipped capability comparison (F7).
    value = caps.get((entry["root"], name), caps.get((None, name)))
    return value == "supported"


def _git_index(manifest: Mapping[str, object] | None) -> (
        dict[str, tuple[object, object, tuple[object, object, object]]] | None):
    if manifest is None or ("git_supplement" not in manifest and "git" not in manifest):
        return None
    raw = manifest["git_supplement"] if "git_supplement" in manifest else manifest["git"]
    records = raw if isinstance(raw, list) else [raw]
    index: dict[str, tuple[object, object, tuple[object, object, object]]] = {}
    for record in records:
        if isinstance(record, dict):
            root, path = record.get("root"), record.get("path")
            content = (record.get("result"), record.get("reason"),
                       record.get("git_kind"), record.get("gitdir"),
                       record.get("status"), record.get("tracked"),
                       record.get("untracked"))
        else:
            root, path, content = None, record, (record, None, None)
        index[f"{root}:{_path_key(path)}"] = (root, path, content)
    return index


def _git_supplement(before: dict[str, object] | None,
                    after: dict[str, object] | None) -> dict[str, object]:
    left, right = _git_index(before), _git_index(after)
    if left is None or right is None:
        return {"verdict": "absent"}
    differences: list[dict[str, object]] = []
    for key in sorted(set(left) | set(right)):
        lrec, rrec = left.get(key), right.get(key)
        if lrec is not None and rrec is not None:
            if lrec[2] != rrec[2]:
                fields = {name: {"before": lrec[2][i], "after": rrec[2][i]}
                          for i, name in enumerate(("result", "reason", "git_kind",
                                                    "gitdir", "status", "tracked",
                                                    "untracked"))
                          if lrec[2][i] != rrec[2][i]}
                differences.append({"root": lrec[0], "path": lrec[1], "kind": "changed",
                                    "fields": fields})
        elif lrec is not None:
            differences.append({"root": lrec[0], "path": lrec[1], "kind": "removed"})
        elif rrec is not None:
            differences.append({"root": rrec[0], "path": rrec[1], "kind": "added"})
    return {"verdict": "differing" if differences else "identical",
            "differences": differences}


def _diagnostics(manifest: dict[str, object] | None,
                 digest: str | None) -> dict[str, object]:
    if manifest is None:
        return {field: None for field in _DIAG_FIELDS}
    return {field: digest if field == "manifest_digest" else manifest.get(field)
            for field in _DIAG_FIELDS}


def _capability_detail(left: Mapping[tuple[int | None, str], object],
                       right: Mapping[tuple[int | None, str], object]) -> str:
    differing = {
        f"{name}@{root}": {"before": left.get((root, name)), "after": right.get((root, name))}
        for root, name in sorted(set(left) | set(right))
        if left.get((root, name), ...) != right.get((root, name), ...)
    }
    return json.dumps(differing, sort_keys=True, separators=(",", ":"), ensure_ascii=True)


def _entries_of(manifest: Mapping[str, object]) -> list[dict[str, object]]:
    raw = manifest["entries"]
    return [entry for entry in raw if isinstance(entry, dict)]


def _compare(before: dict[str, object], after: dict[str, object],
             before_digest: str | None, after_digest: str | None,
             artifact_path: str) -> tuple[DiffResult, list[tuple[str, str]]]:
    caps_left, caps_right = _capability_map(before), _capability_map(after)
    if caps_left != caps_right:
        detail = _capability_detail(caps_left, caps_right)
        result = DiffResult("gate_failed", 2, before_digest, after_digest, 0, 0,
                            ("capability_mismatch",), (), artifact_path)
        return result, [("capability_mismatch", detail)]
    differences: list[dict[str, object]] = []
    for field in _COMPARED_HEADERS:
        if before.get(field) != after.get(field):
            differences.append({"field": field, "kind": "changed"})
    if _git_supplement(before, after)["verdict"] == "differing":
        differences.append({"field": "git", "kind": "changed"})
    left = {(entry["root"], _path_key(entry["path"])): entry
            for entry in _entries_of(before)}
    right = {(entry["root"], _path_key(entry["path"])): entry
             for entry in _entries_of(after)}
    entries_compared = 0
    fields_compared = 0
    for key in sorted(set(left) | set(right)):
        lentry, rentry = left.get(key), right.get(key)
        if lentry is not None and rentry is not None:
            entries_compared += 1
            changed: dict[str, object] = {}
            for field in _BASE_FIELDS:
                fields_compared += 1
                if lentry.get(field) != rentry.get(field):
                    changed[field] = {"before": lentry.get(field), "after": rentry.get(field)}
            for field in _CAP_FIELDS:
                if (_cap_supported(caps_left, field, lentry)
                        and _cap_supported(caps_right, field, rentry)):
                    fields_compared += 1
                    if lentry.get(field) != rentry.get(field):
                        changed[field] = {"before": lentry.get(field),
                                          "after": rentry.get(field)}
            if changed:
                differences.append({"path": lentry["path"], "kind": "changed",
                                    "fields": changed})
        elif lentry is not None:
            differences.append({"path": lentry["path"], "kind": "removed"})
        elif rentry is not None:
            differences.append({"path": rentry["path"], "kind": "added"})
    if differences:
        result = DiffResult("different", 1, before_digest, after_digest,
                            entries_compared, fields_compared, (),
                            tuple(differences), artifact_path)
    else:
        result = DiffResult("equivalent", 0, before_digest, after_digest,
                            entries_compared, fields_compared, (), (), artifact_path)
    return result, []


def _write_json(path: Path, payload: dict[str, object]) -> None:
    blob = json.dumps(payload, sort_keys=True, separators=(",", ":"),
                      indent=None).encode("utf-8") + b"\n"
    with open(path, "wb") as stream:
        stream.write(blob)
        stream.flush()


def diff_manifests(before_path: Path, after_path: Path, artifact_path: Path) -> DiffResult:
    """Validate and compare two manifests; retain the deterministic diff artifact."""
    loaded: dict[str, tuple[dict[str, object] | None, str | None]] = {}
    reasons: list[tuple[str, str]] = []
    for name, path in (("before", before_path), ("after", after_path)):
        try:
            manifest = load_manifest(path)
        except Blocked as error:
            loaded[name] = (None, None)
            reasons.append((error.reason, error.detail))
        else:
            trailer = manifest["completion_trailer"]
            digest = trailer.get("manifest_digest") if isinstance(trailer, dict) else None
            loaded[name] = (manifest, digest if isinstance(digest, str) else None)
    before, before_digest = loaded["before"]
    after, after_digest = loaded["after"]
    git_supplement = _git_supplement(before, after)
    if before is None or after is None:
        result = DiffResult("gate_failed", 2, before_digest, after_digest, 0, 0,
                            tuple(code for code, _ in reasons), (), str(artifact_path))
    else:
        result, compare_reasons = _compare(before, after, before_digest, after_digest,
                                           str(artifact_path))
        reasons = compare_reasons
    artifact: dict[str, object] = {
        "schema_version": DIFF_SCHEMA_VERSION,
        "verdict": result.verdict,
        "exit_status": result.exit_status,
        "reason_codes": [{"code": code, "detail": detail} for code, detail in reasons],
        "before_digest": result.before_digest,
        "after_digest": result.after_digest,
        "entries_compared": result.entries_compared,
        "fields_compared": result.fields_compared,
        "differences": list(result.differences),
        "git_supplement": git_supplement,
        "diagnostics": {"before": _diagnostics(before, before_digest),
                        "after": _diagnostics(after, after_digest)},
    }
    _write_json(artifact_path, artifact)
    return result


def _file_sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def _manifest_window_id(path: Path) -> str | None:
    """Lenient window-id read for receipt binding: the manifest's window id when
    the file parses as a JSON object, else None. Unparseable manifests are
    already gate_failed via diff_manifests; the receipt records them as
    unverifiable rather than refusing, so failure evidence is never lost."""
    try:
        data = json.loads(path.read_bytes())
    except (OSError, ValueError):
        return None
    if not isinstance(data, dict):
        return None
    value = data.get("task_window_id", data.get("task_id"))
    return value if isinstance(value, str) else None


def evidence_receipt(task_window_id: str, before_path: Path, after_path: Path,
                     diff_artifact_path: Path, result: DiffResult,
                     window_trace_ref: str, output_path: Path) -> Path:
    """Write the protected-path-window/1 receipt over the three evidence files.

    Window binding (round-2 review C1): each manifest that parses must carry
    the certified window id; a stale manifest from an earlier window at a
    reused path refuses the receipt instead of certifying old evidence.
    """
    window_ids: dict[str, str | None] = {}
    for label, path in (("before", before_path), ("after", after_path)):
        window_id = _manifest_window_id(path)
        if window_id is not None and window_id != task_window_id:
            raise Blocked("window_id_mismatch",
                          f"{label}: {window_id!r} != {task_window_id!r}")
        window_ids[label] = window_id
    receipt: dict[str, object] = {
        "receipt": RECEIPT_KIND,
        "task_window_id": task_window_id,
        "manifest_window_ids": window_ids,
        "before_manifest": {"path": str(before_path), "sha256": _file_sha256(before_path)},
        "after_manifest": {"path": str(after_path), "sha256": _file_sha256(after_path)},
        "diff_artifact": {"path": str(diff_artifact_path),
                          "sha256": _file_sha256(diff_artifact_path)},
        "window_trace": window_trace_ref,
        "verdict": result.verdict,
        "exit_status": result.exit_status,
        "recorded_ns": time.time_ns(),
    }
    _write_json(output_path, receipt)
    return output_path
