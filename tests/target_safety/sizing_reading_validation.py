# SPDX-License-Identifier: GPL-3.0-or-later
# /// script
# requires-python = ">=3.11"
# dependencies = []
# ///
# Run: python3 -B tests/target_safety/sizing_reading_validation.py
"""Strict required-reading validation matrix: red/green lock for gate blocker 1.

Pure parser focus: a required reading must appear exactly once with the
canonical field label, exact kB unit, nonnegative ASCII decimal integer and the
expected three-field shape. Missing, malformed, ambiguous and duplicate
readings raise the existing typed failures and never select a value. Narrow
fixtures under unique tmp dirs only: no real /proc, sysctl, allocation or
sibling-module imports. Readings stay abort triggers and readiness gates only.
"""
from __future__ import annotations

import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path
from typing import Final, Literal, assert_never

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
from target_safety.resource import (
    ResourceError,
    abort_reason,
    admit,
    read_mem_available,
    read_rss_bytes,
)

GIB: Final = 1024 ** 3


@dataclass(frozen=True, slots=True)
class Rejection:
    """One required-reading fixture the parser must reject with a typed failure."""

    name: str
    kind: Literal["meminfo", "status"]
    content: str
    operation: str


# Permanent counter-fixture matrix for the gate-accepted failure classes plus
# the no-first-valid-duplicate locks (wrong unit, missing unit, negative, extra
# field, duplicates, unparseable labels, non-ASCII digits, missing reading).
REJECTIONS: Final = (
    Rejection("wrong_unit_mb", "meminfo",
              "MemTotal: 1 kB\nMemAvailable: 4194304 MB\n", "meminfo_malformed"),
    Rejection("missing_unit", "meminfo",
              "MemAvailable: 4194304\n", "meminfo_malformed"),
    Rejection("extra_field", "meminfo",
              "MemAvailable: 123 kB extra\n", "meminfo_malformed"),
    Rejection("duplicate_selects_first_valid", "meminfo",
              "MemAvailable: 5 kB\nMemAvailable: 7 kB\n", "meminfo_malformed"),
    Rejection("duplicate_ignores_malformed_tail", "meminfo",
              "MemAvailable: 5 kB\nMemAvailable: junk\n", "meminfo_malformed"),
    Rejection("duplicate_valid_after_malformed", "meminfo",
              "MemAvailable: junk\nMemAvailable: 7 kB\n", "meminfo_malformed"),
    Rejection("unparseable_label_gap", "meminfo",
              "MemAvailable : 5 kB\n", "meminfo_malformed"),
    Rejection("unparseable_label_typo", "meminfo",
              "MemAvailabl: 5 kB\n", "meminfo_malformed"),
    Rejection("glued_label_value", "meminfo",
              "MemAvailable:5 5 kB\n", "meminfo_malformed"),
    Rejection("missing_reading", "meminfo",
              "MemTotal: 1 kB\n", "meminfo_malformed"),
    Rejection("negative_value", "status",
              "Name:\tfixture\nVmRSS:\t-1 kB\n", "rss_malformed"),
    Rejection("non_ascii_digits", "status",
              "VmRSS:\t\u0661\u0662\u0663 kB\n", "rss_malformed"),
    Rejection("missing_unit_rss", "status",
              "VmRSS:\t4194304\n", "rss_malformed"),
    Rejection("missing_reading_rss", "status",
              "Name:\tfixture\nVmHWM:\t999 kB\n", "rss_malformed"),
)


def meminfo_fixture(content: str) -> Path:
    root = Path(tempfile.mkdtemp(prefix="sizing-reading-meminfo-", dir="/tmp/opencode"))
    path = root / "meminfo"
    path.write_text(content)
    return path


def status_fixture(content: str) -> Path:
    root = Path(tempfile.mkdtemp(prefix="sizing-reading-status-", dir="/tmp/opencode"))
    (root / "7").mkdir()
    (root / "7" / "status").write_text(content)
    return root


def probe(row: Rejection) -> int:
    match row.kind:
        case "meminfo":
            return read_mem_available(meminfo_fixture(row.content))
        case "status":
            return read_rss_bytes(7, status_fixture(row.content))
        case unreachable:
            assert_never(unreachable)


def malformed_matrix_rejects_required_readings() -> None:
    # Given: one narrow fixture per gate-accepted failure class.
    accepted: list[str] = []
    for row in REJECTIONS:
        # When: the strict parser reads that fixture.
        try:
            value = probe(row)
        except ResourceError as error:
            # Then: exactly the existing typed failure, never a value.
            assert error.operation == row.operation, (row.name, error)
        else:
            accepted.append(f"{row.name}=>{value}")
    # Then: no malformed reading was accepted and no valid duplicate selected.
    assert not accepted, f"malformed readings accepted: {accepted}"
    print(f"PASS matrix of {len(REJECTIONS)} malformed readings fails closed typed")


def valid_proc_format_and_zero_readings_parse_exactly() -> None:
    # Given: real /proc space and tab layouts and exact zero readings.
    assert read_mem_available(meminfo_fixture(
        "MemTotal:       16384 kB\nMemAvailable:    4194304 kB\n")) == 4 * GIB
    assert read_rss_bytes(7, status_fixture(
        "Name:\tguard\nVmHWM:\t  999 kB\nVmRSS:\t123 kB\nRssAnon:\t  100 kB\n")) == 123 * 1024
    assert read_mem_available(meminfo_fixture("MemAvailable: 0 kB\n")) == 0
    assert read_rss_bytes(7, status_fixture("VmRSS:\t0 kB\n")) == 0
    print("PASS real /proc tab/space format and exact zero readings parse exactly")


def unreadable_sources_fail_typed_unavailable() -> None:
    # Given: absent sources. Then: the existing *_unavailable operations only.
    for path, operation in ((Path("/tmp/opencode/absent-sizing-reading-meminfo"), "meminfo_unavailable"),):
        try:
            read_mem_available(path)
        except ResourceError as error:
            assert error.operation == operation, error
        else:
            raise AssertionError(f"absent source accepted: {path}")
    root = Path(tempfile.mkdtemp(prefix="sizing-reading-absent-", dir="/tmp/opencode"))
    try:
        read_rss_bytes(7, root)
    except ResourceError as error:
        assert error.operation == "rss_unavailable", error
    else:
        raise AssertionError("absent status accepted")
    print("PASS absent required sources fail closed as typed unavailability")


def threshold_controls_remain_exact() -> None:
    # Given: exact admission and abort boundaries from the design.
    admit(4 * GIB)
    for below in (4 * GIB - 1024, 2 * GIB, 0):
        try:
            admit(below)
        except ResourceError as error:
            assert error.operation == "admission_mem_available", error
        else:
            raise AssertionError(f"admission accepted {below}")
    assert abort_reason(2 * GIB, 1 * GIB, 128 * 2 ** 20) is None
    assert "mem_available" in (abort_reason(2 * GIB - 1, 0, 0) or "")
    assert "guard_rss" in (abort_reason(4 * GIB, 1 * GIB + 1024, 0) or "")
    assert "supervisor_rss" in (abort_reason(4 * GIB, 0, 128 * 2 ** 20 + 1024) or "")
    assert "mem_available" in (abort_reason(0, 0, 0) or "")
    print("PASS admission/abort thresholds and exact zero controls remain")


def main() -> int:
    malformed_matrix_rejects_required_readings()
    valid_proc_format_and_zero_readings_parse_exactly()
    unreadable_sources_fail_typed_unavailable()
    threshold_controls_remain_exact()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
