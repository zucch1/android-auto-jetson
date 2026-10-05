# /// script
# requires-python = ">=3.11"
# dependencies = []
# ///
# Run: python3 -B tools/target_safety/stage1.py
"""Qualify locally, send an in-memory helper once, retain exact stage1 receipts."""
from __future__ import annotations

import hashlib
import json
import shlex
import subprocess
import sys
import tempfile
from collections.abc import Callable
from contextlib import ExitStack
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path
from typing import Final, assert_never

# Direct-script bootstrap: run as `python3 -B tools/target_safety/stage1.py`,
# this module has no package, so the relative imports below would fail closed
# with ImportError before any route validation. Enter PACKAGE mode first so the
# identical symbols resolve through the target_safety package. The tools parent
# (not this script's directory) is exposed so the local resource.py cannot
# shadow the stdlib resource module; argv, routes, error classes and remote
# payload bytes are all unchanged.
if __package__ in (None, ""):
    _here = Path(__file__).resolve().parent
    sys.path = [p for p in sys.path if p not in ("", str(_here))]
    sys.path.insert(0, str(_here.parent))
    __package__ = "target_safety"

if __package__:
    from .deadline import (CLEANUP_HEADROOM_SECONDS, COLLECTION_BOUND_SECONDS,
                           SUPERVISOR_BUDGET_SECONDS)
    from .local_capture import LocalCapture
    from .payload import bundle, sizing_bundle, supervisor_bundle
    from .qualification import Receipt, source_checks as _source_checks
    from .report import accepted_ceiling_output, accepted_sizing_output
    from .resource import RESOURCE_POLICY, SIZING_RESOURCE_POLICY
    from .routes import ReceiptDestinationError, resolve as _resolve_route
    from .runner import run_owned
else:
    from deadline import (CLEANUP_HEADROOM_SECONDS, COLLECTION_BOUND_SECONDS,
                          SUPERVISOR_BUDGET_SECONDS)
    from local_capture import LocalCapture
    from payload import bundle, sizing_bundle, supervisor_bundle
    from qualification import Receipt, source_checks as _source_checks
    from report import accepted_ceiling_output, accepted_sizing_output
    from resource import RESOURCE_POLICY, SIZING_RESOURCE_POLICY
    from routes import ReceiptDestinationError, resolve as _resolve_route
    from runner import run_owned

HERE: Final = Path(__file__).resolve().parent
WORKTREE: Final = HERE.parents[1]
EVIDENCE: Final = WORKTREE.parents[1] / "evidence/jetson-android-auto-receiver"
# Fixed bounded envelope from Oracle consultation bg_72e527f3; the remote guard
# window (900s) expires before SSH timeout, and SSH before the local subprocess.
SSH_TIMEOUT_SECONDS: Final = 960
SSH_KILL_GRACE_SECONDS: Final = 5
LOCAL_RUN_TIMEOUT_SECONDS: Final = 980
# Receipt-only mirror of the enforced product caps (coverage.PROTECTED_ENTRY_LIMIT,
# kernel.MAX_WATCHES/WINDOW_SECONDS, inventory.INVENTORY_BYTE_LIMIT) and of the
# authoritative deadline envelope allocation (deadline.py). Envelope tests
# cross-check both sides so drift cannot pass qualification.
EFFECTIVE_CAPS: Final = {
    "protected_entries": 1000000,
    "registered_watches": 1000128,
    "single_monotonic_window_seconds": 900,
    "bytes_per_protected_inventory": 8589934592,
    "ssh_timeout_seconds": SSH_TIMEOUT_SECONDS,
    "ssh_kill_grace_seconds": SSH_KILL_GRACE_SECONDS,
    "local_subprocess_timeout_seconds": LOCAL_RUN_TIMEOUT_SECONDS,
    "startup_bound_seconds": 20.0,
    "supervisor_budget_seconds": 940.0,
    "cleanup_headroom_seconds": 40.0,
    "termination_reap_bound_seconds": 30.0,
    "egress_bound_seconds": 5.0,
    "scheduling_slack_seconds": 5.0,
    "diagnostic_allowance_seconds": 1.0,
}
RESOURCE_CLAIM_LIMITATIONS: Final = (
    "8 GiB caps read bytes per protected inventory, not RSS. Two inventories "
    "allow 16 GiB of hashing reads plus probes. The 1000000/1000128/1048576 "
    "policy is an operational ceiling only; registry/row/JSON memory, RAM fit "
    "and target fit remain unmeasured. Resource readings are abort triggers, "
    "not reservations, and restoration is never guaranteed. Window and "
    "transport timeouts cannot prove remote exit or descriptor cleanup without "
    "an explicit receipt."
)
SSH: Final = (
    "timeout", "--signal=TERM", f"--kill-after={SSH_KILL_GRACE_SECONDS}s",
    f"{SSH_TIMEOUT_SECONDS}s", "ssh", "-T",
    "-o", "BatchMode=yes", "-o", "StrictHostKeyChecking=yes", "-o", "ConnectTimeout=10",
    "-o", "ConnectionAttempts=1", "-o", "UpdateHostKeys=no", "-o", "ControlMaster=no",
    "-o", "ControlPath=none", "-o", "ForwardAgent=no", "-o", "ClearAllForwardings=yes",
    "jetson.local", "bash -c 'set -euo pipefail; exec python3 -B -'",
)


