"""Unprivileged in-memory ceiling supervisor: reversible window around a guard child.

The stdin entrypoint main() runs the window against the single exact setting and
an in-memory guard bundle (no disk). Cleanup is armed before the raise; ambiguous
writes are reconciled by readback; restore is conditional and verified. Acceptance
requires a strict well-formed sizing transcript or a parsed guard final event AND
restored-original AND no conflicts/errors/
signals AND no capped/unfinished collection. Required resource readings and the
child-limit ready handshake are established before the ceiling raise or the run
fails closed. Resources are sampled while collecting; readings are abort
triggers, never reservations. One overall supervisor deadline reserves cleanup
headroom before the child; all recovery and output collection are bounded. No
atomic sysctl CAS: the restore is best-effort readback reconciliation, never
overwriting an observed conflict; supervisor SIGKILL/power loss is best-effort
only, never a guarantee.

Every supervisor stage is streamed live through the bounded diagnostic-write
discipline (stage brackets, heartbeats, cleanup/egress transitions, terminal).
Child records forward only after collection: live localization is supervisor
level, child stages localize retrospectively via the collected stream. The
terminal result is delivered by the egress deadline (budget end minus
scheduling slack) so it provably completes before the worst-case GNU kill;
the authoritative allocation lives in deadline.py (SUPERVISOR_BUDGET = 940).
"""
from __future__ import annotations

import os
import signal
import sys
import time
from dataclasses import asdict, replace
from types import FrameType
from typing import Final

from .bootstrap import hold_before_raise
from .collector import CollectionPolicy
from .deadline import (CLEANUP_HEADROOM_SECONDS, DIAGNOSTIC_ALLOWANCE_SECONDS,
                       SCHEDULING_SLACK_SECONDS, SUPERVISOR_BUDGET_SECONDS)
from .egress import writer
from .guard import GuardChild, GuardResult, GuardStateError
from .report import (CEILING_MODE, SIZING_MODE, GuardEvent, SizingWire, SupervisorOutcome,
                     admission_for_mode, collection_wire, guard_evidence, resource_wire,
                     wire_outcome)
from .resource import ResourceError, Sampler
from .restoration import Restoration
from .setting import (CEILING_ORIGINAL, CEILING_TEMPORARY, SettingError, SettingExecutor,
                      SudoSetting, reconcile_raise)
from .sizing import COMPLETION_EVENT as SIZING_FINAL_EVENT

FINAL_GUARD_EVENT: Final = "protected_no_detected_write_no_persistent_change"
_SIGNALS: Final = (signal.SIGTERM, signal.SIGINT, signal.SIGHUP)


