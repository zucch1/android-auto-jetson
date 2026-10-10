# SPDX-License-Identifier: GPL-3.0-or-later
# /// script
# requires-python = ">=3.11"
# dependencies = []
# ///
# Run: python3 -B tests/target_safety/ceiling_window.py
"""Supervisor orchestration fault cases: acceptance gate, restore, signals, output.

The guard child and supervisor signals are actual process-lifecycle; the setting
boundary is a synthetic in-process fake. No real kernel mutation, no sysctl CAS:
restore is best-effort readback reconciliation and never overwrites a conflict.
"""
from __future__ import annotations

import os
import signal
import sys
import tempfile
import threading
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
from target_safety.setting import CEILING_ORIGINAL, CEILING_TEMPORARY, SettingError
from target_safety.supervisor import Supervisor
from ceiling_fixtures import guard_argv


class FakeSetting:
    """Mutable in-process fake setting; mutation is its documented purpose."""

    def __init__(
        self,
        value: int,
        fail_raise_op: str | None = None,
        fail_restore_op: str | None = None,
        slow_raise: float = 0.0,
        slow_restore: float = 0.0,
        conflict_after_reads: int | None = None,
        corrupt_readback: bool = False,
    ) -> None:
        self.value = value
        self.fail_raise_op = fail_raise_op
        self.fail_restore_op = fail_restore_op
        self.slow_raise = slow_raise
        self.slow_restore = slow_restore
        self.conflict_after_reads = conflict_after_reads
        self.corrupt_readback = corrupt_readback
        self.reads = 0
        self.writes: list[int] = []

    def read(self, timeout: float | None = None, *, deadline: float | None = None) -> int:
        self.reads += 1
        if self.conflict_after_reads is not None and self.reads > self.conflict_after_reads:
            return 131072
        return self.value

    def write(self, value: int, timeout: float | None = None, *, deadline: float | None = None) -> None:
        if value == CEILING_TEMPORARY:
            if self.fail_raise_op is not None:
                raise SettingError(self.fail_raise_op, "fake refused raise")
            if self.slow_raise:
                time.sleep(self.slow_raise)
        if value == CEILING_ORIGINAL and self.fail_restore_op is not None:
            raise SettingError(self.fail_restore_op, "fake refused restore")
        if value == CEILING_ORIGINAL and self.slow_restore:
            time.sleep(self.slow_restore)
        self.writes.append(value)
        self.value = value
        if self.corrupt_readback and value == CEILING_ORIGINAL:
            self.value = 131072

    def preflight(self, timeout: float | None = None, *, deadline: float | None = None) -> None:
        return None


class BrokenPipe:
    def write(self, _s: str) -> int:
        raise BrokenPipeError(32, "broken")

    def flush(self) -> None:
        pass


def _tmp() -> Path:
    return Path(tempfile.mkdtemp(prefix="ceiling-window-", dir="/tmp/opencode"))


def _signal_later(seconds: float, sig: signal.Signals) -> None:
    def fire() -> None:
        time.sleep(seconds)
        os.kill(os.getpid(), sig)
    threading.Thread(target=fire, daemon=True).start()


def acceptance_gate_requires_both() -> None:
    """Given guard/restore outcomes, When gated, Then accept only when BOTH hold."""
    tmp = _tmp()
    good = FakeSetting(CEILING_ORIGINAL)
    ok = Supervisor(good, guard_argv(tmp, "success")).run()
    assert ok.accepted and ok.guard_valid and ok.restored_original
    assert good.value == CEILING_ORIGINAL
    # Guard passes but restore fails: the gate must reject (needs restored-original).
    unrestored = FakeSetting(CEILING_ORIGINAL, fail_restore_op="denied")
    bad_restore = Supervisor(unrestored, guard_argv(tmp, "success")).run()
    assert bad_restore.guard_valid and not bad_restore.restored_original
    assert not bad_restore.accepted
    # Restore succeeds but guard fails: the gate must reject (needs valid guard).
    bad_guard = Supervisor(FakeSetting(CEILING_ORIGINAL), guard_argv(tmp, "fail")).run()
    assert bad_guard.restored_original and not bad_guard.guard_valid
    assert not bad_guard.accepted
    print("PASS acceptance gate requires BOTH valid guard AND restored-original evidence")


def setup_failure_and_raise_conflict_block() -> None:
    """Given failed setup or a conflicting raise, When run, Then no guard and reject."""
    tmp = _tmp()
    wrong = FakeSetting(CEILING_TEMPORARY)
    out = Supervisor(wrong, guard_argv(tmp, "success")).run()
    assert not out.guard_valid and not out.accepted and out.guard_exit is None
    assert wrong.value == CEILING_TEMPORARY
    timeout = FakeSetting(CEILING_ORIGINAL, fail_raise_op="timeout")
    out2 = Supervisor(timeout, guard_argv(tmp, "success")).run()
    assert not out2.guard_valid and not out2.accepted and out2.guard_exit is None
    conflict = FakeSetting(CEILING_ORIGINAL, conflict_after_reads=1)
    out3 = Supervisor(conflict, guard_argv(tmp, "success")).run()
    assert out3.raise_class == "conflict_no_overwrite"
    assert not out3.guard_valid and not out3.accepted and out3.guard_exit is None
    print("PASS setup failure/timeout and raise conflict block the guard and reject")


