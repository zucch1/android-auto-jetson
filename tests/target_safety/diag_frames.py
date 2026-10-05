# /// script
# requires-python = ">=3.11"
# dependencies = []
# ///
# Run: python3 -B tests/target_safety/diag_frames.py
"""Local frame-receipt journal: LF frames with byte offsets, bounded trail, partial tail.

Real local children through the real run_owned pump: the capture stays
file-backed (every byte lands in the file as it crosses the transport), the
lifecycle journal keeps its exact reserved/started/terminal discipline, and the
frame journal carries locally timestamped receipts for every complete
LF-terminated frame plus an explicit trailing-partial marker. Kill-group and
owned-timeout behavior are preserved (covered jointly with ceiling_guard and
ceiling_durable).
"""
from __future__ import annotations

import json
import os
import sys
import tempfile
import time
from contextlib import ExitStack
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
sys.path.insert(0, str(Path(__file__).resolve().parent))
from target_safety import runner
from target_safety.local_capture import FRAME_RECEIPT_LIMIT, LocalCapture


def records(receipt: Path, suffix: str) -> list:
    return [json.loads(line) for line in receipt.with_suffix(suffix).read_text().splitlines()]


def frames_journal_offsets_and_partial_tail() -> None:
    # Given: a child emitting three LF frames and one unterminated tail.
    with tempfile.TemporaryDirectory(dir="/tmp/opencode") as directory, ExitStack() as stack:
        receipt = Path(directory) / "frames.json"
        capture = LocalCapture.reserve(stack, receipt)
        child = ("import os\n"
                 "os.write(1, b\'alpha\\n\')\n"
                 "os.write(1, b\'beta\\n\')\n"
                 "os.write(1, b\'gamma\\n\')\n"
                 "os.write(1, b\'torn-tail\')\n")
        result = runner.run_owned((sys.executable, "-B", "-c", child), capture=capture,
                                  timeout_seconds=5)
        # When: the pump tees the stream. Then: every complete frame is journaled
        # with its byte offset and length, the tail is marked, bytes are intact.
        assert result.exit_status == 0 and not result.timed_out
        assert Path(capture.paths["stdout"]).read_bytes() == b"alpha\nbeta\ngamma\ntorn-tail"
        frames = records(receipt, ".frames.jsonl")
        assert [(f["event"], f["byte_offset"], f["frame_length"]) for f in frames] == [
            ("frame", 0, 6), ("frame", 6, 5), ("frame", 11, 6),
            ("partial_tail", 17, 9)], frames
        assert all(type(f["monotonic"]) is float and f["monotonic"] > 0 for f in frames)
        # Then: the lifecycle journal is untouched by frame receipts.
        assert [event["event"] for event in records(receipt, ".lifecycle.jsonl")] == [
            "reserved", "started", "terminal"]
    print("PASS frame receipts carry local timestamps and byte offsets; torn tail marked")


def frames_journal_is_bounded() -> None:
    # Given: a child emitting more tiny frames than the journal bound.
    count = FRAME_RECEIPT_LIMIT + 5
    with tempfile.TemporaryDirectory(dir="/tmp/opencode") as directory, ExitStack() as stack:
        receipt = Path(directory) / "bounded.json"
        capture = LocalCapture.reserve(stack, receipt)
        child = f"import os\n[os.write(1, b\'x\\n\') for _ in range({count})]"
        result = runner.run_owned((sys.executable, "-B", "-c", child), capture=capture,
                                  timeout_seconds=10)
        # When: the trail fills. Then: it stops at the bound with an explicit
        # marker while the file-backed capture keeps every byte.
        assert result.exit_status == 0 and not result.timed_out
        frames = records(receipt, ".frames.jsonl")
        kinds = [frame["event"] for frame in frames]
        assert kinds.count("frame") == FRAME_RECEIPT_LIMIT, len(kinds)
        assert kinds.count("frames_capped") == 1 and kinds[-1] == "frames_capped", kinds[-3:]
        assert Path(capture.paths["stdout"]).read_bytes() == b"x\n" * count
    print(f"PASS frame journal bounded at {FRAME_RECEIPT_LIMIT} receipts with frames_capped marker")


def timeout_keeps_partial_frames_and_owned_kill() -> None:
    # Given: a child that emits one frame and one torn tail, then hangs.
    with tempfile.TemporaryDirectory(dir="/tmp/opencode") as directory, ExitStack() as stack:
        receipt = Path(directory) / "timeout.json"
        capture = LocalCapture.reserve(stack, receipt)
        child = ("import os, signal\n"
                 "os.write(1, b\'frame\\n\')\n"
                 "os.write(1, b\'torn\')\n"
                 "signal.pause()\n")
        started = time.monotonic()
        result = runner.run_owned((sys.executable, "-B", "-c", child), capture=capture,
                                  timeout_seconds=0.5)
        # When: the owned timeout fires. Then: run_owned kills the group, returns
        # bounded, and the receipts still localize the received frames.
        elapsed = time.monotonic() - started
        assert result.timed_out and result.exit_status < 0 and elapsed < 15.0, elapsed
        frames = records(receipt, ".frames.jsonl")
        assert [(f["event"], f["byte_offset"], f["frame_length"]) for f in frames] == [
            ("frame", 0, 6), ("partial_tail", 6, 4)], frames
        assert result.stdout == "frame\ntorn"
    print(f"PASS owned timeout kept partial frames in {elapsed:.1f}s with the kill group")


