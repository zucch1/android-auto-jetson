"""Fail-closed resource readings, child address-space limit and abort triggers.

Readiness readings gate the ceiling raise; runtime readings are abort triggers,
never reservations and never a guarantee of restoration. Unavailable or
malformed readings and limit-install failures are typed failures that fail
closed. A required reading must appear exactly once with the canonical field
label, exact kB unit and a nonnegative ASCII decimal integer; missing,
malformed, ambiguous and duplicate readings never select a value. The RLIMIT_AS
limit is child-only: the supervisor is never limited.
"""
from __future__ import annotations

import os
import resource as _resource
from collections.abc import Callable
from dataclasses import dataclass
from pathlib import Path
from typing import Final

MEMINFO: Final = Path("/proc/meminfo")
PROC_STATUS: Final = Path("/proc")
KILOBYTE: Final = 1024
# Fixed policy from the topology-sizing design (task-5-topology-sizing-design.json).
ADMISSION_MEM_AVAILABLE_BYTES: Final = 4294967296
# Topology-sizing-only admission floor (3 GiB), authorized by
# task-5-topology-sizing-3gib-policy-approval.json. A separate constant from the
# ceiling/prerequisite 4 GiB floor above: the shared ceiling value is never
# relabelled for sizing and the sizing value never gates a ceiling run.
SIZING_ADMISSION_MEM_AVAILABLE_BYTES: Final = 3221225472
ABORT_MEM_AVAILABLE_BELOW_BYTES: Final = 2147483648
ABORT_GUARD_RSS_ABOVE_BYTES: Final = 1073741824
ABORT_SUPERVISOR_RSS_ABOVE_BYTES: Final = 134217728
CHILD_ADDRESS_SPACE_LIMIT_BYTES: Final = 2147483648
# Enforced supervisor sampling cadence (collector SAMPLE_INTERVAL_SECONDS).
# Reconciliation (hardening correction round 3): a collection heartbeat write is
# capped at one sample interval (supervisor heartbeat bound) and at the
# collection deadline, so at most one sample is delayed and by at most one
# interval - the worst-case observed gap is 2x this value, never the 1.0 s
# diagnostic allowance. Triggers and gates only: cadence is policy, not a
# guarantee against scheduling or reading cost.
SUPERVISOR_SAMPLE_INTERVAL_MAX_SECONDS: Final = 0.1
GUARD_RESOURCE_CHECK_MAX_TOPOLOGY_ENTRIES: Final = 64
_ACKNOWLEDGEMENT: Final = (
    "Readings are abort triggers and readiness gates only: no memory is "
    "reserved, no fit is proven and restoration stays best-effort."
)
# Receipt-only mirror of the enforced resource policy; envelope tests cross-check
# both sides so drift cannot pass qualification. Triggers and gates only: no
# reservation, no RAM-fit claim and no guaranteed restoration.
RESOURCE_POLICY: Final = {
    "admission_mem_available_bytes": ADMISSION_MEM_AVAILABLE_BYTES,
    "abort_mem_available_below_bytes": ABORT_MEM_AVAILABLE_BELOW_BYTES,
    "abort_guard_rss_above_bytes": ABORT_GUARD_RSS_ABOVE_BYTES,
    "abort_supervisor_rss_above_bytes": ABORT_SUPERVISOR_RSS_ABOVE_BYTES,
    "child_only_rlimit_as_bytes": CHILD_ADDRESS_SPACE_LIMIT_BYTES,
    "supervisor_sample_interval_max_seconds": SUPERVISOR_SAMPLE_INTERVAL_MAX_SECONDS,
    "guard_resource_check_max_topology_entries": GUARD_RESOURCE_CHECK_MAX_TOPOLOGY_ENTRIES,
    "acknowledgement": _ACKNOWLEDGEMENT,
}
# Receipt-only mirror for topology-sizing runs. The 3 GiB sizing admission and
# the 4 GiB ceiling admission are both named so a receipt reader can tell them
# apart; every other safeguard is byte-identical to RESOURCE_POLICY. Triggers
# and gates only: no reservation, no RAM-fit claim and no guaranteed restoration.
SIZING_RESOURCE_POLICY: Final = {
    "admission_mem_available_bytes": SIZING_ADMISSION_MEM_AVAILABLE_BYTES,
    "admission_scope": "topology-sizing-only",
    "ceiling_admission_mem_available_bytes": ADMISSION_MEM_AVAILABLE_BYTES,
    "abort_mem_available_below_bytes": ABORT_MEM_AVAILABLE_BELOW_BYTES,
    "abort_guard_rss_above_bytes": ABORT_GUARD_RSS_ABOVE_BYTES,
    "abort_supervisor_rss_above_bytes": ABORT_SUPERVISOR_RSS_ABOVE_BYTES,
    "child_only_rlimit_as_bytes": CHILD_ADDRESS_SPACE_LIMIT_BYTES,
    "supervisor_sample_interval_max_seconds": SUPERVISOR_SAMPLE_INTERVAL_MAX_SECONDS,
    "guard_resource_check_max_topology_entries": GUARD_RESOURCE_CHECK_MAX_TOPOLOGY_ENTRIES,
    "acknowledgement": _ACKNOWLEDGEMENT,
}


