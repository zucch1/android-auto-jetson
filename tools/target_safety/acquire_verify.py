"""Local validation of complete returned acquisition evidence and copied bytes."""
from __future__ import annotations

import hashlib
import json
import os
import tarfile
from contextlib import ExitStack
from pathlib import Path
from typing import Protocol, TypedDict

from tools.sysroot.filesystem import directory, validate_tree
from tools.sysroot.manifest import encode_manifest, parse_manifest
from tools.sysroot.models import SysrootError
from .acquire_manifest import build_manifest
from .acquire_models import (AcquisitionRecord, CopiedEntry, PackageInfo, ProcessObservation,
                             TargetIdentity)
from .acquire_readers import assess_quiet
from .kernel import Blocked
from .snapshot_codec import encode_name
from .snapshot_diff import diff_manifests, load_manifest

EVIDENCE = ("before.json", "after.json", "diff.json", "receipt.json", "acquisition.json")


class ReturnScope(Protocol):
    roots: tuple[str, ...]
    link_exceptions: tuple[str, ...]
    window_id: str
    target_identity: str
    remote_scratch: str
    observation: bytes | None
    fixture: bool


class Verification(TypedDict):
    equivalent: bool
    verdict: str
    rootfs_bytes: int
    evidence_bytes: int
    hashes: dict[str, str]
    entries_verified: int


def extract_returned(archive: Path, returned: Path, cap: int) -> None:
    with tarfile.open(archive) as tar:
        members = tar.getmembers()
        names = set()
        total = 0
        for member in members:
            path = Path(member.name)
            if (path.is_absolute() or ".." in path.parts or member.name in names
                    or not (member.isfile() or member.isdir() or member.issym())
                    or not (member.name in EVIDENCE or path.parts[0] == "snapshot")):
                raise Blocked("archive_member_unsafe", member.name)
            names.add(member.name)
            total += member.size
        if total > cap:
            raise Blocked("archive_expanded_bound", str(total))
        tar.extractall(returned, members=members, filter="data")


def verify_returned(returned: Path, cfg: ReturnScope) -> Verification:
    from .acquire_driver import load_budget
    budget = load_budget()["transfers"]
    evidence_bytes = sum((returned / name).stat().st_size for name in EVIDENCE)
    if evidence_bytes > budget["evidence_bytes_bound"]:
        raise Blocked("evidence_bound_exceeded", str(evidence_bytes))
    receipt = json.loads((returned / "receipt.json").read_bytes())
    hashes = {name: hashlib.sha256((returned / name).read_bytes()).hexdigest() for name in EVIDENCE}
    for field, name in (("before_manifest", "before.json"), ("after_manifest", "after.json"),
                        ("diff_artifact", "diff.json"), ("acquisition_record", "acquisition.json")):
        if receipt[field]["sha256"] != hashes[name]:
            raise Blocked("receipt_hash_mismatch", name)
    if receipt["task_window_id"] != cfg.window_id:
        raise Blocked("window_id_mismatch", "receipt")
    expected_roots = [list(encode_name(os.fsencode(os.path.normpath(r)))) for r in cfg.roots]
    expected_links = [{"path": list(encode_name(os.fsencode(p))), "expected_target": list(encode_name(os.fsencode(t)))}
                      for p, t in (link.split("=", 1) for link in cfg.link_exceptions)]
    for name in ("before.json", "after.json"):
        manifest = load_manifest(returned / name)
        if (manifest["task_window_id"] != cfg.window_id
                or manifest["target_identity"] != cfg.target_identity
                or manifest["roots"] != expected_roots or manifest["link_exceptions"] != expected_links):
            raise Blocked("window_configuration_mismatch", name)
    local_diff = returned.parent / "local-diff.json"
    diff = diff_manifests(returned / "before.json", returned / "after.json", local_diff)
    if json.loads(local_diff.read_bytes()) != json.loads((returned / "diff.json").read_bytes()):
        raise Blocked("returned_diff_mismatch", "recomputed diff")
    raw = json.loads((returned / "acquisition.json").read_bytes())
    if (raw["schema"] != "aa-acquire/1" or raw["task_window_id"] != cfg.window_id
            or raw["scratch"] != cfg.remote_scratch or raw["completed"] is not True
            or raw["error"] is not None or receipt["acquisition_record"]["completed"] is not True
            or raw["provenance"] != ("fixture" if cfg.observation is not None or cfg.fixture else "observed")):
        raise Blocked("acquisition_incomplete", "record")
    samples = tuple(ProcessObservation(tuple(tuple(p) for p in s["processes"]), None, s["complete"])
                    for s in raw["boundary_samples"])
    roots = tuple(Path(r) for r in cfg.roots)
    if (len(samples) != 4 or any(s.complete is not True for s in samples)
            or not all(assess_quiet(a, b, roots).proven for a, b in zip(samples, samples[1:]))):
        raise Blocked("quiet_boundary_unproven", "returned samples")
    quiet = assess_quiet(samples[2], samples[3], roots)
    entries = tuple(CopiedEntry(e["original_input_path"], e["kind"], e["copied_path"], e["sha256"],
                    e["size"], e["link_text"], PackageInfo(**e["package"]),
                    e["original_link_text"], e["original_link_sha256"]) for e in raw["entries"])
    for entry in entries:
        if entry.kind == "symlink":
            original = entry.original_link_text
            if original is None or hashlib.sha256(original.encode()).hexdigest() != entry.original_link_sha256:
                raise Blocked("original_link_digest_mismatch", entry.copied_path)
            expected = os.path.relpath(original, os.path.dirname(entry.original_input_path)) if original.startswith("/") else original
            if entry.link_text != expected:
                raise Blocked("link_relocation_mismatch", entry.copied_path)
    if (not entries or len(set(raw["selected_inputs"])) != len(entries)
            or set(raw["selected_inputs"]) != {e.original_input_path for e in entries}
            or any(e.copied_path != e.original_input_path[1:] for e in entries)):
        raise Blocked("acquisition_inventory_mismatch", "selected/copied")
    identity = raw["identity"]
    record = AcquisitionRecord(raw["schema"], cfg.window_id, raw["provenance"],
        TargetIdentity(identity["kernel"], identity["l4t"], identity["jetpack"],
                       tuple(tuple(p) for p in identity["dpkg_summary"])), entries, quiet, raw["scratch"])
    try:
        manifest = parse_manifest(encode_manifest(build_manifest(record, cfg.window_id)))
        with ExitStack() as stack:
            validate_tree(directory(returned / "snapshot", stack), manifest)
    except SysrootError as error:
        raise Blocked("returned_sysroot_invalid", str(error)) from error
    rootfs_bytes = sum((returned / "snapshot" / e.copied_path).lstat().st_size
                       for e in entries if e.kind == "regular")
    if rootfs_bytes > budget["rootfs_bytes_bound"]:
        raise Blocked("rootfs_bound_exceeded", str(rootfs_bytes))
    if any(e.kind == "regular" and (returned / "snapshot" / e.copied_path).stat().st_size != e.size
           for e in entries):
        raise Blocked("returned_size_mismatch", "inputs")
    return {"equivalent": diff.verdict == "equivalent", "verdict": diff.verdict,
            "rootfs_bytes": rootfs_bytes, "evidence_bytes": evidence_bytes,
            "hashes": hashes, "entries_verified": len(entries)}
