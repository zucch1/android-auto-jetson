# SPDX-License-Identifier: GPL-3.0-or-later
"""Conditional restoration independent of guard-stop failure; shared absolute budget.

Also the home of the phase-aware diagnostic deadline algebra: every synchronous
diagnostic write is bounded by the next safety deadline and by its safety
phase's SHARED aggregate diagnostic allowance
(deadline.DIAGNOSTIC_ALLOWANCE_SECONDS per phase), never by the whole remaining
stream window. The allowance is cumulative per phase: each write accounts its
elapsed wall time against the same per-phase budget, so the writes between two
safety deadlines (collection-completed, guard_result and cleanup_started before
restoration) can never collectively consume more than the allowance - a fresh
per-write allowance would let each of them take the full second.
"""
from __future__ import annotations

import time
from collections.abc import Callable
from typing import assert_never

from .deadline import DIAGNOSTIC_ALLOWANCE_SECONDS
from .egress import writer
from .guard import GuardChild, GuardStateError
from .setting import CEILING_ORIGINAL, SettingError, SettingExecutor, reconcile_restore


class _PhaseAllowance:
    """Cumulative synchronous diagnostic-write budget for one safety phase.

    Mutable per-phase accounting state: draws return the remaining budget as a
    write deadline and every completed write subtracts its elapsed wall time,
    making cumulative consumption explicit instead of a per-write allowance.
    """

    def __init__(self) -> None:
        self.remaining = DIAGNOSTIC_ALLOWANCE_SECONDS

    def until(self, phase_deadline: float, now: float) -> float:
        return min(phase_deadline, now + self.remaining)

    def account(self, elapsed: float) -> None:
        self.remaining = max(0.0, self.remaining - elapsed)


class Restoration:
    """Mutable cleanup state owned and initialized by the Supervisor lifecycle."""

    setting: SettingExecutor
    guard: GuardChild | None
    _deadline: float
    cleanup_headroom_seconds: float
    _allowances: dict[float, _PhaseAllowance]
    _cleaned: bool
    _errors: list[str]
    _conflicts: list[str]
    _raised: bool
    _restore_class: str
    _restore_observed: int | None
    _restored_original: bool

    def _operation_deadline(self, *, restoring: bool = False) -> float:
        reserve = min(10.0, self.cleanup_headroom_seconds / 4) if restoring else self.cleanup_headroom_seconds
        return self._deadline - reserve

    def _allowance(self, phase_deadline: float) -> _PhaseAllowance:
        return self._allowances.setdefault(phase_deadline, _PhaseAllowance())

    def _diagnostic(self, phase_deadline: float, write: Callable[[float], bool]) -> bool:
        """One synchronous diagnostic write under its phase's shared allowance.

        The write deadline is the phase deadline capped by the allowance
        REMAINING for that phase; the write's elapsed wall time is then
        accounted, so cumulative consumption is explicit and the next write of
        the same phase only gets what the budget still holds.
        """
        allowance = self._allowance(phase_deadline)
        started = time.monotonic()
        outcome = write(allowance.until(phase_deadline, started))
        allowance.account(time.monotonic() - started)
        return outcome

    def _stage(self, phase: str, started: bool, *, deadline: float | None = None) -> None:
        """One stage bracket under the enclosing phase's shared allowance."""
        phase_deadline = self._operation_deadline() if deadline is None else deadline
        stream = writer()
        bracket = stream.stage_started if started else stream.stage_completed
        self._diagnostic(phase_deadline, lambda until: bracket(phase, until=until))

    def _transition(self, event: str, phase_deadline: float) -> None:
        """One lifecycle transition under its phase's shared allowance."""
        self._diagnostic(phase_deadline, lambda until: writer().transition(event, until=until))

    def _emit(self, event: str, data: str | int | None, *,
              phase: float | None = None) -> None:
        """One diagnostic record; synchronous writes draw their phase's allowance."""
        if phase is None:
            writer().record(event, data)
        else:
            self._diagnostic(phase, lambda until: writer().record(event, data, until=until))

    def _bound(self, *, restoring: bool = False) -> float:
        remaining = self._operation_deadline(restoring=restoring) - time.monotonic()
        if remaining <= 0:
            raise SettingError("setup_budget_exhausted" if not restoring else "restore_budget_exhausted",
                               "absolute supervisor deadline allowance exhausted")
        return min(5.0, remaining)

    def _restore(self) -> None:
        try:
            observed = self.setting.read(self._bound(restoring=True), deadline=self._operation_deadline(restoring=True))
        except SettingError as error:
            self._errors.append(f"restore_read {error}")
            self._restore_class = "read_error"
            return
        self._restore_observed = observed
        reconcile = reconcile_restore(observed)
        self._restore_class = reconcile.action
        match reconcile.action:
            case "restore_original":
                if self._raised and reconcile.write_value is not None:
                    try:
                        self.setting.write(reconcile.write_value, self._bound(restoring=True),
                                           deadline=self._operation_deadline(restoring=True))
                    except SettingError as error:
                        self._errors.append(f"restore_write {error}")
            case "no_write_original_already":
                pass
            case "conflict_no_overwrite":
                self._conflicts.append(f"restore_observed {observed}")
            case unreachable:
                assert_never(unreachable)
        try:
            readback = self.setting.read(self._bound(restoring=True), deadline=self._operation_deadline(restoring=True))
        except SettingError as error:
            self._errors.append(f"restore_verify {error}")
            self._restored_original = False
            return
        self._restore_observed = readback
        self._restored_original = readback == CEILING_ORIGINAL

    def _cleanup(self) -> None:
        if self._cleaned:
            return
        self._cleaned = True
        try:
            if self.guard is not None:
                self.guard.terminate()
        except (GuardStateError, OSError) as error:
            self._errors.append(f"guard_termination {error}")
        finally:
            self._restore()
