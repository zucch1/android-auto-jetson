# SPDX-License-Identifier: GPL-3.0-or-later
"""Every remote diagnostic write through one bounded nonblocking fd discipline.

deliver() is the proven deadline-bound writer: nonblocking fd writes with an
absolute deadline, partial writes resumed inside that deadline and a boolean
outcome. Writer extends that same mechanism into the single serialized emitter
for every remote record - entry, metadata, stage brackets, heartbeats, cleanup
transitions, egress and the terminal result - as small independently framed
JSONL lines. Diagnostic write failure policy is explicit: any failed write
(EPIPE, backpressure timeout, or a record left partially written) marks the
channel broken and stops every further diagnostic attempt; the receiver keeps
the truncated bytes as evidence and marks the stream incomplete. Sizing work
never aborts on a broken channel and no BrokenPipeError is ever uncaught: the
guard finishes its window and cleanup exactly as before and an undeliverable
terminal record fails closed as rejected evidence. The minimal entry record is
the earliest executable bounded write; its receipt cannot prove execution
absence when zero bytes arrive, only that no entry record was received.
"""
from __future__ import annotations

import io
import json
import os
import select
import sys
import threading
import time
from typing import Final

CHUNK_BYTES: Final = 4096
# One record's write bound (backpressure timeout) and the entry record's short
# absolute deadline (hardening design review correction 1).
WRITE_ATTEMPT_BOUND_SECONDS: Final = 5.0
ENTRY_WRITE_BOUND_SECONDS: Final = 5.0
# Provisional stream deadline for the pre-main records (entry, metadata, module
# load bracket); the owning run rebinds the writer to its authoritative
# envelope deadline before any budget-bound work.
PREAMBLE_STREAM_BOUND_SECONDS: Final = 60.0
ENTRY_EVENT: Final = "remote_entry"
META_EVENT: Final = "remote_entry_meta"
STAGE_STARTED_EVENT: Final = "stage_started"
STAGE_COMPLETED_EVENT: Final = "stage_completed"
HEARTBEAT_EVENT: Final = "heartbeat"
# The expected successful-stage bracket sets of accepted hardened streams: the
# supervisor stages its own window phases, the sizing child stages its own
# (module_load from the bundle preamble). A hardened stream missing any of
# these pairs is torn below the stage granularity and is never accepted.
SUPERVISOR_STAGES: Final = ("module_load", "setting_read", "memory_read", "admission",
                            "guard_init", "collection")
CHILD_STAGES: Final = ("module_load", "guard_init", "watch_session")
# The lifecycle transition sequence of a hardened supervisor stream; every
# stage bracket must close before the first of these and they appear exactly
# once, in this order, before the terminal result.
TERMINAL_TRANSITIONS: Final = ("cleanup_started", "cleanup_completed", "egress_started")


def lf_lines(raw: str) -> list[str]:
    """Frame one raw transcript on LITERAL LF boundaries only.

    A CR never delimits a record: splitlines() would treat a CR-separated blob
    as framed records, so every hardened-stream parser frames with this and
    rejects what does not parse as one JSON record per LF line. The trailing
    empty produced by a final LF is not a record; an LF-unterminated tail is
    still one (last) line and is rejected by the end-of-stream LF check.
    """
    lines = raw.split("\n")
    if lines and lines[-1] == "":
        lines.pop()
    return lines


def deliver(lines: list[str], deadline: float) -> bool:
    """No buffered stdout writes or interpreter flush; StringIO is a local test seam."""
    payload = ("\n".join(lines) + "\n").encode()
    if time.monotonic() >= deadline:
        return False
    if isinstance(sys.stdout, io.StringIO):
        sys.stdout.write(payload.decode())
        return time.monotonic() < deadline
    try:
        fd = sys.stdout.fileno()
        blocking = os.get_blocking(fd)
        os.set_blocking(fd, False)
    except (OSError, ValueError, AttributeError):
        return False
    try:
        offset = 0
        while offset < len(payload):
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                return False
            if not select.select([], [fd], [], remaining)[1]:
                return False
            try:
                offset += os.write(fd, payload[offset:offset + CHUNK_BYTES])
            except BlockingIOError:
                continue
        return time.monotonic() < deadline
    except (OSError, ValueError):
        return False
    finally:
        os.set_blocking(fd, blocking)


