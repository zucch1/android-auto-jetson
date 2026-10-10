# SPDX-License-Identifier: GPL-3.0-or-later
"""Bounded multiplexed child stdin/stdout/stderr collection with hard caps.

One pump replaces unbounded communicate() accumulation. Combined child output
is capped and a single record (line) is capped; crossing either cap, or a
resource abort trigger while collecting, invalidates the run and stops the
guard. Retained bytes are hashed incrementally so compact receipts never carry
output-sized copies. Capped or unfinished data is never accepted. The selector
wait also wakes for the periodic heartbeat: at the earliest of readiness, the
next heartbeat or the collection deadline. Heartbeats report worker progress
counters and the last-progress age; they never extend any deadline and never
mask worker inactivity.
"""
from __future__ import annotations

import hashlib
import os
import subprocess
import time
from collections.abc import Callable
from dataclasses import dataclass
from typing import Final, assert_never

from .collector_io import ChildStreams, CollectionIO
from .resource import ResourceError

COMBINED_OUTPUT_LIMIT_BYTES: Final = 16777216
RECORD_LIMIT_BYTES: Final = 65536
MAX_VIOLATION_DETAILS: Final = 8
DRAIN_GRACE_SECONDS: Final = 5.0
CHUNK_BYTES: Final = 65536
SAMPLE_INTERVAL_SECONDS: Final = 0.1
HEARTBEAT_INTERVAL_SECONDS: Final = 30.0


@dataclass(frozen=True, slots=True)
class WorkerProgress:
    """Worker output progress snapshot for one heartbeat record."""

    records: int
    combined_bytes: int
    stdin_sent: int
    last_progress_age_seconds: float


@dataclass(frozen=True, slots=True)
class CollectionPolicy:
    """Bounded collection envelope; fixed sizing-phase values, never auto-grows."""

    combined_limit: int = COMBINED_OUTPUT_LIMIT_BYTES
    record_limit: int = RECORD_LIMIT_BYTES
    sample_interval: float = SAMPLE_INTERVAL_SECONDS
    drain_grace: float = DRAIN_GRACE_SECONDS
    heartbeat_interval: float = HEARTBEAT_INTERVAL_SECONDS


POLICY: Final = CollectionPolicy()


@dataclass(frozen=True, slots=True)
class CollectionResult:
    """Retained outcome of one bounded collection; capped data is never accepted."""

    stdout: bytes
    stderr: bytes
    stdout_sha256: str
    stderr_sha256: str
    stdin_sent: int
    stdin_complete: bool
    combined_bytes: int
    records: int
    timed_out: bool
    truncated: bool
    invalidated: bool
    first_cause: str | None
    violation_details: tuple[str, ...]
    samples: int


