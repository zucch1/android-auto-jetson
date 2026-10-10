# SPDX-License-Identifier: GPL-3.0-or-later
"""Reversible inotify ceiling setting boundary with reconciliation.

No atomic sysctl CAS exists. The boundary reads/writes only the exact approved
pair and reconciles ambiguous writes by readback. Concurrent-writer races are
detected when observed and then block; no arbitrary-concurrent-writer guarantee
is claimed. All writes are bounded and restricted to the single exact setting.
Read authorization is not write authorization: both write directions must be
established by a non-mutating sudo policy query before the window opens, else
fail-closed. If that probe cannot be established, record a blocker, never weaken.
"""
from __future__ import annotations

import os
import subprocess
import time
from dataclasses import dataclass
from typing import Final, Literal, Protocol, assert_never

from .policy import require_passwordless
from .setting_process import SETTING_TIMEOUT_SECONDS, SettingError, execute

CEILING_SETTING: Final = "fs.inotify.max_user_watches"
CEILING_ORIGINAL: Final = 65536
CEILING_TEMPORARY: Final = 1048576
SUDO: Final[tuple[str, ...]] = ("sudo", "-n")
SYSCTL: Final = "/usr/sbin/sysctl"

ValueClass = Literal["original", "temporary", "conflict"]
ReconcileAction = Literal[
    "proceed",
    "no_write_raise_did_not_take",
    "restore_original",
    "no_write_original_already",
    "conflict_no_overwrite",
]


@dataclass(frozen=True, slots=True)
class Reconcile:
    """Disposition of one observed value in the reversible window."""

    value_class: ValueClass
    action: ReconcileAction
    write_value: int | None


class SettingExecutor(Protocol):
    """Boundary for the single reversible kernel setting; faked in local TDD."""

    def read(self, timeout: float | None = None, *, deadline: float | None = None) -> int: ...

    def write(self, value: int, timeout: float | None = None, *, deadline: float | None = None) -> None: ...

    def preflight(self, timeout: float | None = None, *, deadline: float | None = None) -> None: ...


def classify(observed: int) -> ValueClass:
    """Partition an observed value against the approved reversible pair."""
    if observed == CEILING_ORIGINAL:
        return "original"
    if observed == CEILING_TEMPORARY:
        return "temporary"
    return "conflict"


def reconcile_raise(observed: int) -> Reconcile:
    """Classify the value after the raise write; proceed only when temporary is observed."""
    value_class = classify(observed)
    match value_class:
        case "temporary":
            return Reconcile(value_class, "proceed", None)
        case "original":
            return Reconcile(value_class, "no_write_raise_did_not_take", None)
        case "conflict":
            return Reconcile(value_class, "conflict_no_overwrite", None)
        case unreachable:
            assert_never(unreachable)


def reconcile_restore(observed: int) -> Reconcile:
    """Restore original only when the temporary value is observed; never overwrite a conflict."""
    value_class = classify(observed)
    match value_class:
        case "temporary":
            return Reconcile(value_class, "restore_original", CEILING_ORIGINAL)
        case "original":
            return Reconcile(value_class, "no_write_original_already", None)
        case "conflict":
            return Reconcile(value_class, "conflict_no_overwrite", None)
        case unreachable:
            assert_never(unreachable)


def preflight(executor: SettingExecutor, timeout: float | None = None) -> int:
    """Non-mutating gate: read auth (original) plus write auth for both directions.

    The executor's preflight establishes write authorization (via a non-mutating
    sudo policy query). It raises if either write direction is unestablished, so
    the window never opens without proven restore authority.
    """
    deadline = time.monotonic() + min(SETTING_TIMEOUT_SECONDS, SETTING_TIMEOUT_SECONDS if timeout is None else timeout)
    current = executor.read(min(SETTING_TIMEOUT_SECONDS, max(0.0, deadline - time.monotonic())), deadline=deadline)
    if classify(current) != "original":
        raise SettingError("preflight_wrong_original", f"observed {current}")
    executor.preflight(min(SETTING_TIMEOUT_SECONDS, max(0.0, deadline - time.monotonic())), deadline=deadline)
    return current


