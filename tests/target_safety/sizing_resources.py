# SPDX-License-Identifier: GPL-3.0-or-later
# /// script
# requires-python = ">=3.11"
# dependencies = []
# ///
# Run: python3 -B tests/target_safety/sizing_resources.py
"""Resource boundary qualification: real readings, real rlimit, typed failures.

Abort triggers and readiness gates only: no reservation and no restoration
guarantee is claimed or tested here. Setting authority stays a local fake.
"""
from __future__ import annotations

import io
import os
import sys
import tempfile
from contextlib import redirect_stdout
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
sys.path.insert(0, str(Path(__file__).resolve().parent))
from target_safety import bootstrap, resource
from target_safety.guard import GuardChild, GuardStateError
from target_safety.resource import ResourceError
from target_safety.supervisor import Supervisor
from ceiling_fixtures import MARKER, guard_argv
from ceiling_setting_fixtures import fake_setting

GIB = 1024 ** 3


def meminfo(bytes_available: int) -> Path:
    root = Path(tempfile.mkdtemp(prefix="resource-meminfo-", dir="/tmp/opencode"))
    path = root / "meminfo"
    path.write_text(f"MemTotal: 1 kB\nMemAvailable:  {bytes_available // 1024} kB\n")
    return path


def readings_have_exact_bounds_and_typed_failures() -> None:
    # Given: exact admission and abort boundaries from the design.
    assert resource.read_mem_available(meminfo(4 * GIB)) == 4 * GIB
    assert resource.read_mem_available(meminfo(4 * GIB - 1024)) == 4 * GIB - 1024
    resource.admit(resource.read_mem_available(meminfo(4 * GIB)))
    for reading in (4 * GIB - 1024, 2 * GIB, 0):
        try:
            resource.admit(reading)
        except ResourceError as error:
            assert error.operation == "admission_mem_available", error
        else:
            raise AssertionError(f"admission accepted {reading}")
    assert resource.abort_reason(2 * GIB, None, None) is None
    assert "mem_available" in (resource.abort_reason(2 * GIB - 1, None, None) or "")
    assert "guard_rss" in (resource.abort_reason(4 * GIB, 2 * GIB, None) or "")
    assert "supervisor_rss" in (resource.abort_reason(4 * GIB, 0, 256 * 2 ** 20) or "")
    # When: missing or malformed readings. Then: typed failures, never silent.
    for path, operation in ((Path("/tmp/opencode/absent-meminfo"), "meminfo_unavailable"),
                            (meminfo(4 * GIB).with_name("empty"), "meminfo_malformed")):
        if operation == "meminfo_malformed":
            path.write_text("MemTotal: 1 kB\n")
        try:
            resource.read_mem_available(path)
        except ResourceError as error:
            assert error.operation == operation, error
        else:
            raise AssertionError(f"bad reading accepted: {path}")
    assert resource.read_rss_bytes(os.getpid()) > 0
    try:
        resource.read_rss_bytes(999999999)
    except ResourceError as error:
        assert error.operation == "rss_unavailable", error
    else:
        raise AssertionError("missing rss accepted")
    print("PASS readings exact bounds/excess/missing/malformed are typed")


def failed_rlimit_and_real_allocation_failure() -> None:
    # Given: a failed limit install. Then: typed failure and a typed spawn failure.
    with patch("target_safety.resource._resource.setrlimit", side_effect=OSError("denied")):
        try:
            resource.install_child_address_space_limit()
        except ResourceError as error:
            assert error.operation == "rlimit_install_failed", error
        else:
            raise AssertionError("failed rlimit accepted")
        guard = GuardChild((sys.executable, "-B", "-c", "pass"), 5.0)
        try:
            guard.spawn()
        except GuardStateError as error:
            assert "spawn" in error.state
        else:
            raise AssertionError("child spawned without a working limit install")
    # When: a real child under the installed limit allocates beyond it.
    over = GuardChild((sys.executable, "-B", "-c", "bytearray(3 * 2**30)"), 20.0)
    over.spawn()
    result = over.wait()
    assert result.exit_status != 0 and "MemoryError" in result.stderr, result
    under = GuardChild((sys.executable, "-B", "-c", "x = bytearray(64 * 2**20); print(len(x))"), 20.0)
    under.spawn()
    small = under.wait()
    assert small.exit_status == 0 and "67108864" in small.stdout, small
    print("PASS failed rlimit typed; real allocation failure proves the child-only ceiling")


def handshake_probe_reports_readiness_and_failure() -> None:
    # Given: the real bounded probe under the existing deadline.
    import time
    event = bootstrap.handshake(time.monotonic() + 5.0)
    assert "child_limit_ready" in event
    try:
        bootstrap.handshake(time.monotonic() - 1.0)
    except ResourceError as error:
        assert error.operation == "limit_readiness_unproven", error
    else:
        raise AssertionError("handshake ran without budget")
    with patch("target_safety.bootstrap.subprocess.run",
               side_effect=lambda *a, **k: type("R", (), {"returncode": 1, "stdout": b"", "stderr": b"denied"})()):
        try:
            bootstrap.handshake(time.monotonic() + 5.0)
        except ResourceError as error:
            assert error.operation == "rlimit_install_failed", error
        else:
            raise AssertionError("failed probe accepted")
    print("PASS handshake holds the raise on readiness and typed probe failure")


def admission_failures_block_before_raise() -> None:
    for operation in ("admission_mem_available", "meminfo_unavailable"):
        with tempfile.TemporaryDirectory(dir="/tmp/opencode") as directory:
            root = Path(directory)
            setting, state = fake_setting(root)
            outcome = None
            with redirect_stdout(io.StringIO()):
                def fail(_path=None):
                    raise ResourceError(operation, "fixture")
                with patch.object(bootstrap, "read_mem_available", fail), patch.object(bootstrap, "handshake") as hs:
                    outcome = Supervisor(setting, guard_argv(root, "success")).run()
            assert outcome is not None and not outcome.accepted and outcome.guard_exit is None
            assert state.read_text() == "65536"
            hs.assert_not_called()
            assert any(f"resource {operation}" in e for e in outcome.errors), outcome.errors
    print("PASS admission/readiness failures fail closed before the raise and before the handshake")


def fake_setting_restores_under_the_exact_pair() -> None:
    # Given: the real gate (reading + handshake) with local fake authority only.
    with tempfile.TemporaryDirectory(dir="/tmp/opencode") as directory:
        root = Path(directory)
        setting, state = fake_setting(root)
        out = io.StringIO()
        with redirect_stdout(out):
            code = Supervisor(setting, guard_argv(root, "success"), guard_stdin=b"").run()
        assert code.accepted and code.guard_valid and code.restored_original, code
        assert code.original_observed == 65536 and code.temp_observed == 1048576
        assert code.restore_observed == 65536 and state.read_text() == "65536"
        assert MARKER in out.getvalue() and code.mode == "ceiling"
    print("PASS full window with real resource gate restores the exact pair under fake authority")


def main() -> int:
    readings_have_exact_bounds_and_typed_failures()
    failed_rlimit_and_real_allocation_failure()
    handshake_probe_reports_readiness_and_failure()
    admission_failures_block_before_raise()
    fake_setting_restores_under_the_exact_pair()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