def run(argv: tuple[str, ...], payload: bytes | None = None) -> Receipt:
    """Capture a bounded process exactly, including timeout failure streams."""
    try:
        result = subprocess.run(argv, input=payload, cwd=WORKTREE, capture_output=True,
                                check=False, timeout=LOCAL_RUN_TIMEOUT_SECONDS)
    except subprocess.TimeoutExpired as error:
        return Receipt(argv=list(argv), command=shlex.join(argv), cwd=str(WORKTREE),
                       exit_status=124, stdout=(error.stdout or b"").decode(errors="replace"),
                       stderr=(error.stderr or b"").decode(errors="replace") + "\nlocal total timeout")
    return Receipt(argv=list(argv), command=shlex.join(argv), cwd=str(WORKTREE),
                   exit_status=result.returncode, stdout=result.stdout.decode(errors="replace"),
                   stderr=result.stderr.decode(errors="replace"))


def attempt(stack: ExitStack, destination: Path) -> int:
    """The root receipt is stage1-only and cannot complete task5."""
    capture = Path(tempfile.mkdtemp(prefix="task5-expanded-", dir="/tmp/opencode"))
    qualification = run((sys.executable, "-B", "tests/target_safety/scenarios.py"))
    (capture / "qualification.json").write_text(json.dumps(qualification, indent=2))
    print(qualification["stdout"], end="")
    if qualification["exit_status"]:
        print(qualification["stderr"], file=sys.stderr)
        return 1
    payload = bundle()
    (capture / "remote-stdin.txt").write_bytes(payload)
    # Drive the exact in-memory module loader locally, without target observations.
    smoke = payload.rsplit(b"raise SystemExit", 1)[0] + (
        b"from target_safety.kernel import Watch\n"
        b"with Watch.session() as w: w.check()\nprint('stdin-loader-pass')\n"
    )
    loader = run((sys.executable, "-B", "-"), smoke)
    (capture / "loader.json").write_text(json.dumps(loader, indent=2))
    if loader["exit_status"]:
        print(loader["stderr"], file=sys.stderr)
        return 1
    sources = _source_checks()
    # Reserve exclusively after all local qualification, before any SSH action.
    stream = stack.enter_context(destination.open("x"))
    remote = run(SSH, payload)
    (capture / "remote.json").write_text(json.dumps(remote, indent=2))
    events = [json.loads(line) for line in remote["stdout"].splitlines() if line.startswith("{")]
    safety_pass = (remote["exit_status"] == 0 and
                   any(event["event"] == "protected_no_detected_write_no_persistent_change" for event in events))
    receipt = {
        "schema": "aa-task5-stage1-expanded-1", "timestamp_utc": datetime.now(timezone.utc).isoformat(),
        "task": 5, "stage": 1, "task5_completed": False,
        "outcome": "guarded-prerequisite-observations-pass" if safety_pass else "blocker",
        "scope_approval": str(EVIDENCE / "task-5-scope-approval.json"),
        "namespace_approval": str(EVIDENCE / "task-5-namespace-approval.json"),
        "venv_namespace_approval": str(EVIDENCE / "task-5-venv-namespace-approval.json"),
        "namespace_exclusion": "Only the two exact approved python3 link entries (/home/zucchi/Desktop/infotainment-plan/.local_env/aqt-venv/bin/python3 and /home/zucchi/Desktop/infotainment-plan/.venv/bin/python3) and replacement-sensitive parents protected; /usr/bin/python3 referent is neither followed nor watched through these links. Internal aliases may terminate here only. .codegraph retains full content coverage.",
        "capture_directory": str(capture), "qualification": qualification, "stdin_loader": loader,
        "remote": remote, "remote_stdin_sha256": hashlib.sha256(payload).hexdigest(),
        "remote_stdin_bytes": len(payload), "source_checks": sources,
        "effective_caps": dict(EFFECTIVE_CAPS),
        "resource_claim_limitations": RESOURCE_CLAIM_LIMITATIONS,
        "diagnostics": "AST syntax checks pass; basedpyright LSP unavailable (previously declined); no type/lint pass claimed",
        "supported_safety_claim": "no detected write and no persistent protected-path change" if safety_pass else None,
        "blockers": ([event["data"] for event in events if event["event"] == "guard_blocked"]
                     or ([] if safety_pass else [f"remote exit {remote['exit_status']}; no final safety claim"])),
        "synthetic_tests": "overflow, ignored-watch and unmount parser injection; not real overflow/unmount qualification",
        "original_artifacts": "/tmp/opencode/task5-prerequisites-jcob4wX2 (preserved)",
        "redaction": "No credentials or protected file contents emitted; metadata/hashes and dirty Git paths are private evidence",
        "stage2_pending": "sysroot extraction, bound manifest, cross-build/preset and immutable private cache qualification",
    }
    json.dump(receipt, stream, indent=2)
    print(json.dumps({"outcome": receipt["outcome"], "remote_exit": remote["exit_status"],
                      "capture": str(capture), "blockers": receipt["blockers"]}))
    return 0