def restore_faults_and_conflict_reject() -> None:
    """Given restore denied/timeout/mismatch/conflict, When cleaned up, Then reject."""
    tmp = _tmp()
    for op in ("denied", "timeout"):
        setting = FakeSetting(CEILING_ORIGINAL, fail_restore_op=op)
        out = Supervisor(setting, guard_argv(tmp, "success")).run()
        assert out.guard_valid and not out.restored_original and not out.accepted
        assert any("restore_write" in e for e in out.errors)
    mismatch = FakeSetting(CEILING_ORIGINAL, corrupt_readback=True)
    out2 = Supervisor(mismatch, guard_argv(tmp, "success")).run()
    assert out2.guard_valid and not out2.restored_original and not out2.accepted
    conflict = FakeSetting(CEILING_ORIGINAL, conflict_after_reads=2)
    out3 = Supervisor(conflict, guard_argv(tmp, "success")).run()
    assert out3.restore_class == "conflict_no_overwrite"
    assert not out3.restored_original and not out3.accepted
    assert any("restore_observed" in c for c in out3.conflicts)
    assert conflict.writes == [CEILING_TEMPORARY], "conflict must never be overwritten"
    print("PASS restore denied/timeout/readback-mismatch/conflict reject without overwrite")


def valid_event_observed_conflict_preserves_and_receipts() -> None:
    """Given a valid guard event and an observed restore conflict, Then reject and never overwrite."""
    tmp = _tmp()
    setting = FakeSetting(CEILING_ORIGINAL, conflict_after_reads=2)
    out = Supervisor(setting, guard_argv(tmp, "success")).run()
    assert out.guard_valid, "valid final event must be parsed, not a prose substring"
    assert out.conflicts and not out.restored_original and not out.accepted
    assert setting.writes == [CEILING_TEMPORARY], "observed conflict must never be overwritten"
    assert out.original_observed == CEILING_ORIGINAL
    assert out.temp_observed == CEILING_TEMPORARY
    assert out.restore_observed == 131072
    assert out.guard_exit == 0
    print("PASS valid event + observed conflict rejects, preserves conflict, receipts each observed value")


def failure_errors_return_original_but_reject() -> None:
    """Given guard failure/crash with restore to original, Then restored yet rejected."""
    tmp = _tmp()
    failed = FakeSetting(CEILING_ORIGINAL)
    out = Supervisor(failed, guard_argv(tmp, "fail")).run()
    assert not out.guard_valid and out.restored_original and not out.accepted
    assert failed.value == CEILING_ORIGINAL
    assert out.restore_observed == CEILING_ORIGINAL
    crashed = FakeSetting(CEILING_ORIGINAL, fail_raise_op="error")
    out2 = Supervisor(crashed, guard_argv(tmp, "success")).run()
    assert not out2.guard_valid and out2.restored_original and not out2.accepted
    assert any("raise" in e for e in out2.errors)
    assert crashed.value == CEILING_ORIGINAL
    print("PASS failure/errors return the setting to original yet reject acceptance")


def broken_stdout_never_skips_cleanup() -> None:
    """Given a broken stdout, When the window runs, Then cleanup still restores."""
    tmp = _tmp()
    setting = FakeSetting(CEILING_ORIGINAL)
    old = sys.stdout
    sys.stdout = BrokenPipe()
    try:
        out = Supervisor(setting, guard_argv(tmp, "success")).run()
    finally:
        sys.stdout = old
    assert out.restored_original and not out.accepted
    assert "egress_failed_or_budget_exhausted" in out.errors
    assert setting.value == CEILING_ORIGINAL
    print("PASS broken stdout never skips cleanup; restore still runs")


def supervisor_signals_cleanup_and_restore() -> None:
    """Given TERM during guard and during setup, When signaled, Then restore and reject."""
    tmp = _tmp()
    guard_setting = FakeSetting(CEILING_ORIGINAL)
    _signal_later(0.3, signal.SIGTERM)
    out = Supervisor(guard_setting, guard_argv(tmp, "hang")).run()
    assert not out.accepted and out.restored_original
    assert guard_setting.value == CEILING_ORIGINAL
    setup_setting = FakeSetting(CEILING_ORIGINAL, slow_raise=0.6)
    _signal_later(0.2, signal.SIGTERM)
    out2 = Supervisor(setup_setting, guard_argv(tmp, "success")).run()
    assert not out2.accepted and out2.restored_original
    assert out2.guard_exit is None
    assert setup_setting.value == CEILING_ORIGINAL
    print("PASS supervisor TERM during guard and during setup always restores and rejects")


def signal_after_guard_rejects_and_restores() -> None:
    """Given TERM after the guard completes (during restore), When signaled, Then reject but restore."""
    tmp = _tmp()
    setting = FakeSetting(CEILING_ORIGINAL, slow_restore=0.6)
    _signal_later(0.25, signal.SIGTERM)
    out = Supervisor(setting, guard_argv(tmp, "success")).run()
    assert out.guard_valid and out.restored_original, out
    assert not out.accepted and out.signal is not None
    assert setting.value == CEILING_ORIGINAL
    print(f"PASS signal after guard rejects acceptance yet still restores; fixture={tmp}")


def main() -> int:
    acceptance_gate_requires_both()
    setup_failure_and_raise_conflict_block()
    restore_faults_and_conflict_reject()
    valid_event_observed_conflict_preserves_and_receipts()
    failure_errors_return_original_but_reject()
    broken_stdout_never_skips_cleanup()
    supervisor_signals_cleanup_and_restore()
    signal_after_guard_rejects_and_restores()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
