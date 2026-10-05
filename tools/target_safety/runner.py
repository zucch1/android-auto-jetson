"""Local transport: a bounded subprocess in its own process group.

Owns the child process group (SSH when wired remotely) and preserves partial
stdout/stderr in exclusive local files. Transport stdout is pumped: a tee
writes every byte to the file-backed capture as soon as it crosses the
transport and journals locally timestamped receipts for complete LF-terminated
frames (byte offsets, bounded trail; trailing partial data marked), so remote
monotonic emission times gain a local receipt timeline. The kill always
escalates to a group KILL so TERM-ignoring descendants in the group are
reached; the pump drains until EOF or its own absolute deadline, so escaped
descendants cannot hang pipe collection. No whole-tree guarantee beyond the
process group; best-effort only for SIGKILL/power loss.
"""
from __future__ import annotations

import contextlib
import os
import select
import signal
import subprocess
import sys
import threading
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Final

if __package__: from .local_capture import FRAME_RECEIPT_LIMIT, LocalCapture, Lifecycle
else: from local_capture import FRAME_RECEIPT_LIMIT, LocalCapture, Lifecycle

RUN_KILL_GRACE_SECONDS: Final = 5.0
# Max bytes of an unterminated tail retained in memory; its full LENGTH is still
# journaled and every byte still lands in the file-backed capture.
TAIL_RETAIN_BYTES: Final = 4096
PUMP_EOF: Final = "eof"
PUMP_DEADLINE: Final = "deadline"
PUMP_ERROR: Final = "error"
PUMP_UNFINISHED: Final = "unfinished"


class PumpDisposition:
    """Mutable pump outcome slot: the pump thread fills it, run_owned reads it.

    "eof" is the only complete drain; "deadline" (pump bound hit, bytes may be
    missing), "error" (capture/journaling OSError) and "unfinished" (thread
    still alive at the join bound) are all incomplete evidence and can never be
    accepted as an intact transport result.
    """

    def __init__(self) -> None:
        self.outcome = PUMP_UNFINISHED
        self.detail: str | None = None

    def finish(self, outcome: str, detail: str | None = None) -> None:
        self.outcome, self.detail = outcome, detail


@dataclass(frozen=True, slots=True)
class RunResult:
    """Captured result of one owned transport run, including timeout disposition.

    evidence_complete is the pump's checked disposition: True only when the
    pump drained to EOF with every capture/frame write intact. A journaling or
    capture OSError, the pump deadline or an unfinished thread make it False,
    so a failed-evidence run can never look like exit-0 intact success.
    """

    argv: tuple[str, ...]
    exit_status: int
    stdout: str
    stderr: str
    timed_out: bool
    evidence_complete: bool


def run_owned(
    argv: tuple[str, ...],
    *,
    capture: LocalCapture,
    timeout_seconds: float,
    stdin: bytes | None = None,
    cwd: Path | None = None,
) -> RunResult:
    """Require durable reservation before one launch; own bounded unwind cleanup."""
    capture.record(Lifecycle(event='reserved', invocation_count=1, caller_pid=os.getpid(),
                             paths=capture.paths, timeout_seconds=timeout_seconds,
                             deadline_monotonic=time.monotonic() + timeout_seconds))
    timed_out = False
    read_fd, write_fd = os.pipe()
    try:
        proc = subprocess.Popen(
            argv, stdin=subprocess.PIPE if stdin is not None else subprocess.DEVNULL,
            stdout=write_fd, stderr=capture.stderr, start_new_session=True, cwd=cwd)
    except OSError as error:
        os.close(read_fd)
        os.close(write_fd)
        capture.record(Lifecycle(event='launch_failed', error=str(error), reaped=False))
        raise
    os.close(write_fd)
    pump_deadline = time.monotonic() + timeout_seconds + 2 * RUN_KILL_GRACE_SECONDS
    disposition = PumpDisposition()
    pump = threading.Thread(target=_pump_stdout, args=(read_fd, capture, pump_deadline, disposition),
                            daemon=True)
    # Started before any later journal write can fail, so unwind always joins a
    # live pump and read_fd ownership is unambiguous.
    pump.start()
    try:
        with contextlib.ExitStack() as stack:
            if proc.stdin is not None:
                stack.enter_context(proc.stdin)
            capture.record(Lifecycle(event='started', child_pid=proc.pid, child_pgid=proc.pid))
            print('{"event":"committed_child_start","child_pid":%d}' % proc.pid,
                  file=sys.stderr, flush=True)
            try:
                proc.communicate(input=stdin, timeout=timeout_seconds)
            except subprocess.TimeoutExpired:
                timed_out = True
                _kill_group(proc)
        stopped = _join_pump(pump, pump_deadline)
        outcome = disposition.outcome if stopped else PUMP_UNFINISHED
        evidence_complete = outcome == PUMP_EOF
        capture.record(Lifecycle(event='terminal', exit_status=proc.returncode,
                                 timed_out=timed_out, reaped=proc.returncode is not None,
                                 evidence_complete=evidence_complete, pump_disposition=outcome))
    except (OSError, KeyboardInterrupt, SystemExit) as error:
        _kill_group(proc)
        _join_pump(pump, pump_deadline)
        try:
            capture.record(Lifecycle(event='unwound', error=type(error).__name__,
                                     exit_status=proc.returncode, timed_out=timed_out,
                                     reaped=proc.returncode is not None))
        except OSError as journal_error:
            raise error from journal_error
        raise
    return RunResult(
        argv=argv,
        exit_status=proc.returncode if proc.returncode is not None else -1,
        stdout=capture.text(capture.stdout),
        stderr=capture.text(capture.stderr),
        timed_out=timed_out,
        evidence_complete=evidence_complete,
    )


