"""Guard child process-group lifecycle: spawn, monotonic deadline, TERM/KILL/reap.

The guard runs in its own session/process group so the supervisor can terminate
the whole tree. The child-only RLIMIT_AS ceiling is installed by a bounded
pre-exec hook (one setrlimit, no I/O) so no child ever sets up watches without
it; the supervisor itself is never limited. A single monotonic deadline bounds
the child independent of the guard's own Watch.check(). Cleanup sends TERM then
an unconditional group KILL and reaps the direct child. Output collection goes
through the bounded multiplexed collector: a descendant that leaves the group
and holds the pipes cannot hang the supervisor, and capped/unfinished data is
flagged so it is never accepted. We make NO whole-tree guarantee beyond what the
process group owns (orphaned grandchildren are reaped by init); best-effort only
for SIGKILL/power loss.
"""
from __future__ import annotations

import contextlib
import os
import signal
import subprocess
import time
from collections.abc import Callable
from dataclasses import dataclass
from typing import Final

from .collector import CollectionPolicy, Collector, WorkerProgress
from .resource import install_child_address_space_limit

GUARD_TERM_GRACE_SECONDS: Final = 5.0
GUARD_KILL_GRACE_SECONDS: Final = 5.0


class GuardStateError(RuntimeError):
    """Typed guard-lifecycle misuse (wait/terminate before spawn)."""

    def __init__(self, state: str) -> None:
        self.state = state
        super().__init__(state)


@dataclass(frozen=True, slots=True)
class GuardResult:
    """Outcome of one guard child run, including timeout/kill disposition."""

    exit_status: int
    stdout: str
    stderr: str
    timed_out: bool
    killed: bool
    combined_bytes: int = 0
    records: int = 0
    truncated: bool = False
    invalidated: bool = False
    first_cause: str | None = None
    stdout_sha256: str = ""
    stderr_sha256: str = ""
    samples: int = 0


def _child_limit() -> None:
    """Bounded pre-exec hook: one setrlimit, no I/O and no allocation."""
    install_child_address_space_limit()


class GuardChild:
    """Owns one guard child in its own process group under a monotonic deadline."""

    def __init__(self, argv: tuple[str, ...], deadline_seconds: float,
                 *, cleanup_deadline: float | None = None) -> None:
        self.argv = argv
        self.deadline = time.monotonic() + deadline_seconds
        self.cleanup_deadline = (self.deadline + 15.0 if cleanup_deadline is None else cleanup_deadline)
        self.proc: subprocess.Popen[bytes] | None = None
        self.terminated = False
        self.sample: Callable[[], str | None] | None = None
        self.heartbeat: Callable[[WorkerProgress], None] | None = None
        self.policy: CollectionPolicy = CollectionPolicy()
        self._stdin: bytes | None = None

    def spawn(self, stdin: bytes | None = None) -> None:
        """Start the guard in a new session so its whole tree is one killable group.

        The payload is NOT written here: a manual stdin.write+close deadlocks the
        later collection flush and is an unbounded pre-wait write that bypasses
        the deadline. The payload is driven by the bounded collector in wait().
        """
        self._stdin = stdin
        try:
            self.proc = subprocess.Popen(
                self.argv,
                stdin=subprocess.PIPE if stdin is not None else subprocess.DEVNULL,
                stdout=subprocess.PIPE, stderr=subprocess.PIPE, start_new_session=True,
                preexec_fn=_child_limit,
            )
        except (OSError, subprocess.SubprocessError) as error:
            raise GuardStateError(f"spawn: {error}") from error

    def wait(self) -> GuardResult:
        """Collect stdin/stdout/stderr boundedly; on stop conditions, terminate.

        Bracket semantics (requal observation 2): the window-end TERM/KILL/reap
        runs inside this call, before the caller closes its collection bracket,
        because output collection is not complete until the child is reaped.
        The caller's cleanup_* transitions therefore bracket supervisor cleanup
        only: idempotent terminate escalation + restoration + reconcile.
        """
        proc = self.proc
        if proc is None:
            raise GuardStateError("wait before spawn")
        collected = Collector(proc, self._stdin, self.deadline,
                              policy=self.policy, sample=self.sample,
                              heartbeat=self.heartbeat).run()
        if collected.timed_out or collected.invalidated:
            self.terminate()
        elif proc.poll() is None:
            try:
                proc.wait(timeout=min(GUARD_TERM_GRACE_SECONDS,
                                      max(0.0, self.cleanup_deadline - time.monotonic())))
            except subprocess.TimeoutExpired:
                self.terminate()
        return GuardResult(
            exit_status=proc.returncode if proc.returncode is not None else -1,
            stdout=collected.stdout.decode(errors="replace"),
            stderr=collected.stderr.decode(errors="replace"),
            timed_out=collected.timed_out,
            killed=self.terminated,
            combined_bytes=collected.combined_bytes,
            records=collected.records,
            truncated=collected.truncated,
            invalidated=collected.invalidated,
            first_cause=collected.first_cause,
            stdout_sha256=collected.stdout_sha256,
            stderr_sha256=collected.stderr_sha256,
            samples=collected.samples,
        )

    def terminate(self) -> None:
        """Idempotent: TERM the group, then always KILL the group and reap.

        The unconditional KILL reaches TERM-ignoring grandchildren in the group
        even after the direct child has died and been reaped. Processes outside
        the group are out of scope (no whole-tree guarantee).
        """
        proc = self.proc
        if proc is None or self.terminated:
            return
        try:
            stop_deadline = min(self.cleanup_deadline, time.monotonic() + 10.0)
            self._signal_group(proc.pid, signal.SIGTERM)
            _reap(proc, min(GUARD_TERM_GRACE_SECONDS, max(0.0, stop_deadline - time.monotonic())))
            self._signal_group(proc.pid, signal.SIGKILL)
            if not _reap(proc, min(GUARD_KILL_GRACE_SECONDS, max(0.0, stop_deadline - time.monotonic()))):
                raise GuardStateError("termination reap timeout; stop unconfirmed")
            self.terminated = True
        except OSError as error:
            raise GuardStateError(f"termination: {error}") from error

    @staticmethod
    def _signal_group(pgid: int, sig: signal.Signals) -> None:
        with contextlib.suppress(ProcessLookupError):
            os.killpg(pgid, sig)


def _reap(proc: subprocess.Popen[bytes], grace: float) -> bool:
    try:
        proc.wait(timeout=grace)
        return True
    except subprocess.TimeoutExpired:
        return False
