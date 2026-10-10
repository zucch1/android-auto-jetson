# SPDX-License-Identifier: GPL-3.0-or-later
"""Runnable finite local driver: prepare -> transport -> retrieve -> verify.

One bounded target window over a single SSH connection (BatchMode, no retry,
outer + remote timeouts, durable stderr/exit records). The stdin payload is a
deterministic source tar of the route plus the frozen input policy; the target
returns one bounded tar archive holding before/after/diff/receipt/acquisition
evidence and the rootfs. Output is streamed under fixed limits (512 MiB evidence,
<=2 GiB rootfs) and fail-closed, then hash-verified locally. Reuses the hardened
SSH option set without redesigning stage1. This driver makes no target contact in
dry-run; tests drive the identical entry/payload/archive path with a local fake.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import shlex
import subprocess
import sys
import tarfile
import uuid
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Final

if __package__ in (None, ""):
    _here = Path(__file__).resolve().parent
    sys.path = [p for p in sys.path if p not in ("", str(_here))]
    sys.path.insert(0, str(_here.parent))
    __package__ = "target_safety"

from .acquire_driver import (DEFAULT_LINK_EXCEPTIONS, DEFAULT_ROOTS, build_tar,
                             load_budget, module_sources)
from .kernel import Blocked
from .acquire_transport_io import TransportReceipt, lifecycle_deadline, run_bounded as _run_bounded

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from .acquire_verify import extract_returned, verify_returned

HERE: Final = Path(__file__).resolve().parent
SSH_HOST: Final = "jetson.local"
SSH_OPTIONS: Final = (
    "-o", "BatchMode=yes", "-o", "StrictHostKeyChecking=yes", "-o", "ConnectTimeout=10",
    "-o", "ConnectionAttempts=1", "-o", "UpdateHostKeys=no", "-o", "ControlMaster=no",
    "-o", "ControlPath=none", "-o", "ForwardAgent=no", "-o", "ClearAllForwardings=yes",
)
EXIT_OK: Final = 0
EXIT_FAILED: Final = 1


@dataclass(frozen=True, slots=True)
class TransportConfig:
    roots: tuple[str, ...]
    link_exceptions: tuple[str, ...]
    window_id: str
    target_identity: str
    remote_scratch: str
    local_out: Path
    observation: bytes | None = None
    fixture: bool = False


def build_payload(observation: bytes | None) -> bytes:
    members = module_sources()
    members["target_safety/acquire-input-policy.json"] = (
        HERE / "acquire-input-policy.json").read_bytes()
    if observation is not None:
        members["observation.json"] = observation
    return build_tar(members)


def remote_command(cfg: TransportConfig) -> str:
    roots = " ".join(f"--root {shlex.quote(r)}" for r in cfg.roots)
    links = " ".join(f"--link-exception {shlex.quote(e)}" for e in cfg.link_exceptions)
    if not cfg.remote_scratch.startswith("/tmp/") or ".." in Path(cfg.remote_scratch).parts:
        raise Blocked("scratch_not_tmp", cfg.remote_scratch)
    script = "\n".join([
        "set -euo pipefail",
        f"SCRATCH={shlex.quote(cfg.remote_scratch)}",
        'SRC="$SCRATCH/src"',
        'mkdir -m 700 "$SCRATCH"',
        'mkdir "$SRC"',
        'tar -x -C "$SRC"',
        'EXTRA=(); if [ -f "$SRC/observation.json" ]; then EXTRA=(--input-set "$SRC/observation.json"); fi',
        "set +e",
        f'PYTHONPATH="$SRC" python3 -B -m target_safety.acquire --scratch "$SCRATCH" '
        f'{roots} {links} --task-window-id {shlex.quote(cfg.window_id)} '
        f'--target-identity {shlex.quote(cfg.target_identity)} "${{EXTRA[@]}}" 1>&2',
        "ACQUIRE_EXIT=$?",
        "set -e",
        'tar -cf "$SCRATCH/return.tar" -C "$SCRATCH" before.json after.json diff.json '
        "receipt.json acquisition.json snapshot",
        'printf "AA_ARCHIVE_SHA256=" >&2; sha256sum "$SCRATCH/return.tar" >&2',
        'cat "$SCRATCH/return.tar"',
        "exit $ACQUIRE_EXIT",
    ])
    seconds = int(load_budget()["remote_deadline_seconds"])
    return "set -euo pipefail\n" + shlex.join((*_timeout_prefix(seconds), "bash", "-c", script))


def _timeout_prefix(seconds: int) -> tuple[str, ...]:
    return ("timeout", "--signal=TERM", "--kill-after=5s", f"{seconds}s")


def transport_argv(cfg: TransportConfig, kind: str, timeout_seconds: int) -> tuple[str, ...]:
    command = remote_command(cfg)
    prefix = _timeout_prefix(timeout_seconds)
    if kind == "ssh":
        return prefix + ("ssh", "-T", *SSH_OPTIONS, SSH_HOST, command)
    return prefix + ("bash", "-c", command)


def run_window(cfg: TransportConfig, kind: str = "ssh") -> dict[str, object]:
    if kind == "ssh" and (cfg.roots != DEFAULT_ROOTS or cfg.link_exceptions != DEFAULT_LINK_EXCEPTIONS
                          or cfg.observation is not None or cfg.fixture
                          or Path(cfg.remote_scratch).parent != Path("/tmp")
                          or cfg.target_identity != SSH_HOST):
        raise Blocked("live_scope_mismatch", "fixed roots/exceptions; live selection only")
    budget = load_budget()
    timeout_seconds = int(budget["transport"]["outer_timeout_seconds"])
    cap = int(budget["transfers"]["return_archive_bytes_bound"])
    cfg.local_out.mkdir(parents=True, exist_ok=False)
    result = {"success": False, "verification": {"equivalent": False}, "error": None}
    try:
        with lifecycle_deadline(int(budget["total_budget_seconds"])):
            payload = build_payload(cfg.observation)
            stdout_sink = cfg.local_out / "return.tar"
            argv = transport_argv(cfg, kind, timeout_seconds)
            result.update(argv=list(argv), payload_sha256=hashlib.sha256(payload).hexdigest(),
                          payload_bytes=len(payload), remote_command=remote_command(cfg))
            receipt = _run_bounded(argv, payload, stdout_sink, cap, timeout_seconds)
            result["receipt"] = asdict(receipt)
            (cfg.local_out / "transport.stderr").write_text(receipt.stderr)
            (cfg.local_out / "transport.exit").write_text(str(receipt.exit_status))
            expected = [line.split("=", 1)[1].split()[0] for line in receipt.stderr.splitlines()
                        if line.startswith("AA_ARCHIVE_SHA256=")]
            if expected != [receipt.stdout_sha256]:
                raise Blocked("archive_digest_mismatch", "remote expected digest")
            returned = cfg.local_out / "returned"
            returned.mkdir()
            result["returned"] = str(returned)
            extract_returned(stdout_sink, returned, cap)
            verification = verify_returned(returned, cfg)
            result["verification"] = verification
            result["success"] = (receipt.exit_status == 0 and not receipt.timed_out
                                  and verification["equivalent"])
    except (Blocked, OSError, ValueError, KeyError, TypeError, tarfile.TarError,
            subprocess.SubprocessError) as error:
        result["error"] = str(error)
        result["timed_out"] = isinstance(error, Blocked) and "timeout" in error.reason
    (cfg.local_out / "transport-result.json").write_text(json.dumps(result, sort_keys=True) + "\n")
    return result


def _config(args: argparse.Namespace) -> TransportConfig:
    observation = Path(args.input_set).read_bytes() if args.input_set else None
    scratch = args.remote_scratch or f"/tmp/aa-acquire-{args.window_id}-{uuid.uuid4().hex}"
    return TransportConfig(tuple(args.root or DEFAULT_ROOTS), tuple(args.link_exception or DEFAULT_LINK_EXCEPTIONS), args.window_id,
                           args.target_identity, scratch, args.local_out, observation)


def parse(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="task-5 finite acquisition transport driver")
    parser.add_argument("--root", action="append")
    parser.add_argument("--link-exception", action="append")
    parser.add_argument("--window-id", default="task-5-acquisition-window")
    parser.add_argument("--target-identity", default="jetson.local")
    parser.add_argument("--remote-scratch", default=None,
                        help="target /tmp scratch; defaults to a unique /tmp/aa-acquire-<window-id>")
    parser.add_argument("--local-out", type=Path, default=Path("/tmp/aa-acquire-local"))
    parser.add_argument("--input-set", default=None, help="observation JSON (tests); omit for live select")
    parser.add_argument("--transport", choices=("ssh", "local"), default="ssh")
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--dry-run", action="store_true", help="print command; no target contact")
    mode.add_argument("--execute", action="store_true", help="perform the single reviewed window")
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse(argv)
    cfg = _config(args)
    if args.dry_run:
        payload = build_payload(cfg.observation)
        print(json.dumps({
            "remote_command": remote_command(cfg),
            "argv": list(transport_argv(cfg, args.transport,
                                        int(load_budget()["transport"]["outer_timeout_seconds"]))),
            "payload_sha256": hashlib.sha256(payload).hexdigest(),
            "payload_bytes": len(payload),
            "total_budget_seconds": load_budget()["total_budget_seconds"],
            "target_contact": "none (dry-run)",
        }, indent=2))
        return EXIT_OK
    result = run_window(cfg, args.transport)
    print(json.dumps(result, indent=2, default=str))
    return EXIT_OK if result["success"] else EXIT_FAILED


if __name__ == "__main__":
    raise SystemExit(main())