class ResourceError(RuntimeError):
    """Typed resource-boundary failure owned by the reading/limit/install edge."""

    def __init__(self, operation: str, detail: str) -> None:
        self.operation = operation
        self.detail = detail
        super().__init__(operation, detail)

    def __str__(self) -> str:
        return f"{self.operation}: {self.detail}"


@dataclass(frozen=True, slots=True)
class Extrema:
    """Observed resource extrema for compact sizing receipts."""

    min_mem_available_bytes: int | None
    max_guard_rss_bytes: int | None
    max_supervisor_rss_bytes: int | None


@dataclass(frozen=True, slots=True)
class _ReadingSpec:
    """Canonical required-reading label and its typed failure operations."""

    label: str
    unavailable_operation: str
    malformed_operation: str


_MEMINFO_READING: Final = _ReadingSpec("MemAvailable:", "meminfo_unavailable", "meminfo_malformed")
_RSS_READING: Final = _ReadingSpec("VmRSS:", "rss_unavailable", "rss_malformed")


def _read_required_kilobytes(path: Path, spec: _ReadingSpec) -> int:
    """Parse exactly one canonical kB reading; every other shape is typed.

    Exactly one line must carry the canonical label in the expected three-field
    shape with the exact kB unit and a nonnegative ASCII decimal integer value.
    Missing, malformed, ambiguous and duplicate readings raise the spec's typed
    operations and never select a first valid value.
    """
    try:
        raw = path.read_text()
    except OSError as error:
        raise ResourceError(spec.unavailable_operation, f"{path}: {error}") from error
    readings = [line for line in raw.splitlines() if line.startswith(spec.label)]
    if len(readings) != 1:
        raise ResourceError(spec.malformed_operation, f"{path}: {spec.label} count {len(readings)}")
    fields = readings[0].split()
    if len(fields) != 3 or fields[0] != spec.label or fields[2] != "kB":
        raise ResourceError(spec.malformed_operation, f"{path}: {readings[0]!r}")
    value = fields[1]
    if not (value.isascii() and value.isdigit()):
        raise ResourceError(spec.malformed_operation, f"{path}: {readings[0]!r}")
    return int(value) * KILOBYTE


def read_mem_available(path: Path = MEMINFO) -> int:
    """Parse the single MemAvailable reading into bytes; bad shapes are typed."""
    return _read_required_kilobytes(path, _MEMINFO_READING)


def read_rss_bytes(pid: int, proc: Path = PROC_STATUS) -> int:
    """Parse the single VmRSS reading into bytes; bad shapes are typed."""
    return _read_required_kilobytes(proc / str(pid) / "status", _RSS_READING)


def install_child_address_space_limit(limit_bytes: int = CHILD_ADDRESS_SPACE_LIMIT_BYTES) -> None:
    """Install the child-only RLIMIT_AS ceiling; install failure is typed."""
    try:
        _resource.setrlimit(_resource.RLIMIT_AS, (limit_bytes, limit_bytes))
    except (OSError, ValueError) as error:
        raise ResourceError("rlimit_install_failed", f"RLIMIT_AS={limit_bytes}: {error}") from error


def child_limit_readiness(limit_bytes: int = CHILD_ADDRESS_SPACE_LIMIT_BYTES) -> None:
    """Prove the address-space ceiling is installable before any ceiling raise."""
    try:
        _soft, hard = _resource.getrlimit(_resource.RLIMIT_AS)
    except (OSError, ValueError) as error:
        raise ResourceError("limit_readiness_unproven", f"RLIMIT_AS: {error}") from error
    unlimited = _resource.RLIM_INFINITY
    if hard != unlimited and hard < limit_bytes:
        raise ResourceError("limit_readiness_unproven", f"hard {hard} < {limit_bytes}")