def attempt_ceiling(stack: ExitStack, destination: Path) -> int:
    """Local ceiling-supervisor TDD only: no SSH, no remote action, no kernel mutation."""
    capture = Path(tempfile.mkdtemp(prefix="task5-ceiling-", dir="/tmp/opencode"))
    suites = {
        "qualification": run((sys.executable, "-B", "tests/target_safety/ceiling_qa.py")),
    }
    for name, result in suites.items():
        (capture / f"{name}.json").write_text(json.dumps(result, indent=2))
        print(result["stdout"], end="")
        if result["exit_status"]:
            print(result["stderr"], file=sys.stderr)
    ok = all(result["exit_status"] == 0 for result in suites.values())
    sources = _source_checks()
    stream = stack.enter_context(destination.open("x"))
    receipt = {
        "schema": "aa-task5-prerequisites-ceiling-1",
        "timestamp_utc": datetime.now(timezone.utc).isoformat(),
        "task": 5, "stage": "ceiling-supervisor-local-qualification", "task5_completed": False,
        "outcome": "ceiling-supervisor-local-qualified" if ok else "blocker",
        "scope": "local supervisor implementation + TDD only; no SSH, no remote action, no kernel mutation",
        "ceiling": {"setting": "fs.inotify.max_user_watches", "original": 65536, "temporary": 1048576},
        "capture_directory": str(capture), "suites": suites, "source_checks": sources,
        "effective_caps": dict(EFFECTIVE_CAPS),
        "supervisor_envelope": {"budget_seconds": SUPERVISOR_BUDGET_SECONDS,
                                "cleanup_headroom_seconds": CLEANUP_HEADROOM_SECONDS,
                                "child_deadline_seconds": COLLECTION_BOUND_SECONDS},
        "limitations": (
            "No atomic sysctl CAS: restore is best-effort readback reconciliation and never "
            "overwrites an observed conflict; supervisor SIGKILL/power loss is best-effort only, "
            "never a guarantee. No arbitrary-concurrent-writer guarantee; observed conflicts block. "
            "Fault cases mix synthetic (setting boundary) and actual process-lifecycle (guard/transport)."
        ),
        "redaction": "No credentials or protected file contents emitted; module paths, hashes and fixture paths only",
    }
    json.dump(receipt, stream, indent=2)
    print(json.dumps({"outcome": receipt["outcome"], "capture": str(capture)}))
    return 0 if ok else 1


