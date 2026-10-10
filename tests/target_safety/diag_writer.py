# SPDX-License-Identifier: GPL-3.0-or-later
# /// script
# requires-python = ">=3.11"
# dependencies = []
# ///
# Run: python3 -B tests/target_safety/diag_writer.py
"""Bounded diagnostic-write discipline: partial writes, EPIPE, deadlines, truncation.

Real local pipes and the real deliver()/Writer code path only; injected os.write
faults are labeled 'injected (scripted)'. No kernel mutation and no target
contact. The receiver half is covered by feeding torn transcripts to the strict
parsers and checking that evidence is preserved while the stream is marked
incomplete.
"""
from __future__ import annotations

import io
import json
import os
import sys
import threading
import time
from contextlib import redirect_stdout, suppress
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
sys.path.insert(0, str(Path(__file__).resolve().parent))
from target_safety import deadline, egress
from target_safety.egress import Writer, deliver
from target_safety.sizing_validation import parse_sizing_child
from sizing_outcomes import complete, ready, transcript


class FdStream:
    """stdout stand-in exposing one raw fd so the bounded fd path runs unmodified."""

    def __init__(self, fd: int) -> None:
        self._fd = fd

    def fileno(self) -> int:
        return self._fd


def entry_record_is_minimal() -> None:
    # Given: one writer on the local test seam.
    out = io.StringIO()
    with redirect_stdout(out):
        writer = Writer(time.monotonic() + 5.0)
        emitted = writer.entry()
    # When: the minimal entry record is framed. Then: exactly {event, pid, monotonic_ns}.
    record = json.loads(out.getvalue())
    assert emitted and set(record) == {"event", "pid", "monotonic_ns"}, record
    assert record["event"] == "remote_entry" and type(record["pid"]) is int and record["pid"] > 0
    assert type(record["monotonic_ns"]) is int and record["monotonic_ns"] >= 0
    assert "seq" not in record and "data" not in record
    print("PASS entry record is the minimal {event, pid, monotonic_ns} shape")


