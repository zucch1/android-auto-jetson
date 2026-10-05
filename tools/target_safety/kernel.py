"""Bounded Linux inotify ownership and fail-closed event parsing."""
from __future__ import annotations

import ctypes
import os
import struct
import time
from collections.abc import Iterator
from contextlib import contextmanager
from dataclasses import dataclass
from pathlib import Path
from typing import Final

MUTATIONS: Final = 0x00000FCE
LOSS: Final = 0x0000E000
SELF: Final = 0x00000C00
NOFOLLOW: Final = 0x02000000
MASK_ADD: Final = 0x20000000
# Fixed policy from Oracle ses_efcd07e79ffevkpmjv0yGGmc0r; never auto-grows.
# 1000000 entries + 7 ancestors leave 121; kernel gap 48448 is not reserved.
MAX_WATCHES: Final = 1000128
WINDOW_SECONDS: Final = 900
WINDOW_TIMEOUT_MESSAGE: Final = f"{WINDOW_SECONDS} seconds"
MONITOR_CAPACITY_FILES: Final = (
    "/proc/sys/fs/inotify/max_user_watches",
    "/proc/sys/fs/inotify/max_user_instances",
    "/proc/sys/fs/inotify/max_queued_events",
)


class Blocked(RuntimeError):
    """Typed coverage or safety rejection with its affected path/event."""

    def __init__(self, reason: str, detail: str = "") -> None:
        self.reason = reason
        self.detail = detail
        super().__init__(reason, detail)

    def __str__(self) -> str:
        return f"{self.reason}: {self.detail}"


@dataclass(frozen=True, slots=True)
class Event:
    descriptor: int
    mask: int
    cookie: int
    name: str


class Watch:
    """Mutable watch registry; one deadline bounds setup, reads and draining."""

    def __init__(self) -> None:
        self.libc = ctypes.CDLL(None, use_errno=True)
        self.libc.inotify_init1.argtypes = [ctypes.c_int]
        self.libc.inotify_init1.restype = ctypes.c_int
        self.libc.inotify_add_watch.argtypes = [ctypes.c_int, ctypes.c_char_p, ctypes.c_uint32]
        self.libc.inotify_add_watch.restype = ctypes.c_int
        self.fd: int = self.libc.inotify_init1(os.O_NONBLOCK | os.O_CLOEXEC)
        if self.fd < 0:
            raise OSError(ctypes.get_errno(), "inotify_init1")
        # One monotonic deadline bounds setup, reads and draining; never reset.
        self.deadline = time.monotonic() + WINDOW_SECONDS
        self.paths: dict[int, str] = {}
        # None means entire inode/tree object; sets mean ancestor entry filters.
        self.filters: dict[int, set[str] | None] = {}
        self.violations: list[Event] = []
        self.closed = False

    @classmethod
    @contextmanager
    def session(cls) -> Iterator[Watch]:
        """Drain through cleanup and always release the kernel descriptor."""
        watch = cls()
        try:
            yield watch
        finally:
            try:
                watch.check()
            finally:
                os.close(watch.fd)
                watch.closed = True

    def add(self, path: Path, entry: str | None = None) -> None:
        """Merge filters for shared ancestors/inodes without dropping masks."""
        self.check()
        if len(self.paths) >= MAX_WATCHES:
            raise Blocked("watch_limit", str(path))
        wd: int = self.libc.inotify_add_watch(
            self.fd, os.fsencode(path), MUTATIONS | LOSS | NOFOLLOW | MASK_ADD,
        )
        if wd < 0:
            raise OSError(ctypes.get_errno(), "inotify_add_watch", str(path))
        if wd not in self.filters:
            self.filters[wd] = set() if entry is not None else None
        selected = self.filters[wd]
        if entry is None:
            self.filters[wd] = None
        elif selected is not None:
            selected.add(entry)
        self.paths[wd] = str(path)
        self.check()

    def consume(self, data: bytes) -> None:
        """Parse complete kernel frames; synthetic callers exercise this boundary."""
        offset = 0
        while offset < len(data):
            if len(data) - offset < 16:
                raise Blocked("malformed_event", str(len(data) - offset))
            descriptor, mask, cookie, length = struct.unpack_from("iIII", data, offset)
            if length > len(data) - offset - 16 or length % 4:
                raise Blocked("malformed_event_length", str(length))
            name = os.fsdecode(data[offset + 16:offset + 16 + length].split(b"\0", 1)[0])
            event = Event(descriptor, mask, cookie, name)
            selected = self.filters.get(event.descriptor)
            if (event.mask & (LOSS | SELF) or event.descriptor not in self.paths
                    or selected is None or event.name in selected):
                self.violations.append(event)
            offset += 16 + length
        if self.violations:
            first = self.violations[0]
            raise Blocked("watch_violation", f"{self.paths.get(first.descriptor)} {first}")

    def check(self) -> None:
        """Drain the whole queue; never accept overflow, unknown or lost watches."""
        while True:
            if time.monotonic() >= self.deadline:
                raise Blocked("window_timeout", WINDOW_TIMEOUT_MESSAGE)
            try:
                data = os.read(self.fd, 1024 * 1024)
            except BlockingIOError:
                if self.violations:
                    first = self.violations[0]
                    raise Blocked("window_invalidated",
                                  f"{self.paths.get(first.descriptor)} {first}") from None
                return
            if not data:
                raise Blocked("watch_eof")
            try:
                self.consume(data)
            except Blocked as error:
                if error.reason != "watch_violation":
                    raise
                # Retain every violation while draining remaining kernel packets.
                continue


def monitor_capacities(watch: Watch) -> dict[str, str | None]:
    """Setup diagnostics only after session start; never a capacity pass or kernel write.

    Actual watch installation through protect() remains required; these readings
    only record the host inotify ceilings and prove no capacity on their own.
    """
    watch.check()
    readings: dict[str, str | None] = {}
    for name in MONITOR_CAPACITY_FILES:
        path = Path(name)
        readings[name] = path.read_text().strip() if path.is_file() else None
        watch.check()
    return readings
