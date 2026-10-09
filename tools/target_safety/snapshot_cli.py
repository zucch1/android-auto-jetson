"""CLI driver for the ppic/1 protected-path snapshot producer (measurement entry point).

The producer (snapshot.py) had no command surface; this driver binds its typed
SnapshotRequest to argv and prints exactly one final JSON line on stdout, so a
single bounded transport can capture the run outcome without parsing prose.

Arguments: --root PATH (repeatable, order preserved as root indices),
--link-exception path=target (repeatable, split on the FIRST '='),
--manifest PATH (retention destination, outside the roots), --task-id and
--target-identity (recorded verbatim in the manifest envelope). The run uses a
REAL clock (time.time_ns) and deadline_seconds=840, which sits inside the
recorded 900 s remote budget of task-5-traversal-measurement-budget/1.

Output contract (one compact JSON line, flush=True):
  ok     {"status":"ok","entry_count":N,"bytes_hashed":B,"elapsed_seconds":F,
          "traversal_errors":N,"capabilities":[...],
          "manifest_sha256":"...","manifest_bytes":N}
  blocked {"status":"blocked","reason":"...","detail":"..."}

exit codes: 0 on ok, 70 (EX_SOFTWARE) on a producer Blocked, 2 on usage errors.
elapsed_seconds is the producer interval (manifest end_ns - start_ns; the
manifest serialization/write happens after end_ns by producer design).
manifest_sha256 is SHA-256 of the retained manifest FILE bytes and
manifest_bytes its size -- the pair identifies exactly what a retrieval must
land, so a transported copy can be checked byte-for-byte. Unexpected
non-Blocked exceptions propagate (stderr traceback, exit 1): fail-closed and
visible, never converted into a summary line.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import time
from collections.abc import Sequence
from dataclasses import dataclass
from pathlib import Path
from typing import Final

from .kernel import Blocked
from .snapshot import snapshot
from .snapshot_envelope import SnapshotManifest
from .snapshot_models import LinkException, SnapshotRequest

DEADLINE_SECONDS: Final = 840.0
EXIT_OK: Final = 0
EXIT_BLOCKED: Final = 70


@dataclass(frozen=True, slots=True)
class CliArgs:
    """Typed driver inputs, parsed once at the argv boundary."""

    roots: tuple[Path, ...]
    link_exceptions: tuple[LinkException, ...]
    manifest: Path
    task_id: str
    target_identity: str


def _link_exception(text: str) -> LinkException:
    """Parse one 'path=target' pair, splitting on the first '=' only."""
    path, sep, target = text.partition("=")
    if not sep or not path or not target:
        raise argparse.ArgumentTypeError(f"expected path=target, got {text!r}")
    return LinkException(path=Path(path), expected_target=target)


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="snapshot_cli",
        description="One-shot ppic/1 protected-path snapshot measurement driver.")
    parser.add_argument("--root", action="append", type=Path, required=True,
                        metavar="PATH", help="protected root (repeatable, order = root index)")
    parser.add_argument("--link-exception", action="append", type=_link_exception,
                        default=[], metavar="PATH=TARGET",
                        help="approved namespace-only link (repeatable)")
    parser.add_argument("--manifest", type=Path, required=True,
                        help="manifest retention path (outside the roots)")
    parser.add_argument("--task-id", default="", help="window id recorded in the envelope")
    parser.add_argument("--target-identity", default="",
                        help="caller-provided target identity recorded verbatim")
    return parser


def parse(argv: Sequence[str] | None = None) -> CliArgs:
    """Parse argv into typed inputs; usage errors exit 2 through argparse."""
    namespace = _parser().parse_args(argv)
    return CliArgs(
        roots=tuple(namespace.root),
        link_exceptions=tuple(namespace.link_exception),
        manifest=namespace.manifest,
        task_id=namespace.task_id,
        target_identity=namespace.target_identity,
    )


def _summary(manifest_path: Path, manifest: SnapshotManifest) -> dict[str, object]:
    """Build the ok summary from the producer result and the retained file."""
    blob = manifest_path.read_bytes()
    return {
        "status": "ok",
        "entry_count": len(manifest.entries),
        "bytes_hashed": manifest.bytes_hashed,
        "elapsed_seconds": (manifest.end_ns - manifest.start_ns) / 1e9,
        "traversal_errors": len(manifest.traversal_errors),
        "capabilities": [record.jsonable() for record in manifest.capabilities],
        "manifest_sha256": hashlib.sha256(blob).hexdigest(),
        "manifest_bytes": len(blob),
    }


def run(args: CliArgs) -> int:
    """Produce one snapshot and print its single summary line; 0 ok, 70 blocked."""
    request = SnapshotRequest(
        roots=args.roots,
        link_exceptions=args.link_exceptions,
        task_id=args.task_id,
        target_identity=args.target_identity,
        now_ns=time.time_ns,
        deadline_seconds=DEADLINE_SECONDS,
    )
    try:
        manifest = snapshot(request, args.manifest)
    except Blocked as error:
        print(json.dumps({"status": "blocked", "reason": error.reason,
                          "detail": error.detail}, separators=(",", ":")), flush=True)
        return EXIT_BLOCKED
    print(json.dumps(_summary(args.manifest, manifest), separators=(",", ":")), flush=True)
    return EXIT_OK


def main(argv: Sequence[str] | None = None) -> int:
    return run(parse(argv))