def _pump_stdout(read_fd: int, capture: LocalCapture, deadline: float,
                 slot: PumpDisposition) -> None:
    """Tee transport stdout to the capture file and journal frame receipts.

    File-backed capture is preserved: every byte reaches the capture file as
    soon as it crosses the transport. Each complete LF-terminated frame gets
    one locally timestamped receipt with its byte offset and length in the
    bounded journal (FRAME_RECEIPT_LIMIT, then one frames_capped marker);
    trailing unterminated data is marked partial_tail with its full LENGTH
    counted while at most TAIL_RETAIN_BYTES are held in memory. The pump stops
    at EOF or at its absolute deadline (escaped pipe holders cannot hang it)
    and reports which through the disposition slot; a capture or journaling
    OSError marks the slot errored and stops the pump, keeping every byte the
    file already holds as evidence.
    """
    pending = b""
    pending_length = 0
    base = 0
    receipts = 0
    outcome = PUMP_DEADLINE
    try:
        try:
            while True:
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    break
                if not select.select([read_fd], [], [], remaining)[0]:
                    break
                chunk = os.read(read_fd, 65536)
                if not chunk:
                    outcome = PUMP_EOF
                    break
                capture.stdout.write(chunk)
                capture.stdout.flush()
                while chunk:
                    index = chunk.find(b"\n")
                    if index < 0:
                        pending_length += len(chunk)
                        pending += chunk
                        if len(pending) > TAIL_RETAIN_BYTES:
                            pending = pending[-TAIL_RETAIN_BYTES:]
                        break
                    frame_length = pending_length + index + 1
                    if receipts < FRAME_RECEIPT_LIMIT:
                        capture.frame("frame", base, frame_length)
                        receipts += 1
                        if receipts == FRAME_RECEIPT_LIMIT:
                            capture.frame("frames_capped", base + frame_length, 0)
                    base += frame_length
                    pending = b""
                    pending_length = 0
                    chunk = chunk[index + 1:]
            slot.finish(outcome)
            if pending_length:
                capture.frame("partial_tail", base, pending_length)
            capture.close_frames()
        except OSError as error:
            slot.finish(PUMP_ERROR, f"{type(error).__name__}: {error}")
    finally:
        os.close(read_fd)


def _join_pump(pump: threading.Thread, deadline: float) -> bool:
    """Bounded pump join so an escaped pipe holder can never hang collection.

    Returns whether the pump thread actually stopped: an unfinished pump means
    incomplete evidence and is never accepted.
    """
    pump.join(timeout=max(0.0, deadline - time.monotonic()) + 1.0)
    return not pump.is_alive()


def _kill_group(proc: subprocess.Popen[bytes]) -> None:
    try:
        pgid = os.getpgid(proc.pid)
    except ProcessLookupError:
        pgid = proc.pid
    for sig in (signal.SIGTERM, signal.SIGKILL):
        with contextlib.suppress(ProcessLookupError):
            os.killpg(pgid, sig)
        _reap(proc, RUN_KILL_GRACE_SECONDS)


def _reap(proc: subprocess.Popen[bytes], grace: float) -> bool:
    try:
        proc.wait(timeout=grace)
        return True
    except subprocess.TimeoutExpired:
        return False