def admit(mem_available: int, threshold: int = ADMISSION_MEM_AVAILABLE_BYTES) -> None:
    """Admission gate before the raise; below the given floor fails closed.

    The threshold is threaded explicitly by every mode-aware caller; the
    default stays the ceiling/prerequisite 4 GiB floor so sizing-only 3 GiB
    admission never silently replaces it.
    """
    if mem_available < threshold:
        raise ResourceError("admission_mem_available",
                            f"{mem_available} < {threshold}")


def abort_reason(mem_available: int, guard_rss: int | None,
                 supervisor_rss: int | None) -> str | None:
    """First abort trigger crossed, if any (triggers, not reservations)."""
    if mem_available < ABORT_MEM_AVAILABLE_BELOW_BYTES:
        return f"mem_available {mem_available} < {ABORT_MEM_AVAILABLE_BELOW_BYTES}"
    if guard_rss is not None and guard_rss > ABORT_GUARD_RSS_ABOVE_BYTES:
        return f"guard_rss {guard_rss} > {ABORT_GUARD_RSS_ABOVE_BYTES}"
    if supervisor_rss is not None and supervisor_rss > ABORT_SUPERVISOR_RSS_ABOVE_BYTES:
        return f"supervisor_rss {supervisor_rss} > {ABORT_SUPERVISOR_RSS_ABOVE_BYTES}"
    return None


class Sampler:
    """Mutable supervisor-side sampling state; mutation is its documented purpose.

    The optional child_poll is the child Popen's own poll: it is the only
    liveness authority for child RSS readings (never a missing proc or a
    state inference). Fixture-pid callers without one keep unconditional
    child readings and fail closed exactly as before.
    """

    def __init__(self, child_pid: int | None, *,
                 meminfo: Path = MEMINFO, proc: Path = PROC_STATUS,
                 child_poll: Callable[[], int | None] | None = None) -> None:
        self.child_pid = child_pid
        self.meminfo = meminfo
        self.proc = proc
        self.child_poll = child_poll
        self.min_mem: int | None = None
        self.max_guard_rss: int | None = None
        self.max_supervisor_rss: int | None = None
        self.samples = 0

    def sample(self) -> str | None:
        """Take one reading pair; returns the first abort trigger, if any."""
        self.samples += 1
        mem = read_mem_available(self.meminfo)
        self.min_mem = mem if self.min_mem is None else min(self.min_mem, mem)
        guard_rss = self._guard_rss()
        supervisor_rss = read_rss_bytes(os.getpid(), self.proc)
        self.max_supervisor_rss = (supervisor_rss if self.max_supervisor_rss is None
                                   else max(self.max_supervisor_rss, supervisor_rss))
        return abort_reason(mem, guard_rss, supervisor_rss)

    def _guard_rss(self) -> int | None:
        """Child RSS only while the child poll confirms the child runs.

        A poll-confirmed exit skips the reading entirely, so an exited or
        reaped child never needs one. A child RSS boundary failure is
        rechecked against the poll: an exit between the confirmation and the
        reading never fakes a fault, while a confirmed-running child's
        missing or malformed reading still fails closed. The observed
        maximum is kept, never fabricated and never reset to zero.
        """
        if self.child_pid is None:
            return None
        if self.child_poll is not None and self.child_poll() is not None:
            return None
        try:
            rss = read_rss_bytes(self.child_pid, self.proc)
        except ResourceError:
            if self.child_poll is not None and self.child_poll() is not None:
                return None
            raise
        self.max_guard_rss = rss if self.max_guard_rss is None else max(self.max_guard_rss, rss)
        return rss

    def extrema(self) -> Extrema:
        return Extrema(self.min_mem, self.max_guard_rss, self.max_supervisor_rss)


def checkpoint(meminfo: Path = MEMINFO) -> None:
    """Guard-side cadence reading; typed failures and abort triggers both stop.

    The guard's own RSS is read once and judged against the guard ceiling only;
    the supervisor's RSS is a different process and is the supervisor sampler's
    reading (128 MiB supervisor / 1 GiB guard stay on separate pids).
    """
    reason = abort_reason(read_mem_available(meminfo), read_rss_bytes(os.getpid()), None)
    if reason is not None:
        raise ResourceError("guard_resource_abort", reason)