def _meta_data() -> dict[str, str | None]:
    """Boot identity and interpreter metadata for the second record; never raises."""
    try:
        with open("/proc/sys/kernel/random/boot_id", encoding="ascii") as handle:
            boot_id = handle.read().strip()
    except OSError:
        boot_id = None
    version = ".".join(str(part) for part in sys.version_info[:3])
    return {"boot_id": boot_id, "python_version": version, "executable": sys.executable}


def stage_phase(data: object) -> str | None:
    """The phase identifier of a stage record; None marks a wrong shape."""
    if not isinstance(data, dict) or set(data) != {"phase"}:
        return None
    phase = data.get("phase")
    return phase if isinstance(phase, str) and phase else None


def entry_record_ok(record: dict) -> bool:
    """The minimal entry shape is exactly {event, pid, monotonic_ns} with both ints."""
    if set(record) != {"event", "pid", "monotonic_ns"}:
        return False
    pid, timestamp = record.get("pid"), record.get("monotonic_ns")
    return (type(pid) is int and pid >= 0 and type(timestamp) is int and timestamp >= 0)


def hardened_records(records: list[dict]) -> bool:
    """True once any hardened-stream record appears; legacy streams carry none."""
    return any(record.get("event") in (ENTRY_EVENT, META_EVENT, STAGE_STARTED_EVENT,
                                       STAGE_COMPLETED_EVENT, HEARTBEAT_EVENT,
                                       *TERMINAL_TRANSITIONS) or "seq" in record
               for record in records)


def hardened_stream_ok(records: list[dict], raw: str, expected_stages: tuple[str, ...]) -> bool:
    """Full hardened-stream discipline; legacy outcome-only streams pass unchanged.

    Records frame on LITERAL LF boundaries (lf_lines; a CR never delimits a
    record) and the raw transcript must end in LF (a last record left
    unterminated is rejected). Then the record discipline of
    diagnostic_stream_ok applies. One failure family for the receiver's strict
    child parser: framing, metadata placement, sequence, bracket order and
    stage set.
    """
    return (not hardened_records(records)
            or (raw.endswith("\n") and diagnostic_stream_ok(records, expected_stages)))


def diagnostic_stream_ok(records: list[dict], expected_stages: tuple[str, ...]) -> bool:
    """True for a complete hardened stream; diagnostics present but torn means False.

    Streams carrying no diagnostic record at all (legacy outcome-only
    transcripts) pass unchanged; once any entry, sequence or stage record
    appears the full discipline is required: exactly one minimal entry record
    first (never again later), the metadata record second (never later),
    contiguous sequence numbers 1..N on every other record, and the stage
    brackets in strict phase order: exactly started/completed for each expected
    phase, in the expected sequence, no overlap or interleaving, with every
    bracket closed before the first lifecycle transition (cleanup_started,
    cleanup_completed, egress_started). A reversed pair set, a bracket after a
    cleanup transition or a second entry record is never accepted.
    """
    if not hardened_records(records):
        return True
    if not records or records[0].get("event") != ENTRY_EVENT or not entry_record_ok(records[0]):
        return False
    metas = [index for index, record in enumerate(records)
             if record.get("event") == META_EVENT]
    if len(metas) != 1 or metas[0] != 1:
        return False
    if any(record.get("event") == ENTRY_EVENT for record in records[1:]):
        return False
    expected = 1
    remaining = iter(expected_stages)
    want: str | None = next(remaining)
    opened = False
    after_transitions = False
    for record in records[1:]:
        if type(record.get("seq")) is not int or record["seq"] != expected:
            return False
        expected += 1
        name = record.get("event")
        if name in (STAGE_STARTED_EVENT, STAGE_COMPLETED_EVENT):
            phase = stage_phase(record.get("data"))
            if after_transitions or phase is None or phase != want:
                return False
            if name == STAGE_STARTED_EVENT and not opened:
                opened = True
            elif name == STAGE_COMPLETED_EVENT and opened:
                opened = False
                want = next(remaining, None)
            else:
                return False
        elif name in TERMINAL_TRANSITIONS:
            after_transitions = True
    return not opened and want is None


