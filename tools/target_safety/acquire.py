# SPDX-License-Identifier: GPL-3.0-or-later
"""One inventoried, quiet-gated task-5 acquisition window in /tmp-only scratch.

Selection, identity and input reads are bracketed by protected-path inventories.
Read failures retain after evidence best effort; missing evidence never passes.
Four process/cwd samples gate acquisition and acceptance, without a writer-watch
claim. Fixture observations remain labeled fixture. Record and copied-content
digests are bound to the receipt for local sysroot validation. Bundle is stdlib-only.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import stat
import subprocess
from dataclasses import replace
from collections.abc import Callable, Sequence
from pathlib import Path
from typing import Final

from .acquire_models import (
    AcquisitionRecord,
    CopiedEntry,
    InputSpec,
    PackageInfo,
    ProcessObservation,
    QUIET_NOTE,
    RECORD_SCHEMA,
    QuietBoundary,
    TargetIdentity,
    WindowRequest,
    WindowResult,
    load_input_set,
)
from .acquire_readers import assess_quiet, live_identity, live_package_of, observe_processes
from .kernel import Blocked
from .snapshot import snapshot
from .snapshot_diff import diff_manifests, evidence_receipt
from .snapshot_models import LinkException, SnapshotRequest

__all__ = [
    "AcquisitionRecord", "CopiedEntry", "InputSpec", "PackageInfo", "QUIET_NOTE",
    "RECORD_SCHEMA", "QuietBoundary", "TargetIdentity", "WindowRequest", "WindowResult",
    "acquire", "load_input_set", "live_identity", "live_package_of", "main", "run_window",
]

EXIT_OK: Final = 0
EXIT_NOT_EQUIVALENT: Final = 1
EXIT_BLOCKED: Final = 70
# Fail-closed bound on the copied input tree, keeping the target->local transfer
# finite. Fixed, never auto-grows; enforced while streaming, not after a whole copy.
COPY_BYTE_LIMIT: Final = 2_147_483_648


def _snapshot_request(request: WindowRequest) -> SnapshotRequest:
    return SnapshotRequest(
        roots=request.roots,
        link_exceptions=request.link_exceptions,
        task_id=request.task_window_id,
        target_identity=request.target_identity,
        now_ns=request.now_ns,
        deadline_seconds=request.snapshot_deadline_seconds,
    )


def _copy_regular(src: str, dest: Path, counter: list[int]) -> tuple[str, int]:
    """No-follow streaming read; the byte bound is enforced as bytes flow."""
    fd = os.open(src, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK)
    try:
        dest.parent.mkdir(parents=True, exist_ok=True)
        hasher = hashlib.sha256()
        size = 0
        with os.fdopen(fd, "rb") as stream:
            fd = -1
            with dest.open("wb") as out:
                while chunk := stream.read(1024 * 1024):
                    counter[0] += len(chunk)
                    if counter[0] > COPY_BYTE_LIMIT:
                        raise Blocked("acquire_transfer_byte_limit", str(counter[0]))
                    hasher.update(chunk)
                    out.write(chunk)
                    size += len(chunk)
        return hasher.hexdigest(), size
    finally:
        if fd >= 0:
            os.close(fd)


def _stage_entry(spec: InputSpec, snapshot_dir: Path,
                 package_of: Callable[[str], PackageInfo],
                 counter: list[int]) -> CopiedEntry:
    """Copy one input read-only into the snapshot tree and record its evidence."""
    src = spec.path
    rel = src[1:]
    dest = snapshot_dir / rel
    info = os.lstat(src)
    package = package_of(src)
    if stat.S_ISREG(info.st_mode):
        sha256, size = _copy_regular(src, dest, counter)
        return CopiedEntry(src, "regular", rel, sha256, size, None, package)
    if stat.S_ISLNK(info.st_mode):
        original = os.readlink(src)
        link_text = os.path.relpath(original, os.path.dirname(src)) if original.startswith("/") else original
        dest.parent.mkdir(parents=True, exist_ok=True)
        os.symlink(link_text, dest)
        sha256 = hashlib.sha256(link_text.encode("utf-8")).hexdigest()
        return CopiedEntry(src, "symlink", rel, sha256, None, link_text, package,
                           original, hashlib.sha256(original.encode("utf-8")).hexdigest())
    raise Blocked("input_not_file_or_link", src)


def _stage_inputs(request: WindowRequest) -> tuple[CopiedEntry, ...]:
    if not request.input_set:
        raise Blocked("acquisition_inputs_empty", request.task_window_id)
    counter = [0]
    return tuple(_stage_entry(spec, request.scratch / "snapshot", request.package_of, counter)
                 for spec in request.input_set)


def acquire(request: WindowRequest) -> AcquisitionRecord:
    """Read-only acquisition of the selected inputs + identity into scratch."""
    (request.scratch / "snapshot").mkdir(parents=True, exist_ok=True)
    return AcquisitionRecord(
        schema=RECORD_SCHEMA,
        task_window_id=request.task_window_id,
        provenance="observed",
        identity=request.identity(),
        entries=_stage_inputs(request),
        quiet_boundary=assess_quiet(observe_processes(), observe_processes(), request.roots),
        scratch=str(request.scratch),
    )


def run_window(request: WindowRequest) -> WindowResult:
    """Before -> acquire -> after -> equivalence -> receipt; after runs even on failure."""
    scratch = request.scratch
    if not scratch.is_absolute() or not str(scratch).startswith("/tmp/") or ".." in scratch.parts:
        raise Blocked("scratch_not_tmp", str(scratch))
    (scratch / "snapshot").mkdir(parents=True, exist_ok=True)
    before_path = scratch / "before.json"
    after_path = scratch / "after.json"
    diff_path = scratch / "diff.json"
    receipt_path = scratch / "receipt.json"
    record_path = scratch / "acquisition.json"

    before_obs = observe_processes()
    snapshot(_snapshot_request(request), before_path)
    post_before = observe_processes()
    entries: tuple[CopiedEntry, ...] = ()
    identity = TargetIdentity(None, None, None, ())
    acquire_error: Blocked | None = None
    pre_acquire = observe_processes()
    try:
        if not (assess_quiet(before_obs, post_before, request.roots).proven
                and assess_quiet(post_before, pre_acquire, request.roots).proven):
            raise Blocked("quiet_boundary_unproven", "before acquisition")
        if request.select is not None:
            observation = request.select()
            request = replace(request, input_set=observation.specs,
                              identity=lambda: observation.identity,
                              package_of=observation.packages.__getitem__,
                              provenance=request.provenance if request.provenance == "fixture"
                              else observation.provenance)
        identity = request.identity()
        entries = _stage_inputs(request)
    except Blocked as error:
        acquire_error = error
    except (OSError, subprocess.SubprocessError) as error:
        acquire_error = Blocked("acquisition_read_failed", str(error))
    try:
        snapshot(_snapshot_request(request), after_path)
    except (Blocked, OSError) as error:
        acquire_error = Blocked("after_inventory_failed", str(error))
    post_after = observe_processes()
    quiet = assess_quiet(pre_acquire, post_after, request.roots)
    if not quiet.proven and acquire_error is None:
        acquire_error = Blocked("quiet_boundary_unproven", "after acquisition")
    record = AcquisitionRecord(RECORD_SCHEMA, request.task_window_id, "observed",
                               identity, entries, quiet, str(scratch),
                               acquire_error is None,
                               str(acquire_error) if acquire_error else None,
                               tuple(s.path for s in request.input_set),
                               (before_obs, post_before, pre_acquire, post_after))
    record = replace(record, provenance=request.provenance)
    record_path.write_text(json.dumps(record.jsonable(), sort_keys=True,
                                      separators=(",", ":")) + "\n", encoding="utf-8")

    diff = diff_manifests(before_path, after_path, diff_path)
    if after_path.exists():
        evidence_receipt(request.task_window_id, before_path, after_path, diff_path,
                         diff, str(record_path), receipt_path)
        receipt = json.loads(receipt_path.read_bytes())
    else:
        receipt = {"task_window_id": request.task_window_id, "verdict": "gate_failed"}
    receipt["acquisition_record"] = {"sha256": hashlib.sha256(record_path.read_bytes()).hexdigest(),
                                     "completed": record.completed}
    receipt_path.write_text(json.dumps(receipt, sort_keys=True, separators=(",", ":")) + "\n")

    return WindowResult(scratch, scratch / "snapshot", before_path, after_path,
                        diff_path, receipt_path, record_path, diff, record, acquire_error)


def exit_code(verdict: str, acquire_error: Blocked | None) -> int:
    """Failed equivalence or a failed acquisition is a failed operation (nonzero)."""
    if acquire_error is not None:
        return EXIT_BLOCKED
    return EXIT_OK if verdict == "equivalent" else EXIT_NOT_EQUIVALENT


def _link_exception(text: str) -> LinkException:
    path, sep, target = text.partition("=")
    if not sep or not path or not target:
        raise argparse.ArgumentTypeError(f"expected path=target, got {text!r}")
    return LinkException(path=Path(path), expected_target=target)


def parse(argv: Sequence[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        prog="acquire",
        description="One-shot bounded task-5 target acquisition window.")
    parser.add_argument("--scratch", type=Path, required=True)
    parser.add_argument("--input-set", type=Path, default=None,
                        help="captured observation JSON; omit to select live on target")
    parser.add_argument("--root", action="append", type=Path, required=True)
    parser.add_argument("--link-exception", action="append", type=_link_exception,
                        default=[], metavar="PATH=TARGET")
    parser.add_argument("--task-window-id", required=True)
    parser.add_argument("--target-identity", required=True)
    return parser.parse_args(argv)


def main(argv: Sequence[str] | None = None) -> int:
    args = parse(argv)
    from .acquire_input_select import select_inputs
    if args.input_set is not None:
        select = lambda: load_input_set(args.input_set)
    else:
        select = select_inputs
    request = WindowRequest(
        roots=tuple(args.root),
        link_exceptions=tuple(args.link_exception),
        input_set=(),
        scratch=args.scratch,
        task_window_id=args.task_window_id,
        target_identity=args.target_identity,
        package_of=live_package_of,
        identity=live_identity,
        select=select,
        provenance="fixture" if args.input_set is not None else "observed",
    )
    try:
        result = run_window(request)
    except Blocked as error:
        print(json.dumps({"status": "blocked", "reason": error.reason,
                          "detail": error.detail}, separators=(",", ":")), flush=True)
        return EXIT_BLOCKED
    equivalent = result.diff.verdict == "equivalent" and result.acquire_error is None
    print(json.dumps({
        "status": "ok" if equivalent else "failed",
        "equivalent": result.diff.verdict == "equivalent",
        "verdict": result.diff.verdict,
        "acquire_error": None if result.acquire_error is None
                         else {"reason": result.acquire_error.reason,
                               "detail": result.acquire_error.detail},
        "entries": len(result.record.entries),
        "quiet_status": result.record.quiet_boundary.status,
        "scratch": str(result.scratch),
    }, separators=(",", ":")), flush=True)
    return exit_code(result.diff.verdict, result.acquire_error)


if __name__ == "__main__":
    raise SystemExit(main())