def failed_journaling_is_a_visible_transport_failure() -> None:
    # Given: a child emitting one frame whose frame journal raises OSError
    # (Oracle repro: injected capture.frame() failure).
    def broken_frame(self: LocalCapture, event: str, byte_offset: int, frame_length: int) -> None:
        raise OSError("injected frame journal fault")

    with tempfile.TemporaryDirectory(dir="/tmp/opencode") as directory, ExitStack() as stack:
        receipt = Path(directory) / "journal.json"
        capture = LocalCapture.reserve(stack, receipt)
        with patch.object(LocalCapture, "frame", broken_frame):
            result = runner.run_owned((sys.executable, "-B", "-c", "print('frame')"),
                                      capture=capture, timeout_seconds=5)
        # When: the pump journals. Then: the failure is a visible transport
        # failure - never exit 0 with intact-looking evidence - and the terminal
        # lifecycle record carries the failed disposition.
        assert result.exit_status == 0 and not result.timed_out
        assert result.evidence_complete is False, result
        terminal = records(receipt, ".lifecycle.jsonl")[-1]
        assert terminal["event"] == "terminal"
        assert terminal["evidence_complete"] is False, terminal
        assert terminal["pump_disposition"] == "error", terminal
        # Then: the file-backed capture still holds every received byte.
        assert Path(capture.paths["stdout"]).read_bytes() == b"frame\n"
    print("PASS injected frame-journal OSError surfaces as failed transport evidence")


def unfinished_pump_marks_evidence_incomplete() -> None:
    # Given: a pump thread that never stops (blocked sink) and a fast child.
    def stuck(read_fd: int, capture: LocalCapture, deadline: float, slot: object) -> None:
        try:
            time.sleep(30.0)
        finally:
            os.close(read_fd)

    with tempfile.TemporaryDirectory(dir="/tmp/opencode") as directory, ExitStack() as stack:
        receipt = Path(directory) / "stuck.json"
        capture = LocalCapture.reserve(stack, receipt)
        with patch.object(runner, "_pump_stdout", stuck), \
                patch.object(runner, "RUN_KILL_GRACE_SECONDS", 0.05):
            result = runner.run_owned((sys.executable, "-B", "-c", "print('x')"),
                                      capture=capture, timeout_seconds=0.2)
        # When: the join bound hits. Then: the verified-unfinished pump is
        # incomplete evidence and the transport result never looks complete.
        assert result.exit_status == 0 and not result.timed_out
        assert result.evidence_complete is False, result
        terminal = records(receipt, ".lifecycle.jsonl")[-1]
        assert terminal["event"] == "terminal"
        assert terminal["evidence_complete"] is False, terminal
        assert terminal["pump_disposition"] == "unfinished", terminal
    print("PASS unfinished pump (thread still alive at the join bound) rejected as incomplete")


def unterminated_tail_counts_length_beyond_the_retained_cap() -> None:
    # Given: a >TAIL_RETAIN_BYTES LF-less prefix before one framed line, and a
    # pure unterminated tail longer than the retained cap.
    prefix = b"a" * (runner.TAIL_RETAIN_BYTES + 904)
    with tempfile.TemporaryDirectory(dir="/tmp/opencode") as directory, ExitStack() as stack:
        receipt = Path(directory) / "tail.json"
        capture = LocalCapture.reserve(stack, receipt)
        child = (f"import os\nos.write(1, {prefix!r} + b'x\\n')")
        result = runner.run_owned((sys.executable, "-B", "-c", child), capture=capture,
                                  timeout_seconds=5)
        # When: the pump frames. Then: the frame LENGTH counts every dropped
        # tail byte (never just the retained 4096) and the capture is intact.
        assert result.evidence_complete is True, result
        frames = records(receipt, ".frames.jsonl")
        assert [(f["event"], f["byte_offset"], f["frame_length"]) for f in frames] == [
            ("frame", 0, len(prefix) + 2)], frames
        assert Path(capture.paths["stdout"]).read_bytes() == prefix + b"x\n"
    with tempfile.TemporaryDirectory(dir="/tmp/opencode") as directory, ExitStack() as stack:
        receipt = Path(directory) / "tail-only.json"
        capture = LocalCapture.reserve(stack, receipt)
        tail = b"z" * (runner.TAIL_RETAIN_BYTES * 2 + 80)
        child = f"import os\nos.write(1, {tail!r})"
        result = runner.run_owned((sys.executable, "-B", "-c", child), capture=capture,
                                  timeout_seconds=5)
        # Then: the unterminated tail is journaled with its full counted length.
        assert result.evidence_complete is True, result
        frames = records(receipt, ".frames.jsonl")
        assert [(f["event"], f["byte_offset"], f["frame_length"]) for f in frames] == [
            ("partial_tail", 0, len(tail))], frames
        assert Path(capture.paths["stdout"]).read_bytes() == tail
    print("PASS unterminated tail length counted in full; retained bytes capped")


def main() -> int:
    frames_journal_offsets_and_partial_tail()
    frames_journal_is_bounded()
    timeout_keeps_partial_frames_and_owned_kill()
    failed_journaling_is_a_visible_transport_failure()
    unfinished_pump_marks_evidence_incomplete()
    unterminated_tail_counts_length_beyond_the_retained_cap()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
