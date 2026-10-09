"""Child-limit ready handshake held before the ceiling raise.

A bounded probe child installs the exact child-only RLIMIT_AS ceiling and
reports `child_limit_ready` on stdout. The supervisor holds the ceiling raise
until that handshake is observed (hold-before-raise) inside the existing
operation deadline. No unbounded preexec_fn and no manual stdin write: the
probe is an ordinary bounded subprocess whose stdin is closed. The probe is
stdlib-only so it runs identically from source and from the in-memory bundle.
The optional stage sink brackets the memory-read and admission boundaries so
the caller can stream them through the bounded diagnostic-write discipline.
"""
from __future__ import annotations

import json
import subprocess
import sys
import time
from collections.abc import Callable
from typing import Final

from .resource import (
    ADMISSION_MEM_AVAILABLE_BYTES,
    CHILD_ADDRESS_SPACE_LIMIT_BYTES,
    ResourceError,
    admit,
    install_child_address_space_limit,
    read_mem_available,
)

LIMIT_READY_EVENT: Final = "child_limit_ready"
PROBE_TIMEOUT_MAX_SECONDS: Final = 5.0
_PROBE: Final = (
    "import json, resource, time\n"
    f"resource.setrlimit(resource.RLIMIT_AS, ({CHILD_ADDRESS_SPACE_LIMIT_BYTES}, "
    f"{CHILD_ADDRESS_SPACE_LIMIT_BYTES}))\n"
    "print(json.dumps({'event': '" + LIMIT_READY_EVENT + "', 'data': "
    + str(CHILD_ADDRESS_SPACE_LIMIT_BYTES) + ", 'monotonic_ns': time.monotonic_ns()}), flush=True)\n"
)


def _no_stage(_phase: str, _started: bool) -> None:
    """Default boundary sink: the handshake itself never writes diagnostics."""


def handshake(deadline: float) -> str:
    """Run the bounded limit probe; require its exact readiness event before return."""
    timeout = min(PROBE_TIMEOUT_MAX_SECONDS, max(0.0, deadline - time.monotonic()))
    if timeout <= 0:
        raise ResourceError("limit_readiness_unproven", "no remaining handshake budget")
    try:
        result = subprocess.run((sys.executable, "-B", "-c", _PROBE), stdin=subprocess.DEVNULL,
                                capture_output=True, check=False, timeout=timeout)
    except subprocess.TimeoutExpired as error:
        raise ResourceError("limit_readiness_unproven", f"handshake timeout {timeout}") from error
    except OSError as error:
        raise ResourceError("limit_readiness_unproven", f"handshake launch {error}") from error
    lines = [line for line in result.stdout.decode(errors="replace").splitlines() if line.strip()]
    if result.returncode != 0 or len(lines) != 1:
        raise ResourceError("rlimit_install_failed",
                            f"probe exit {result.returncode}: {result.stderr.decode(errors='replace')[:200]}")
    try:
        event = json.loads(lines[0])
    except json.JSONDecodeError as error:
        raise ResourceError("limit_readiness_unproven", f"probe output {lines[0]!r}") from error
    if not isinstance(event, dict) or event.get("event") != LIMIT_READY_EVENT:
        raise ResourceError("limit_readiness_unproven", f"probe output {lines[0]!r}")
    return lines[0]


def hold_before_raise(deadline: float, threshold: int = ADMISSION_MEM_AVAILABLE_BYTES,
                      *, stage: Callable[[str, bool], None] | None = None) -> str:
    """Admission reading plus the limit-ready handshake; the raise stays held.

    The explicit `threshold` is threaded to admit() unchanged: sizing runs pass
    the sizing-only 3 GiB floor, ceiling/prerequisite runs the 4 GiB default.
    Required readings and limit readiness are established before any ceiling
    raise or the run fails closed. The optional `stage` sink is called with
    (phase, started) around the memory-read and admission boundaries so the
    caller can bracket them in the diagnostic stream. Triggers and gates only:
    no reservation and no guaranteed restoration.
    """
    mark = _no_stage if stage is None else stage
    mark("memory_read", True)
    reading = read_mem_available()
    mark("memory_read", False)
    mark("admission", True)
    admit(reading, threshold)
    mark("admission", False)
    return handshake(deadline)


def child_limit_ready() -> int:
    """Install the child-only ceiling in this process and return its readiness data.

    Used by bundle tails so the guard child proves its own limit before any
    watch setup. The supervisor-side handshake above does not replace this. The
    caller emits the LIMIT_READY_EVENT record itself through the bounded
    diagnostic-write discipline.
    """
    install_child_address_space_limit()
    return CHILD_ADDRESS_SPACE_LIMIT_BYTES
