"""Selector and original-pipe lifetime ownership for bounded collection.

Owns the child's original stdout/stderr/stdin pipes independently of selector
construction and registration, so every path — construction fault, register
fault, select fault, unregister fault, close fault or unexpected error — still
attempts the selector close and EVERY remaining pipe close. Expected IO faults
are recorded for the caller and surface as a rejected collection result; an
unexpected error propagates only after all closures were attempted.
"""
from __future__ import annotations

import os
import selectors
from collections.abc import Callable
from dataclasses import dataclass
from typing import IO, Literal

Role = Literal["out", "err", "in"]
FaultSink = Callable[[str], None]

_READ_ROLES: tuple[Role, Role] = ("out", "err")
_ALL_ROLES: tuple[Role, Role, Role] = ("out", "err", "in")


@dataclass(frozen=True, slots=True)
class ChildStreams:
    """The child's original pipe views, owned for close independent of selector state."""

    stdout: IO[bytes] | None
    stderr: IO[bytes] | None
    stdin: IO[bytes] | None


@dataclass(frozen=True, slots=True)
class ReadyEvent:
    """One selector readiness event typed by stream role."""

    role: Role
    fd: int
    fileobj: IO[bytes]


@dataclass(frozen=True, slots=True)
class SelectBatch:
    """Outcome of one bounded selector wait: ready events and a fault flag."""

    events: tuple[ReadyEvent, ...]
    faulted: bool


class CollectionIO:
    """Owns the selector and the original child pipes for one bounded collection."""

    def __init__(self, streams: ChildStreams, record: FaultSink) -> None:
        self._record = record
        self._open: dict[Role, IO[bytes]] = {}
        for role, stream in (("out", streams.stdout), ("err", streams.stderr),
                             ("in", streams.stdin)):
            if stream is not None:
                self._open[role] = stream
        self._registered: dict[Role, IO[bytes]] = {}
        self._selector: selectors.BaseSelector | None = None

    @property
    def registered(self) -> bool:
        """True while any stream is still registered with the selector."""
        return bool(self._registered)

    def start(self, *, want_stdin: bool) -> bool:
        """Build the selector and register owned streams.

        Returns False after a recorded expected IO setup fault; the owned pipes
        stay owned so close_all still closes them. A non-IO failure propagates.
        """
        try:
            selector = selectors.DefaultSelector()
        except OSError as error:
            self._record(f"selector_construction_fault {error}")
            return False
        self._selector = selector
        try:
            for role in _READ_ROLES:
                stream = self._open.get(role)
                if stream is None:
                    continue
                os.set_blocking(stream.fileno(), False)
                selector.register(stream, selectors.EVENT_READ, role)
                self._registered[role] = stream
            stdin = self._open.get("in")
            if stdin is not None and want_stdin:
                os.set_blocking(stdin.fileno(), False)
                selector.register(stdin, selectors.EVENT_WRITE, "in")
                self._registered["in"] = stdin
            elif stdin is not None:
                self.close_role("in")
        except OSError as error:
            self._record(f"selector_register_fault {error}")
            return False
        return True

    def select_ready(self, timeout: float) -> SelectBatch:
        """Wait up to timeout for readiness; an expected IO fault is recorded."""
        selector = self._selector
        if selector is None:
            raise RuntimeError("select_ready before successful start")
        try:
            ready = selector.select(timeout)
        except OSError as error:
            self._record(f"selector_select_fault {error}")
            return SelectBatch((), True)
        events: list[ReadyEvent] = []
        for key, _mask in ready:
            role = self._role_for(key.fd)
            if role is not None:
                events.append(ReadyEvent(role, key.fd, self._registered[role]))
        return SelectBatch(tuple(events), False)

    def unregister(self, event: ReadyEvent) -> None:
        """Drop one registration; an expected IO fault is recorded and kept retryable."""
        selector = self._selector
        if selector is None or self._registered.get(event.role) is not event.fileobj:
            return
        try:
            selector.unregister(event.fileobj)
        except OSError as error:
            self._record(f"selector_unregister_fault {error}")
            return
        del self._registered[event.role]

    def close_role(self, role: Role) -> None:
        """Unregister then close one owned pipe once; expected IO faults are recorded."""
        stream = self._open.pop(role, None)
        if stream is None:
            return
        self.unregister(ReadyEvent(role, stream.fileno(), stream))
        try:
            stream.close()
        except OSError as error:
            self._record(f"close_fault {role} {error}")

    def close_all(self) -> BaseException | None:
        """Attempt every remaining close and return the first unexpected fault.

        Every remaining registration is dropped, then the selector, then every
        remaining pipe: a fault in one closer never skips the rest. Expected IO
        faults are recorded; the first non-IO fault is returned for the caller
        to propagate once closure is complete.
        """
        unexpected: BaseException | None = None
        selector = self._selector
        self._selector = None
        for role in _ALL_ROLES:
            stream = self._registered.pop(role, None)
            if stream is None or selector is None:
                continue
            try:
                selector.unregister(stream)
            except OSError as error:
                self._record(f"selector_unregister_fault {error}")
            except BaseException as error:
                unexpected = error if unexpected is None else unexpected
        if selector is not None:
            unexpected = self._close_closer("selector", selector.close, unexpected)
        for role in _ALL_ROLES:
            stream = self._open.pop(role, None)
            if stream is not None:
                unexpected = self._close_closer(role, stream.close, unexpected)
        return unexpected

    def _role_for(self, fd: int) -> Role | None:
        for role, stream in self._registered.items():
            if stream.fileno() == fd:
                return role
        return None

    def _close_closer(self, what: str, closer: Callable[[], None],
                      unexpected: BaseException | None) -> BaseException | None:
        """Run one closer, recording IO faults and deferring any unexpected fault."""
        try:
            closer()
        except OSError as error:
            self._record(f"close_fault {what} {error}")
        except BaseException as error:
            return error if unexpected is None else unexpected
        return unexpected
