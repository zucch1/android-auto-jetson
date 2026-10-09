# SPDX-License-Identifier: GPL-3.0-or-later
# /// script
# requires-python = ">=3.12"
# dependencies = []
# ///
# Run: streamed to the target by probe-usb-aoa.sh; host tests use fake roots/runners.
"""Read-only target USB-gadget, UDC and host-tool evidence.

Nothing here writes a gadget, binds a UDC, toggles USB authorization, invokes
``adb`` or installs a package. ``active`` requires a gadget actually bound to a
UDC reporting ``configured``; a merely configured-but-idle controller is reported
as idle coexistence rather than a pass.
"""
from __future__ import annotations

import shutil
import subprocess
from collections.abc import Callable, Sequence
from dataclasses import dataclass
from pathlib import Path
from typing import Final, Protocol

UDC_ROOT: Final = Path("/sys/class/udc")
CONFIGFS_ROOT: Final = Path("/sys/kernel/config/usb_gadget")
JOURNAL_WINDOW_S: Final = 30
COMMAND_TIMEOUT_S: Final = 10
JOURNAL_LINES: Final = 20


@dataclass(frozen=True, slots=True)
class CommandResult:
    args: tuple[str, ...]
    exit_code: int
    stdout: str
    stderr: str


class CommandRunner(Protocol):
    def run(self, args: Sequence[str], timeout_s: float) -> CommandResult: ...


class SubprocessRunner:
    """Bounded, shell-free runner; a missing binary is a normal result, not a crash."""

    def run(self, args: Sequence[str], timeout_s: float) -> CommandResult:
        argv = list(args)
        try:
            completed = subprocess.run(argv, capture_output=True, text=True,
                                       timeout=timeout_s, check=False)
        except FileNotFoundError:
            return CommandResult(tuple(argv), 127, "", "not-found")
        except subprocess.TimeoutExpired as error:
            out = error.stdout if isinstance(error.stdout, str) else ""
            return CommandResult(tuple(argv), 124, out, "timeout")
        return CommandResult(tuple(argv), completed.returncode, completed.stdout, completed.stderr)


@dataclass(frozen=True, slots=True)
class UdcState:
    name: str
    state: str
    bound_gadget: str
    configured: bool


@dataclass(frozen=True, slots=True)
class GadgetStatus:
    udcs: tuple[UdcState, ...]
    configfs_gadgets: tuple[str, ...]
    configured: bool
    active: bool
    state: str


@dataclass(frozen=True, slots=True)
class HostEnvironment:
    modemmanager_state: str
    modemmanager_active: bool
    mmcli_present: bool
    journal_lines: int
    adb_present: bool
    adb_path: str


def _read(path: Path) -> str | None:
    try:
        return path.read_text(encoding="utf-8", errors="replace").strip()
    except OSError:
        return None


def gadget_status(udc_root: Path = UDC_ROOT, configfs_root: Path = CONFIGFS_ROOT) -> GadgetStatus:
    """Snapshot UDC and configfs binding state without asserting an unbound active pass."""
    bound: dict[str, str] = {}
    gadgets: list[str] = []
    if configfs_root.is_dir():
        for gadget in sorted(configfs_root.iterdir()):
            gadgets.append(gadget.name)
            udc = _read(gadget / "UDC")
            if udc:
                bound[udc] = gadget.name
    udcs: list[UdcState] = []
    if udc_root.is_dir():
        for entry in sorted(udc_root.iterdir()):
            udcs.append(UdcState(
                name=entry.name,
                state=_read(entry / "state") or "unknown",
                bound_gadget=bound.get(entry.name, ""),
                configured=entry.name in bound,
            ))
    configured = bool(bound)
    active = configured and any(udc.configured and udc.state == "configured" for udc in udcs)
    return GadgetStatus(
        udcs=tuple(udcs),
        configfs_gadgets=tuple(gadgets),
        configured=configured,
        active=active,
        state=udcs[0].state if udcs else "idle",
    )


def host_environment(runner: CommandRunner,
                     which: Callable[[str], str | None] = shutil.which) -> HostEnvironment:
    """Bounded ModemManager state plus presence-only adb/mmcli discovery."""
    state = runner.run(["systemctl", "is-active", "ModemManager"], COMMAND_TIMEOUT_S)
    reported = state.stdout.strip()
    journal = runner.run(
        ["journalctl", "-u", "ModemManager", "--since", f"-{JOURNAL_WINDOW_S}s",
         "--no-pager", "-n", str(JOURNAL_LINES)], JOURNAL_WINDOW_S)
    adb = which("adb")
    return HostEnvironment(
        modemmanager_state=reported or "unknown",
        modemmanager_active=reported == "active",
        mmcli_present=which("mmcli") is not None,
        journal_lines=len([line for line in journal.stdout.splitlines() if line.strip()]),
        adb_present=adb is not None,
        adb_path=adb or "",
    )
