# /// script
# requires-python = ">=3.10"
# dependencies = []
# ///
# Run only: sudo -n -- /usr/bin/python3 -I -B /absolute/path/acquire_proc_helper.py
"""Privileged read-only /proc scanner: no project imports, inputs, or writes.

16,384 processes permits populated systems without the old 400-entry cutoff.
Limits are fixed: 10 seconds, 16 MiB JSON, 256-byte comm, 4096-byte cwd/stat.
Limit/access failures remain incomplete; only positively identified kernel
threads or zombies with no cwd are excluded, never assigned a fabricated cwd.
"""
from __future__ import annotations

import json
import os
import signal
import sys
from dataclasses import asdict, dataclass
from typing import Final

PROCESS_LIMIT: Final = 16384
OUTPUT_LIMIT: Final = 16 * 1024 * 1024
TIMEOUT_SECONDS: Final = 10


@dataclass(frozen=True, slots=True)
class ProcSample:
    processes: tuple[tuple[str, str, str | None], ...]
    complete: bool
    euid: int
    excluded_nonwriters: int = 0
    reason: str | None = None


def _comm(pid: str) -> str:
    with open(f"/proc/{pid}/comm", "rb") as stream:
        raw = stream.read(257)
    if len(raw) > 256:
        raise OSError("proc comm bound")
    return raw.decode("utf-8", "replace").strip()


def _nonwriter(pid: str) -> bool:
    with open(f"/proc/{pid}/stat", "rb") as stream:
        raw = stream.read(4097)
    if len(raw) > 4096:
        return False
    fields = raw.rsplit(b") ", 1)[-1].split()
    return len(fields) > 6 and (fields[0] == b"Z" or bool(int(fields[6]) & 0x00200000))


def scan() -> ProcSample:
    observed: list[tuple[str, str, str | None]] = []
    complete = True
    excluded = 0
    try:
        pids = sorted((p for p in os.listdir("/proc") if p.isdigit()), key=int)
        if len(pids) > PROCESS_LIMIT:
            return ProcSample((), False, os.geteuid(), reason="process_limit")
        for pid in pids:
            try:
                comm = _comm(pid)
            except FileNotFoundError:
                continue
            except OSError:
                comm = "?"
                complete = False
            try:
                cwd = os.readlink(f"/proc/{pid}/cwd")
                if len(os.fsencode(cwd)) > 4096:
                    cwd = None
            except FileNotFoundError:
                if not os.path.exists(f"/proc/{pid}"):
                    continue
                try:
                    if _nonwriter(pid):
                        excluded += 1
                        continue
                except (OSError, ValueError):
                    complete = False
                cwd = None
            except OSError:
                cwd = None
            if cwd is None:
                complete = False
            observed.append((pid, comm, cwd))
    except OSError:
        complete = False
    return ProcSample(tuple(observed), complete, os.geteuid(), excluded,
                      None if complete else "proc_access_incomplete")


def main() -> int:
    if len(sys.argv) != 1:
        return 64
    if os.geteuid() != 0:
        sample = ProcSample((), False, os.geteuid(), reason="root_visibility_required")
    else:
        # The privileged child bounds itself even if its ordinary-user parent exits.
        signal.signal(signal.SIGALRM, lambda signum, frame: sys.exit(70))
        signal.alarm(TIMEOUT_SECONDS)
        sample = scan()
    raw = json.dumps({"schema": "aa-acquire-proc/1", **asdict(sample)},
                     ensure_ascii=True, separators=(",", ":")).encode() + b"\n"
    if len(raw) > OUTPUT_LIMIT:
        raw = json.dumps({"schema": "aa-acquire-proc/1", **asdict(ProcSample(
            (), False, os.geteuid(), reason="output_limit"))}).encode() + b"\n"
        sample = ProcSample((), False, os.geteuid(), reason="output_limit")
    sys.stdout.buffer.write(raw)
    sys.stdout.buffer.flush()
    return 0 if sample.complete else 70


if __name__ == "__main__":
    raise SystemExit(main())