@dataclass(frozen=True, slots=True)
class SudoSetting:
    """Bounded sudo -n sysctl operations restricted to the exact ceiling setting."""

    sudo: tuple[str, ...] = SUDO
    sysctl: str = SYSCTL
    timeout_seconds: float = SETTING_TIMEOUT_SECONDS

    def _run(self, args: tuple[str, ...], timeout: float | None = None,
             *, deadline: float | None = None) -> subprocess.CompletedProcess[str]:
        bound = min(self.timeout_seconds, SETTING_TIMEOUT_SECONDS,
                    self.timeout_seconds if timeout is None else timeout)
        operation_deadline = time.monotonic() + bound
        return execute(args,
                       operation_deadline if deadline is None else min(operation_deadline, deadline))

    def _run_ok(self, args: tuple[str, ...], timeout: float | None = None,
                *, deadline: float | None = None) -> str:
        result = self._run(args, timeout, deadline=deadline)
        if result.returncode != 0:
            raise SettingError("nonzero_exit", f"{args} -> {result.returncode}: {result.stderr.strip()}")
        return result.stdout

    def read(self, timeout: float | None = None, *, deadline: float | None = None) -> int:
        raw = self._run_ok((self.sysctl, "-n", CEILING_SETTING), timeout, deadline=deadline)
        try:
            return int(raw.strip())
        except ValueError as error:
            raise SettingError("parse", f"{(self.sysctl, '-n', CEILING_SETTING)}: non-integer read {raw!r}") from error

    def write(self, value: int, timeout: float | None = None, *, deadline: float | None = None) -> None:
        if value != CEILING_ORIGINAL and value != CEILING_TEMPORARY:
            raise SettingError("refused_write", f"value {value} not the exact reversible pair")
        self._run_ok((*self.sudo, "-u", "root", "--", self.sysctl, "-w", f"{CEILING_SETTING}={value}"), timeout, deadline=deadline)

    def preflight(self, timeout: float | None = None, *, deadline: float | None = None) -> None:
        """Permission probes plus strict passwordless policy, all inside five seconds."""
        if not os.path.isabs(self.sysctl) or os.path.realpath(self.sysctl) != self.sysctl:
            raise SettingError("preflight_executable", "exact canonical absolute sysctl executable required")
        bound = min(SETTING_TIMEOUT_SECONDS, self.timeout_seconds,
                    self.timeout_seconds if timeout is None else timeout)
        operation_deadline = time.monotonic() + bound
        policy_deadline = operation_deadline if deadline is None else min(operation_deadline, deadline)
        for value in (CEILING_ORIGINAL, CEILING_TEMPORARY):
            self._probe_write(value, deadline=policy_deadline)
        argv = (*self.sudo, "-l", "-l")
        raw = self._run_ok(argv, deadline=policy_deadline)
        try:
            require_passwordless(raw, (f"{self.sysctl} -w {CEILING_SETTING}={CEILING_ORIGINAL}",
                                       f"{self.sysctl} -w {CEILING_SETTING}={CEILING_TEMPORARY}"))
        except SettingError as error:
            raise SettingError(error.operation, f"{argv}: {error.detail}") from error

    def _probe_write(self, value: int, timeout: float | None = None,
                     *, deadline: float | None = None) -> None:
        argv = (*self.sudo, "-u", "root", "-l", "--", self.sysctl, "-w", f"{CEILING_SETTING}={value}")
        try:
            result = self._run(argv, timeout, deadline=deadline)
        except SettingError as error:
            raise SettingError("preflight_write_probe_unavailable", f"{argv}: {error}") from error
        if result.returncode != 0:
            raise SettingError(
                "preflight_write_unauthorized",
                f"{argv} -> {result.returncode}: {result.stderr.strip()}",
            )