def partial_writes_resume_within_deadline() -> None:
    # Given: a real pipe and an injected partial os.write (one third per call).
    reader, write_fd = os.pipe()
    try:
        real_write = os.write

        def partial(fd: int, data: bytes) -> int:
            return real_write(fd, data[:max(1, len(data) // 3)])

        with patch.object(egress.sys, "stdout", FdStream(write_fd)), \
                patch.object(egress.os, "write", partial):
            delivered = deliver(["hello-partial-world"], time.monotonic() + 5.0)
        os.close(write_fd)
        write_fd = -1
        # Then: the partial writes resume inside the deadline and framing survives.
        assert delivered
        assert os.read(reader, 4096) == b"hello-partial-world\n"
    finally:
        os.close(reader)
        if write_fd >= 0:
            os.close(write_fd)
    print("PASS partial writes resume within the deadline; one framed line delivered")


def epipe_stops_further_diagnostic_attempts() -> None:
    # Given: a real pipe whose read end is closed (real EPIPE on write).
    reader, write_fd = os.pipe()
    os.close(reader)
    try:
        writes: list[int] = []
        real_write = os.write

        def counted(fd: int, data: bytes) -> int:
            writes.append(len(data))
            return real_write(fd, data)

        writer = Writer(time.monotonic() + 5.0)
        with patch.object(egress.sys, "stdout", FdStream(write_fd)), \
                patch.object(egress.os, "write", counted):
            first = writer.record("stage_started", {"phase": "probe"})
            attempts = len(writes)
            second = writer.record("stage_completed", {"phase": "probe"})
            third = writer.record("stage_completed", {"phase": "probe"})
        # Then: the first write latches the broken channel and nothing is retried.
        assert not first and writer.broken and not second and not third
        assert len(writes) == attempts == 1, writes
    finally:
        os.close(write_fd)
    print("PASS real EPIPE marks the channel broken and stops every further attempt")


def backpressure_timeout_latches_the_channel() -> None:
    # Given: a real pipe filled past its capacity with a reader that never drains.
    reader, write_fd = os.pipe()
    try:
        os.set_blocking(write_fd, False)
        with suppress(BlockingIOError):
            while True:
                os.write(write_fd, b"x" * 4096)
        os.set_blocking(write_fd, True)
        writer = Writer(time.monotonic() + 0.2)
        with patch.object(egress.sys, "stdout", FdStream(write_fd)):
            started = time.monotonic()
            emitted = writer.record("heartbeat", {"records": 0})
            elapsed = time.monotonic() - started
        # Then: the write is bounded by its absolute deadline and latches.
        assert not emitted and writer.broken and elapsed < 1.5, elapsed
        assert not writer.terminal("sizing_outcome", {})
    finally:
        os.close(reader)
        os.close(write_fd)
    print(f"PASS backpressure write failed in {elapsed:.2f}s (bounded) and latched the stream")


def sequence_numbers_are_contiguous_and_serialized() -> None:
    # Given: one writer driven concurrently from two emitters.
    out = io.StringIO()
    writer = Writer(time.monotonic() + 5.0)

    def emit(_index: int) -> None:
        for _ in range(25):
            writer.record("guard_result", 0)

    with redirect_stdout(out):
        threads = [threading.Thread(target=emit, args=(index,)) for index in range(2)]
        for thread in threads:
            thread.start()
        for thread in threads:
            thread.join()
    seqs = [json.loads(line)["seq"] for line in out.getvalue().splitlines()]
    # Then: records never interleave mid-line and sequence numbers stay contiguous.
    assert seqs == list(range(1, 51)), seqs[:10]
    print("PASS 50 concurrent emits serialize to contiguous sequence numbers 1..50")


def torn_transcripts_are_marked_incomplete_with_evidence() -> None:
    # Given: a valid child transcript plus a torn trailing record.
    good = transcript(ready(), complete())
    torn = good + '\n{"event": "topology_sizing_compl'
    # When: the strict parser sees the truncation.
    parsed = parse_sizing_child(torn)
    # Then: the stream is marked incomplete and earlier evidence is preserved.
    assert not parsed.valid and parsed.reason == "malformed_or_truncated_line", parsed.reason
    assert [event.event for event in parsed.events] == ["child_limit_ready", "topology_sizing_complete"]
    broken_meta = parse_sizing_child(good + "\n" + json.dumps(
        {"event": "remote_entry_meta", "data": "not-a-dict-record", "monotonic_ns": 1}))
    assert not broken_meta.valid and broken_meta.reason
    print("PASS torn/foreign records mark the child stream incomplete with evidence preserved")


def terminal_delivery_keeps_the_egress_bound() -> None:
    # Given: a writer whose stream deadline is 100 s away (the whole remaining window).
    captured: list[tuple[list[str], float]] = []
    real_deliver = egress.deliver

    def instrumented(lines: list[str], deadline: float) -> bool:
        captured.append((lines, deadline))
        return real_deliver(lines, deadline)

    out = io.StringIO()
    with redirect_stdout(out), patch.object(egress, "deliver", instrumented):
        writer = Writer(time.monotonic() + 100.0)
        before = time.monotonic()
        emitted = writer.terminal("sizing_outcome", {"accepted": True})
    # When: the terminal record is framed. Then: its write deadline is the 5 s
    # egress bound (min(now + 5, stream deadline)), never the 100 s stream window.
    assert emitted and len(captured) == 1, captured
    line, deadline = captured[0]
    assert json.loads(line[0])["event"] == "sizing_outcome"
    assert deadline <= before + egress.WRITE_ATTEMPT_BOUND_SECONDS + 0.05, deadline
    assert deadline < time.monotonic() + 50.0, deadline
    print("PASS terminal delivery capped at the 5 s egress bound even with 100 s remaining")


def synchronous_write_deadline_is_capped_by_the_phase_deadline() -> None:
    # Given: a writer whose stream deadline is 100 s away and one enclosing
    # phase deadline 0.2 s away (the next safety deadline).
    captured: list[float] = []
    real_deliver = egress.deliver

    def instrumented(lines: list[str], write_deadline: float) -> bool:
        captured.append(write_deadline)
        return real_deliver(lines, write_deadline)

    out = io.StringIO()
    with redirect_stdout(out), patch.object(egress, "deliver", instrumented):
        writer = Writer(time.monotonic() + 100.0)
        before = time.monotonic()
        emitted = writer.record("guard_result", 0, until=before + 0.2)
    # When: the synchronous diagnostic write is framed. Then: its deadline is
    # the phase deadline, never the 100 s stream window or the 5 s write bound.
    assert emitted and captured and captured[0] <= before + 0.2 + 0.05, captured
    assert captured[0] < before + 1.0, captured
    print("PASS synchronous write deadline capped by the enclosing phase deadline")


def heartbeat_write_is_capped_by_the_diagnostic_allowance() -> None:
    # Given: a writer with a far stream deadline and the explicit allowance.
    captured: list[float] = []
    real_deliver = egress.deliver

    def instrumented(lines: list[str], write_deadline: float) -> bool:
        captured.append(write_deadline)
        return real_deliver(lines, write_deadline)

    out = io.StringIO()
    with redirect_stdout(out), patch.object(egress, "deliver", instrumented):
        writer = Writer(time.monotonic() + 100.0)
        before = time.monotonic()
        emitted = writer.heartbeat({"records": 0}, bound=deadline.DIAGNOSTIC_ALLOWANCE_SECONDS)
    # When: the collection heartbeat is framed. Then: its deadline is the
    # aggregate diagnostic allowance, so the 0.1 s sampling cadence slips at
    # most that much and never the 5 s write bound.
    allowance = deadline.DIAGNOSTIC_ALLOWANCE_SECONDS
    assert emitted and captured and captured[0] <= before + allowance + 0.05, captured
    assert captured[0] < before + 2.0 * allowance, captured
    print("PASS heartbeat write capped by the explicit DIAGNOSTIC_ALLOWANCE")


def main() -> int:
    entry_record_is_minimal()
    partial_writes_resume_within_deadline()
    epipe_stops_further_diagnostic_attempts()
    backpressure_timeout_latches_the_channel()
    sequence_numbers_are_contiguous_and_serialized()
    torn_transcripts_are_marked_incomplete_with_evidence()
    terminal_delivery_keeps_the_egress_bound()
    synchronous_write_deadline_is_capped_by_the_phase_deadline()
    heartbeat_write_is_capped_by_the_diagnostic_allowance()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
