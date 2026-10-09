"""Finite acquisition transport lifecycle and concurrent pipe capture."""
from __future__ import annotations

import hashlib
import os
import selectors
import signal
import subprocess
import time
from contextlib import contextmanager
from collections.abc import Iterator
from dataclasses import dataclass
from pathlib import Path
from types import FrameType

from .kernel import Blocked


@dataclass(frozen=True, slots=True)
class TransportReceipt:
    exit_status: int
    stdout_sha256: str
    stdout_bytes: int
    stderr: str
    timed_out: bool


@contextmanager
def lifecycle_deadline(seconds: float) -> Iterator[None]:
    def expired(signum: int, frame: FrameType | None) -> None:
        raise Blocked("local_total_timeout", str(seconds))
    previous = signal.signal(signal.SIGALRM, expired)
    signal.setitimer(signal.ITIMER_REAL, seconds)
    try:
        yield
    finally:
        signal.setitimer(signal.ITIMER_REAL, 0)
        signal.signal(signal.SIGALRM, previous)


def run_bounded(argv: tuple[str, ...], stdin: bytes, stdout_sink: Path, cap: int,
                timeout_seconds: float) -> TransportReceipt:
    deadline = time.monotonic() + timeout_seconds
    hasher = hashlib.sha256()
    total = 0
    errors = bytearray()
    offset = 0
    timed_out = False
    with subprocess.Popen(argv, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                          stderr=subprocess.PIPE, start_new_session=True) as proc:
        assert proc.stdin and proc.stdout and proc.stderr
        try:
            with selectors.DefaultSelector() as selector, stdout_sink.open("wb") as out, \
                    stdout_sink.with_suffix(".stderr").open("wb") as err_out:
                for stream, event, label in ((proc.stdin, selectors.EVENT_WRITE, "stdin"),
                        (proc.stdout, selectors.EVENT_READ, "stdout"),
                        (proc.stderr, selectors.EVENT_READ, "stderr")):
                    os.set_blocking(stream.fileno(), False)
                    selector.register(stream, event, label)
                while selector.get_map():
                    remaining = deadline - time.monotonic()
                    if remaining <= 0:
                        timed_out = True
                        break
                    for key, _ in selector.select(min(remaining, 0.1)):
                        if key.data == "stdin":
                            if offset == len(stdin):
                                selector.unregister(key.fileobj)
                                proc.stdin.close()
                            else:
                                try:
                                    offset += os.write(key.fd, stdin[offset:offset + 65536])
                                except BrokenPipeError:
                                    selector.unregister(key.fileobj)
                                    proc.stdin.close()
                            continue
                        chunk = os.read(key.fd, 65536)
                        if not chunk:
                            selector.unregister(key.fileobj)
                        elif key.data == "stdout":
                            total += len(chunk)
                            if total > cap:
                                raise Blocked("transport_output_bound", str(total))
                            hasher.update(chunk)
                            out.write(chunk)
                        else:
                            err_out.write(chunk)
                            errors.extend(chunk)
                            if len(errors) > 8 * 1024 * 1024:
                                raise Blocked("transport_stderr_bound", str(len(errors)))
                if timed_out:
                    os.killpg(proc.pid, signal.SIGKILL)
                try:
                    proc.wait(timeout=max(0.001, deadline - time.monotonic()))
                except subprocess.TimeoutExpired:
                    timed_out = True
                    os.killpg(proc.pid, signal.SIGKILL)
                    proc.wait(timeout=5)
        finally:
            if proc.poll() is None:
                os.killpg(proc.pid, signal.SIGKILL)
                proc.wait(timeout=5)
    return TransportReceipt(proc.returncode, hasher.hexdigest(), total,
                            errors.decode(errors="replace"), timed_out or proc.returncode in (124, 137))