class Supervisor(Restoration):
    """Reversible ceiling window around one guard child; owns cleanup and the gate."""

    def __init__(
        self,
        setting: SettingExecutor,
        guard_argv: tuple[str, ...],
        *,
        guard_stdin: bytes = b"",
        final_event: str | None = None,
        mode: str = CEILING_MODE,
        budget_seconds: float = SUPERVISOR_BUDGET_SECONDS,
        cleanup_headroom_seconds: float = CLEANUP_HEADROOM_SECONDS,
    ) -> None:
        self.setting = setting
        self.guard_argv = guard_argv
        self.guard_stdin = guard_stdin
        self.mode = mode
        self.final_event = final_event or (SIZING_FINAL_EVENT if mode == SIZING_MODE else FINAL_GUARD_EVENT)
        self.budget_seconds = budget_seconds
        self.cleanup_headroom_seconds = cleanup_headroom_seconds
        self.guard: GuardChild | None = None
        self.guard_policy: CollectionPolicy = CollectionPolicy()
        self._sampler: Sampler | None = None
        self._deadline = 0.0
        self._cleaned = False
        self._conflicts: list[str] = []
        self._errors: list[str] = []
        self._raise_class = "not_raised"
        self._restore_class = "not_restored"
        self._original_observed: int | None = None
        self._temp_observed: int | None = None
        self._restore_observed: int | None = None
        self._allowances = {}
        self._guard_result: GuardResult | None = None
        self._guard_events: tuple[GuardEvent, ...] = ()
        self._sizing_wire: SizingWire | None = None
        self._restored_original = False
        self._signum: int | None = None
        self._signal_count = 0
        self._raised = False

    def _on_signal(self, signum: int, _frame: FrameType | None) -> None:
        self._signum = signum
        self._signal_count += 1
        guard = self.guard
        if guard is not None and guard.proc is not None and not guard.terminated:
            try:
                os.killpg(guard.proc.pid, signal.SIGKILL if self._signal_count > 1 else signal.SIGTERM)
            except ProcessLookupError:
                return
            except OSError as error:
                self._errors.append(f"guard_signal {error}")

    def _resource_gate(self) -> bool:
        """Required readings and limit readiness before the raise; fail closed."""
        try:
            self._emit("limit_readiness",
                       hold_before_raise(self._operation_deadline(), admission_for_mode(self.mode),
                                         stage=self._stage), phase=self._operation_deadline())
        except ResourceError as error:
            self._errors.append(f"resource {error}")
            return False
        return True

    def _window(self) -> tuple[GuardResult | None, bool]:
        operation = self._operation_deadline()
        self._stage("setting_read", True)
        try:
            self._original_observed = self.setting.read(self._bound(), deadline=operation)
            if self._original_observed != CEILING_ORIGINAL:
                raise SettingError("preflight_wrong_original", f"observed {self._original_observed}")
            self.setting.preflight(self._bound(), deadline=operation)
            self._bound()
        except SettingError as error:
            self._errors.append(f"preflight {error}")
            return None, False
        self._stage("setting_read", False)
        if self._signum is not None:
            self._errors.append(f"aborted_by_signal {self._signum}")
            return None, False
        if not self._resource_gate():
            return None, False
        self._emit("ceiling_raise", CEILING_ORIGINAL, phase=operation)
        try:
            bound = self._bound()
            self._raised = True
            self.setting.write(CEILING_TEMPORARY, bound, deadline=operation)
            observed = self.setting.read(self._bound(), deadline=operation)
        except SettingError as error:
            self._errors.append(f"raise {error}")
            return None, False
        self._temp_observed = observed
        raise_reconcile = reconcile_raise(observed)
        self._raise_class = raise_reconcile.action
        self._emit("ceiling_raised", observed, phase=operation)
        if raise_reconcile.action != "proceed":
            self._errors.append(f"raise_not_open {raise_reconcile.action}")
            return None, False
        if self._signum is not None:
            self._errors.append(f"aborted_by_signal {self._signum}")
            return None, False
        child_budget = (self._deadline - self.cleanup_headroom_seconds) - time.monotonic()
        if child_budget <= 0:
            self._errors.append(f"setup_budget_exhausted {self.budget_seconds}")
            return None, False
        guard = GuardChild(self.guard_argv, child_budget,
                           cleanup_deadline=self._deadline - min(25.0, self.cleanup_headroom_seconds * 5 / 8))
        guard.deadline = operation
        guard.policy = self.guard_policy
        # One heartbeat per interval in the collection waits; never extends a
        # deadline: the write is capped at the collection deadline
        # (until=guard.deadline) and at one resource-sample interval (bound), so
        # it can delay the 0.1 s sampling cadence by at most that interval and
        # can never run past the collection phase it reports on.
        guard.heartbeat = lambda progress: writer().heartbeat(
            {"phase": "collection", **asdict(progress)}, until=guard.deadline,
            bound=min(DIAGNOSTIC_ALLOWANCE_SECONDS, self.guard_policy.sample_interval))
        self.guard = guard
        self._stage("guard_init", True)
        guard.spawn(self.guard_stdin or None)
        self._stage("guard_init", False)
        self._sampler = Sampler(guard.proc.pid, child_poll=guard.proc.poll) if guard.proc is not None else Sampler(None)
        guard.sample = self._sampler.sample
        self._stage("collection", True)
        result = guard.wait()
        # Requal obs 2: the window-end TERM/KILL/reap runs inside guard.wait()
        # (this bracket's tail), so cleanup_* below brackets supervisor cleanup
        # only: idempotent terminate escalation + restoration + reconcile.
        restoration = self._operation_deadline(restoring=True)
        self._stage("collection", False, deadline=restoration)
        self._guard_result = result
        self._emit("guard_result", result.exit_status, phase=restoration)
        return result, False

    def _guard_evidence(self, result: GuardResult) -> bool:
        """Strict sizing transcript or legacy ceiling events; always forward."""
        evidence = guard_evidence(result, self.mode, self.final_event)
        self._guard_events, self._sizing_wire = evidence.events, evidence.sizing
        if evidence.error is not None:
            self._errors.append(evidence.error)
        for event, data in evidence.forward:
            self._emit(event, data)
        return evidence.valid

    def _outcome(self, guard_valid: bool) -> SupervisorOutcome:
        result = self._guard_result
        return SupervisorOutcome(
            accepted=(guard_valid and self._restored_original and not self._conflicts
                     and not self._errors and self._signum is None),
            guard_valid=guard_valid,
            restored_original=self._restored_original,
            raise_class=self._raise_class,
            restore_class=self._restore_class,
            conflicts=tuple(self._conflicts),
            errors=tuple(self._errors),
            signal=self._signum,
            original_observed=self._original_observed,
            temp_observed=self._temp_observed,
            restore_observed=self._restore_observed,
            guard_exit=result.exit_status if result else None,
            guard_events=self._guard_events,
            guard_stdout="" if self.mode == SIZING_MODE else (result.stdout if result else ""),
            guard_stderr="" if self.mode == SIZING_MODE else (result.stderr if result else ""),
            mode=self.mode,
            sizing=self._sizing_wire,
            resources=resource_wire(self._sampler) if self._sampler is not None else None,
            collection=collection_wire(result) if result is not None else None,
        )

    def run(self) -> SupervisorOutcome:
        self._deadline = time.monotonic() + self.budget_seconds
        egress_deadline = self._deadline - SCHEDULING_SLACK_SECONDS
        writer().rebind(egress_deadline)
        restoration = self._operation_deadline(restoring=True)
        previous = {sig: signal.signal(sig, self._on_signal) for sig in _SIGNALS}
        guard_valid = False
        try:
            try:
                self._window()
            except (GuardStateError, OSError) as error:
                self._errors.append(f"guard_lifecycle {error}")
        finally:
            try:
                self._transition("cleanup_started", restoration)
                self._cleanup()
                self._emit("ceiling_cleanup", self._restore_class, phase=egress_deadline)
            finally:
                self._transition("cleanup_completed", egress_deadline)
                for sig, handler in previous.items():
                    signal.signal(sig, handler)
        result = self._guard_result
        if result is not None and time.monotonic() < self._deadline:
            guard_valid = self._guard_evidence(result)
        if time.monotonic() >= self._deadline:
            self._errors.append("report_budget_exhausted")
        outcome = self._outcome(guard_valid)
        event, payload = wire_outcome(outcome)
        self._transition("egress_started", egress_deadline)
        if not writer().terminal(event, payload):
            return replace(outcome, accepted=False, errors=(*outcome.errors, "egress_failed_or_budget_exhausted"))
        return outcome


GUARD_BUNDLE: bytes = b""


def main(
    setting: SettingExecutor | None = None,
    guard_argv: tuple[str, ...] | None = None,
    guard_stdin: bytes | None = None,
    mode: str = CEILING_MODE,
) -> int:
    """In-memory stdin entrypoint. Production calls with no args (real deps).

    Test seams inject a fake setting/guard here; they cannot alter the production
    remote payload (fixed at bundle build) or the target authority (fixed setting).
    """
    resolved_setting = setting if setting is not None else SudoSetting()
    resolved_argv = guard_argv if guard_argv is not None else (sys.executable, "-B", "-")
    resolved_stdin = GUARD_BUNDLE if guard_stdin is None else guard_stdin
    outcome = Supervisor(resolved_setting, resolved_argv, guard_stdin=resolved_stdin, mode=mode).run()
    return 0 if outcome.accepted else 70