def _guarded_attempt(stack: ExitStack, destination: Path, spec: "AttemptSpec") -> int:
    """Shared exclusive-reserve + strict transport + typed acceptance gate."""
    print('local qualification_start', file=sys.stderr, flush=True)
    qualification = run((sys.executable, "-B", "tests/target_safety/ceiling_qa.py"))
    print('local qualification_done', qualification['exit_status'], file=sys.stderr, flush=True)
    if qualification["exit_status"]:
        print(qualification["stderr"], file=sys.stderr)
        return 1
    sources = _source_checks()
    stream = stack.enter_context(destination.open("x"))
    capture = LocalCapture.reserve(stack, destination)
    remote = run_owned(SSH, capture=capture, timeout_seconds=LOCAL_RUN_TIMEOUT_SECONDS,
                       stdin=spec.payload, cwd=WORKTREE)
    print('local transport_returned', remote.exit_status, file=sys.stderr, flush=True)
    # Incomplete pump/journal evidence (failed capture, unfinished pump) is
    # rejected even when the transport exit looks clean.
    clean = remote.exit_status == 0 and not remote.timed_out and remote.evidence_complete
    outcome = spec.accept(remote.stdout) if clean else False
    receipt = {
        "schema": spec.schema,
        "timestamp_utc": datetime.now(timezone.utc).isoformat(),
        "task": 5, "stage": spec.stage, "task5_completed": False,
        "qualification_exit": qualification["exit_status"], "accepted": outcome,
        "local_capture": capture.paths, "remote_stdin_sha256": hashlib.sha256(spec.payload).hexdigest(),
        "remote": {"argv": list(remote.argv), "exit_status": remote.exit_status,
                   "stdout": remote.stdout, "stderr": remote.stderr, "timed_out": remote.timed_out,
                   "evidence_complete": remote.evidence_complete},
        "source_checks": sources, "effective_caps": dict(EFFECTIVE_CAPS),
        "resource_policy": dict(spec.policy),
    }
    json.dump(receipt, stream, indent=2)
    return 0 if outcome else 1


@dataclass(frozen=True, slots=True)
class AttemptSpec:
    """Fixed identity of one guarded remote attempt route."""

    schema: str
    stage: str
    payload: bytes
    accept: Callable[[str], bool]
    policy: dict[str, str | int]


def attempt_target(stack: ExitStack, destination: Path) -> int:
    """Single approved guarded remote attempt: qualify, reserve, then strict SSH."""
    return _guarded_attempt(stack, destination, AttemptSpec(
        schema="aa-task5-prerequisites-ceiling-attempt-1",
        stage="ceiling-guarded-remote-attempt",
        payload=supervisor_bundle(), accept=accepted_ceiling_output,
        policy=RESOURCE_POLICY))


def attempt_sizing(stack: ExitStack, destination: Path) -> int:
    """Single approved guarded topology-only sizing attempt; never prerequisite acceptance."""
    return _guarded_attempt(stack, destination, AttemptSpec(
        schema="aa-task5-topology-sizing-attempt-1",
        stage="topology-sizing-guarded-remote-attempt",
        payload=sizing_bundle(), accept=accepted_sizing_output,
        policy=SIZING_RESOURCE_POLICY))


def main() -> int:
    """Accept only allowlisted receipt destinations; preserve historical files."""
    with ExitStack() as stack:
        destination, kind = _resolve_route(EVIDENCE, sys.argv[1:])
        match kind:
            case "target":
                return attempt_target(stack, destination)
            case "ceiling":
                return attempt_ceiling(stack, destination)
            case "sizing":
                return attempt_sizing(stack, destination)
            case "expanded":
                return attempt(stack, destination)
            case unreachable:
                assert_never(unreachable)


if __name__ == "__main__":
    raise SystemExit(main())