class Collector:
    """One bounded multiplexed pump for a child's stdin/stdout/stderr."""

    def __init__(self, proc: subprocess.Popen[bytes], stdin: bytes | None, deadline: float,
                 *, policy: CollectionPolicy = POLICY,
                 sample: Callable[[], str | None] | None = None,
                 heartbeat: Callable[[WorkerProgress], None] | None = None) -> None:
        self.proc = proc
        self.stdin = stdin
        self.deadline = deadline
        self.policy = policy
        self.sample = sample
        self.heartbeat = heartbeat
        self._out = bytearray()
        self._err = bytearray()
        self._out_hash = hashlib.sha256()
        self._err_hash = hashlib.sha256()
        self._sent = 0
        self._records = 0
        self._line = {"out": 0, "err": 0}
        self._causes: list[str] = []
        self._truncated = False
        self._samples = 0
        self._stopped = False
        self._timed_out = False

    def _violation(self, cause: str) -> None:
        """Record causes briefly; retained causes stay capped at the detail cap."""
        if len(self._causes) < MAX_VIOLATION_DETAILS:
            self._causes.append(cause)

    def _append(self, stream: str, data: bytes) -> bool:
        room = self.policy.combined_limit - (len(self._out) + len(self._err))
        if room <= 0:
            self._truncated = True
            self._violation(f"combined_output_limit {self.policy.combined_limit}")
            return False
        if len(data) > room:
            data = data[:room]
            self._truncated = True
        target = self._out if stream == "out" else self._err
        digest = self._out_hash if stream == "out" else self._err_hash
        target += data
        digest.update(data)
        parts = data.split(b"\n")
        for part in parts[:-1]:
            self._line[stream] += len(part)
            if self._line[stream] > self.policy.record_limit:
                self._violation(f"oversized_record {self._line[stream]} > {self.policy.record_limit}")
                return False
            self._records += 1
            self._line[stream] = 0
        self._line[stream] += len(parts[-1])
        if self._line[stream] > self.policy.record_limit:
            self._violation(f"oversized_record {self._line[stream]} > {self.policy.record_limit}")
            return False
        if self._truncated:
            self._violation(f"combined_output_limit {self.policy.combined_limit}")
            return False
        return True

    def run(self) -> CollectionResult:
        """Drive stdin and read both streams until EOF, cap, violation or deadline.

        The selector and the original child pipes are owned independently of
        registration and are closed on every path, including typed sample/read/
        write failures and unexpected errors; an expected boundary failure is
        recorded as an invalidated result and never propagates or fakes success.
        Every selector wait is bounded by the remainder to the next due resource
        sample, the collection deadline and the drain budget — never a fresh
        full interval. Caps, deadlines and resource aborts stop the pump and the
        guard; stream/stdin IO faults invalidate without faking EOF or success
        and let the child finish boundedly on its own.
        """
        proc = self.proc
        cio = CollectionIO(ChildStreams(proc.stdout, proc.stderr, proc.stdin), self._violation)
        remaining = self.stdin or b""
        drain_until: float | None = None
        last_sample = 0.0
        started = time.monotonic()
        last_progress = started
        next_heartbeat = started + self.policy.heartbeat_interval if self.heartbeat is not None else None
        try:
            if cio.start(want_stdin=bool(remaining)):
                while cio.registered and not self._stopped:
                    now = time.monotonic()
                    if now >= self.deadline:
                        self._violation("collection_deadline")
                        self._timed_out = True
                        self._stopped = True
                        break
                    if self.sample is not None and (not self._samples or now - last_sample >= self.policy.sample_interval):
                        last_sample = now
                        self._samples += 1
                        try:
                            reason = self.sample()
                        except (ResourceError, OSError) as error:
                            self._violation(f"resource_sample_fault {error}")
                            self._stopped = True
                            break
                        if reason is not None:
                            self._violation(f"resource_abort {reason}")
                            self._stopped = True
                            break
                    # One heartbeat per interval; the sink caps its own write at
                    # one sample interval and at the collection deadline, so this
                    # 0.1 s sampling cadence is delayed by at most one interval.
                    if next_heartbeat is not None and now >= next_heartbeat:
                        self.heartbeat(WorkerProgress(
                            records=self._records, combined_bytes=len(self._out) + len(self._err),
                            stdin_sent=self._sent,
                            last_progress_age_seconds=max(0.0, now - last_progress)))
                        next_heartbeat = now + self.policy.heartbeat_interval
                        # The synchronous callback may have overrun the
                        # collection deadline: recheck it immediately, before any
                        # readiness event is processed (loop-top discipline).
                        if time.monotonic() >= self.deadline:
                            self._violation("collection_deadline")
                            self._timed_out = True
                            self._stopped = True
                            break
                    select_start = time.monotonic()
                    wait = self.policy.sample_interval
                    if self.sample is not None:
                        wait = min(wait, last_sample + self.policy.sample_interval - select_start)
                    wait = min(wait, self.deadline - select_start)
                    if next_heartbeat is not None:
                        wait = min(wait, next_heartbeat - select_start)
                    if drain_until is not None:
                        wait = min(wait, drain_until - select_start)
                    batch = cio.select_ready(max(0.0, wait))
                    if batch.faulted:
                        self._stopped = True
                        break
                    for event in batch.events:
                        match event.role:
                            case "in":
                                if remaining:
                                    try:
                                        written = os.write(event.fd, remaining[:CHUNK_BYTES])
                                    except (BlockingIOError, InterruptedError):
                                        continue
                                    except OSError as error:
                                        self._violation(f"stdin_write_fault {error}")
                                        remaining = b""
                                    else:
                                        self._sent += written
                                        remaining = remaining[written:]
                                if not remaining:
                                    cio.close_role("in")
                                    cio.unregister(event)
                            case "out" | "err":
                                try:
                                    chunk = os.read(event.fd, CHUNK_BYTES)
                                except (BlockingIOError, InterruptedError):
                                    continue
                                except OSError as error:
                                    self._violation(f"stream_read_fault {error}")
                                    cio.unregister(event)
                                    continue
                                if not chunk:
                                    cio.unregister(event)
                                    continue
                                last_progress = time.monotonic()
                                if not self._append(event.role, chunk):
                                    self._stopped = True
                                    break
                            case unreachable:
                                assert_never(unreachable)
                    if proc.poll() is not None:
                        if not cio.registered:
                            break
                        drain_until = now + self.policy.drain_grace if drain_until is None else drain_until
                        if time.monotonic() >= drain_until:
                            self._violation("drain_incomplete")
                            self._stopped = True
                            break
        finally:
            unexpected_close = cio.close_all()
        if unexpected_close is not None:
            raise unexpected_close
        return CollectionResult(
            stdout=bytes(self._out), stderr=bytes(self._err),
            stdout_sha256=self._out_hash.hexdigest(), stderr_sha256=self._err_hash.hexdigest(),
            stdin_sent=self._sent, stdin_complete=self._sent == len(self.stdin or b""),
            combined_bytes=len(self._out) + len(self._err), records=self._records,
            timed_out=self._timed_out, truncated=self._truncated,
            invalidated=bool(self._causes) or self._truncated,
            first_cause=self._causes[0] if self._causes else None,
            violation_details=tuple(self._causes),
            samples=self._samples,
        )
