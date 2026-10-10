# SPDX-License-Identifier: GPL-3.0-or-later
"""Exclusive local transport bytes and fsynced lifecycle, not remote durability.

The parent directory is trusted and already exists. Reservations survive failure;
missing started evidence cannot exclude launch in the Popen-to-journal window.
The frame journal is a bounded local receipt trail for transport stdout: every
complete LF-terminated frame gets a locally timestamped receipt with its byte
offset and length, trailing unterminated data is marked explicitly. It is
flushed per record and fsynced at close; the lifecycle journal stays the
fsynced commit record.
"""
from __future__ import annotations

import json
import os
import time
from contextlib import ExitStack
from dataclasses import dataclass
from pathlib import Path
from typing import BinaryIO, Final, TypedDict


# Bound on per-frame receipts: the tee itself is unbounded and file-backed;
# only the receipt trail stops (with an explicit marker) past this count.
FRAME_RECEIPT_LIMIT: Final = 4096


class CapturePaths(TypedDict):
    stdout: str
    stderr: str
    lifecycle: str
    frames: str


class FrameReceipt(TypedDict):
    event: str
    monotonic: float
    byte_offset: int
    frame_length: int


class Lifecycle(TypedDict, total=False):
    event: str
    monotonic: float
    invocation_count: int
    caller_pid: int
    paths: CapturePaths
    timeout_seconds: float
    deadline_monotonic: float
    child_pid: int
    child_pgid: int
    exit_status: int | None
    timed_out: bool
    reaped: bool
    error: str
    evidence_complete: bool
    pump_disposition: str


def _private_open(path: str, flags: int) -> int:
    return os.open(path, flags, 0o600)


@dataclass(frozen=True, slots=True)
class LocalCapture:
    """Stack-owned exclusive files; raw bytes are never decoded in the artifacts."""

    stdout: BinaryIO
    stderr: BinaryIO
    journal: BinaryIO
    frames: BinaryIO
    paths: CapturePaths

    @classmethod
    def reserve(cls, stack: ExitStack, receipt: Path) -> LocalCapture:
        paths = CapturePaths(stdout=str(receipt.with_suffix('.remote.stdout')),
                             stderr=str(receipt.with_suffix('.remote.stderr')),
                             lifecycle=str(receipt.with_suffix('.lifecycle.jsonl')),
                             frames=str(receipt.with_suffix('.frames.jsonl')))
        stdout = stack.enter_context(open(paths['stdout'], 'x+b', opener=_private_open))
        stderr = stack.enter_context(open(paths['stderr'], 'x+b', opener=_private_open))
        journal = stack.enter_context(open(paths['lifecycle'], 'x+b', opener=_private_open))
        frames = stack.enter_context(open(paths['frames'], 'x+b', opener=_private_open))
        return cls(stdout, stderr, journal, frames, paths)

    def record(self, event: Lifecycle) -> None:
        """Commit a lifecycle transition before returning to its caller."""
        self.journal.write((json.dumps({**event, 'monotonic': time.monotonic()}) + '\n').encode())
        self.journal.flush()
        os.fsync(self.journal.fileno())

    def frame(self, event: str, byte_offset: int, frame_length: int) -> None:
        """One locally timestamped frame receipt in the bounded journal."""
        receipt: FrameReceipt = {"event": event, "monotonic": time.monotonic(),
                                 "byte_offset": byte_offset, "frame_length": frame_length}
        self.frames.write((json.dumps(receipt) + '\n').encode())
        self.frames.flush()

    def close_frames(self) -> None:
        """Durably close the frame journal after the last receipt."""
        self.frames.flush()
        os.fsync(self.frames.fileno())

    def text(self, stream: BinaryIO) -> str:
        # Snapshot without moving the writer's shared offset: descendants may append.
        size = os.fstat(stream.fileno()).st_size
        chunks = [os.pread(stream.fileno(), min(1048576, size - offset), offset)
                  for offset in range(0, size, 1048576)]
        return b''.join(chunks).decode(errors='replace')