class Writer:
    """Single serialized emitter of framed JSONL records over the bounded write.

    One instance owns the process channel: sequence numbers are monotonic and
    contiguous, every record is written whole or the channel is marked broken,
    and a broken channel makes every later emit a no-op returning False. The
    lock serializes emitters; signal handlers must never emit (a handler would
    deadlock against the interrupted emit's held lock).
    """

    def __init__(self, deadline: float) -> None:
        self.deadline = deadline
        self.broken = False
        self.seq = 0
        self._lock = threading.Lock()

    def rebind(self, deadline: float) -> None:
        """Move the absolute stream deadline between records, never mid-record."""
        self.deadline = deadline

    def entry(self) -> bool:
        """Minimal earliest record: exactly {event, pid, monotonic_ns}, no sequence."""
        with self._lock:
            line = json.dumps({"event": ENTRY_EVENT, "pid": os.getpid(),
                               "monotonic_ns": time.monotonic_ns()})
            return self._write(line, min(time.monotonic() + ENTRY_WRITE_BOUND_SECONDS,
                                         self.deadline))

    def meta(self) -> bool:
        """Second record: boot identity and interpreter metadata."""
        return self.record(META_EVENT, _meta_data())

    def stage_started(self, phase: str, *, bound: float = WRITE_ATTEMPT_BOUND_SECONDS,
                      until: float | None = None) -> bool:
        return self.record(STAGE_STARTED_EVENT, {"phase": phase}, bound=bound, until=until)

    def stage_completed(self, phase: str, *, bound: float = WRITE_ATTEMPT_BOUND_SECONDS,
                        until: float | None = None) -> bool:
        return self.record(STAGE_COMPLETED_EVENT, {"phase": phase}, bound=bound, until=until)

    def heartbeat(self, data: object, *, bound: float = WRITE_ATTEMPT_BOUND_SECONDS,
                  until: float | None = None) -> bool:
        return self.record(HEARTBEAT_EVENT, data, bound=bound, until=until)

    def transition(self, event: str, data: object = None, *,
                   bound: float = WRITE_ATTEMPT_BOUND_SECONDS,
                   until: float | None = None) -> bool:
        """One named lifecycle transition (cleanup_started, egress_started, ...)."""
        return self.record(event, data, bound=bound, until=until)

    def terminal(self, event: str, data: object) -> bool:
        """Terminal delivery is bounded by the 5 s egress window, never the full stream.

        The deadline is min(now + WRITE_ATTEMPT_BOUND_SECONDS, stream deadline):
        WRITE_ATTEMPT_BOUND_SECONDS mirrors deadline.EGRESS_BOUND_SECONDS (the
        terminal-result delivery window), so a long remaining stream window can
        never hand the terminal record more than the egress bound.
        """
        return self.record(event, data)

    def record(self, event: str, data: object = None, *,
               bound: float = WRITE_ATTEMPT_BOUND_SECONDS,
               until: float | None = None) -> bool:
        """One framed record carrying the next sequence number; False stops the stream.

        The write deadline derives from the write's own bound and the stream
        deadline, further capped by `until` - the enclosing phase deadline (the
        next safety deadline) - so a synchronous diagnostic write can never
        consume the phase it delays (deadline.DIAGNOSTIC_ALLOWANCE_SECONDS).
        """
        with self._lock:
            self.seq += 1
            line = json.dumps({"event": event, "data": data,
                               "monotonic_ns": time.monotonic_ns(), "seq": self.seq})
            deadline = min(time.monotonic() + bound, self.deadline)
            if until is not None:
                deadline = min(deadline, until)
            return self._write(line, deadline)

    def _write(self, line: str, deadline: float) -> bool:
        if self.broken:
            return False
        if not deliver([line], deadline):
            self.broken = True
            return False
        return True


_WRITER: Writer | None = None


def writer() -> Writer:
    """The process-single diagnostic emitter (single-emitter discipline)."""
    global _WRITER
    if _WRITER is None:
        _WRITER = Writer(time.monotonic() + PREAMBLE_STREAM_BOUND_SECONDS)
    return _WRITER
