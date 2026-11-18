#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# /// script
# requires-python = ">=3.12"
# dependencies = []
# ///
"""Host audio / package-maintenance preflight for the soak stages (#154).

A 1800 s soak stage failed from a ~400 ms stall in the PipeWire Pulse server.
Two host conditions caused it: the user's PipeWire daemons ran SCHED_OTHER
(their real-time grant had failed at boot), and apt-daily / PackageKit work
landed inside the stage. This module checks, before a stage starts, that
neither can happen, the way ``aa_soak_chain`` already checks for builds. No
A/V or audio gate threshold is involved: a host that fails a check cannot
qualify the audio at all, so the stage is BLOCKED, not run.

Three checks (each is read-only: ``ps``, ``systemctl show/is-active``):

1. **Scheduling.** Every ``pw-data-loop`` thread of every ``pipewire`` and
   ``pipewire-pulse`` process of the user must be SCHED_RR or SCHED_FF
   (``ps -L -o cls,rtprio,comm -p PID``). A missing process, a missing
   thread or any other class is BLOCKED.
2. **Package maintenance.** No ``apt``, ``apt-get``, ``dpkg``,
   ``unattended-upgrades``, ``packagekitd`` (or the ``apt.systemd.daily``
   wrapper) process, and neither ``apt-daily.service`` nor
   ``apt-daily-upgrade.service`` active. This one is transient, so the chain
   **waits** for it, bounded by ``--maintenance-wait-s`` (default 900 s,
   0 = BLOCK at once), polling every ``POLL_S``; still busy after that is
   BLOCKED.
3. **Timers.** The next elapse of ``apt-daily.timer`` and
   ``apt-daily-upgrade.timer`` (``systemctl show -p NextElapseUSecRealtime``)
   must be later than the stage's planned end plus a margin (default 900 s,
   covering the launcher's setup and cleanup). Otherwise the stage is
   **refused** (BLOCKED), with the earliest safe start in the reason; it does
   not wait, because the wait could be hours. An overdue timer (elapse in the
   past) counts as inside the window. An unparsable elapse is BLOCKED; a
   timer with no next elapse (inactive or disabled) cannot fire.

Every command goes through an injected runner, so the checks are tested with
canned output. Informational only: ``pw-top -b -n 2`` ERR counters per node
(``read_pw_top``), recorded at stage start and end, never gated.
"""
from __future__ import annotations

import re
import shlex
import subprocess
import time
from collections.abc import Callable, Sequence
from dataclasses import dataclass
from datetime import datetime, timezone

SCHEMA = "aa-host-preflight/1"
PIPEWIRE_PROCESSES = ("pipewire", "pipewire-pulse")
DATA_LOOP_THREAD = "pw-data-loop"
REALTIME_CLASSES = frozenset({"RR", "FF"})
# /proc comm names (15 characters at most): "unattended-upgrades" is "unattended-upgr".
MAINTENANCE_PROCESSES = frozenset({
    "apt", "apt-get", "dpkg", "unattended-upgr", "packagekitd", "apt.systemd.dai"})
MAINTENANCE_SERVICES = ("apt-daily.service", "apt-daily-upgrade.service")
BUSY_SERVICE_STATES = frozenset({"active", "activating", "reloading", "deactivating"})
APT_TIMERS = ("apt-daily.timer", "apt-daily-upgrade.timer")
DEFAULT_TIMER_MARGIN_S = 900.0
DEFAULT_MAINTENANCE_WAIT_S = 900.0
POLL_S = 10.0
COMMAND_TIMEOUT_S = 15.0
PW_TOP_TIMEOUT_S = 30.0
COUNTER_ROWS_MAX = 64


@dataclass(frozen=True)
class Output:
    """One command's result; ``rc`` is None when it could not run or timed out."""
    rc: int | None
    stdout: str = ""


Runner = Callable[[Sequence[str]], Output]


def local_runner(argv: Sequence[str], timeout: float = COMMAND_TIMEOUT_S) -> Output:
    try:
        done = subprocess.run(list(argv), capture_output=True, text=True, timeout=timeout, check=False)
    except (OSError, subprocess.SubprocessError):
        return Output(None)
    return Output(done.returncode, done.stdout)


def ssh_runner(host: str) -> Runner:
    """A runner for a soak host other than this one: bounded, key-only ssh."""
    def run(argv: Sequence[str]) -> Output:
        command = " ".join(shlex.quote(part) for part in argv)
        return local_runner(["ssh", "-o", "BatchMode=yes", "-o", "ConnectTimeout=10", host, command],
                            timeout=PW_TOP_TIMEOUT_S + 15)
    return run


# ---- 1. PipeWire scheduling --------------------------------------------------
def parse_threads(text: str) -> list[dict]:
    """Rows of ``ps -L -o cls,rtprio,comm``: [{"cls", "rtprio", "comm"}]; the header is skipped."""
    rows = []
    for line in text.splitlines():
        parts = line.split(None, 2)
        if len(parts) == 3 and parts[0] != "CLS":
            rows.append({"cls": parts[0], "rtprio": parts[1], "comm": parts[2].strip()})
    return rows


def check_pipewire_scheduling(run: Runner, uid: str) -> dict:
    """The ``pw-data-loop`` threads of the user's pipewire and pipewire-pulse must be RR/FF."""
    processes, reasons = [], []
    for name in PIPEWIRE_PROCESSES:
        pids = run(["pgrep", "-u", uid, "-x", name])
        found = pids.stdout.split() if pids.rc == 0 else []
        if pids.rc is None:
            reasons.append(f"host_audio:{name}:unverifiable (pgrep failed)")
        elif not found:
            reasons.append(f"host_audio:{name}:not_running")
        for pid in found:
            ps = run(["ps", "-L", "-o", "cls,rtprio,comm", "-p", pid])
            if ps.rc != 0:
                reasons.append(f"host_audio:{name}[{pid}]:unverifiable (ps failed)")
                continue
            loops = [row for row in parse_threads(ps.stdout) if row["comm"] == DATA_LOOP_THREAD]
            processes.append({"process": name, "pid": int(pid), "data_loops": loops})
            if not loops:
                reasons.append(f"host_audio:{name}[{pid}]:no_{DATA_LOOP_THREAD}_thread")
            for row in loops:
                if row["cls"] not in REALTIME_CLASSES:
                    reasons.append(f"host_audio:{name}[{pid}]:{DATA_LOOP_THREAD}_not_realtime "
                                   f"(cls={row['cls']} rtprio={row['rtprio']})")
    return {"ok": not reasons, "reasons": reasons, "processes": processes}


# ---- 2. package maintenance ---------------------------------------------------
def check_package_maintenance(run: Runner) -> dict:
    """Maintenance processes running, or an apt-daily service active."""
    reasons, running, active = [], [], []
    unverifiable = False
    ps = run(["ps", "-e", "-o", "pid=,comm="])
    if ps.rc != 0:
        unverifiable = True
        reasons.append("host_maintenance:unverifiable (ps failed)")
    else:
        for line in ps.stdout.splitlines():
            parts = line.split(None, 1)
            if len(parts) == 2 and parts[1].strip() in MAINTENANCE_PROCESSES:
                running.append({"pid": int(parts[0]), "comm": parts[1].strip()})
    states = run(["systemctl", "is-active", *MAINTENANCE_SERVICES])
    lines = states.stdout.split()
    if states.rc is None or len(lines) != len(MAINTENANCE_SERVICES):
        unverifiable = True
        reasons.append("host_maintenance:unverifiable (systemctl is-active failed)")
    else:
        # is-active exits non-zero when a unit is inactive, so only the text counts.
        active = [{"unit": unit, "state": state} for unit, state in zip(MAINTENANCE_SERVICES, lines)
                  if state in BUSY_SERVICE_STATES]
    reasons += [f"host_maintenance:{item['comm']}_running (pid {item['pid']})" for item in running]
    reasons += [f"host_maintenance:{item['unit']}_{item['state']}" for item in active]
    return {"ok": not reasons, "reasons": reasons, "unverifiable": unverifiable,
            "processes": running, "services": active}


# ---- 3. apt timers -------------------------------------------------------------
_ELAPSE = re.compile(r"^(?:[A-Z][a-z]{2} )?(\d{4})-(\d{2})-(\d{2}) (\d{2}):(\d{2}):(\d{2})(?:\.\d+)? UTC$")


def parse_elapse(output: str) -> tuple[str, float | None]:
    """``NextElapseUSecRealtime=...`` -> ("none"|"time"|"unparsable", epoch seconds).

    Asked with ``TZ=UTC LC_ALL=C``, systemd prints ``Wed 2026-11-18 09:00:00 UTC``;
    an inactive timer prints an empty value, ``n/a`` or ``0``.
    """
    value = output.strip().partition("=")[2].strip()
    if value in ("", "n/a", "0"):
        return "none", None
    match = _ELAPSE.match(value)
    if not match:
        return "unparsable", None
    stamp = datetime(*map(int, match.groups()), tzinfo=timezone.utc)
    return "time", stamp.timestamp()


def check_apt_timers(run: Runner, now: float, stage_s: float,
                     margin_s: float = DEFAULT_TIMER_MARGIN_S) -> dict:
    """Refuse a stage whose planned window plus margin contains an apt timer's elapse."""
    window_end = now + stage_s + margin_s
    timers, reasons = [], []
    for unit in APT_TIMERS:
        out = run(["env", "TZ=UTC", "LC_ALL=C", "systemctl", "show", "-p", "NextElapseUSecRealtime", unit])
        kind, epoch = parse_elapse(out.stdout) if out.rc == 0 else ("unverifiable", None)
        item = {"unit": unit, "next_elapse_epoch": epoch, "state": kind}
        if kind in ("unparsable", "unverifiable"):
            reasons.append(f"host_maintenance:{unit}:next_elapse_{kind}")
        elif epoch is not None and epoch <= window_end:
            item["inside_window"] = True
            when = datetime.fromtimestamp(epoch, timezone.utc).strftime("%Y-%m-%d %H:%M:%S UTC")
            safe = datetime.fromtimestamp(epoch + margin_s, timezone.utc).strftime("%H:%M UTC")
            reasons.append(f"host_maintenance:{unit}_inside_stage_window (elapses {when}; "
                           f"stage {stage_s:.0f} s + margin {margin_s:.0f} s from now; "
                           f"start after {safe})")
        timers.append(item)
    return {"ok": not reasons, "reasons": reasons, "timers": timers, "window_s": stage_s,
            "margin_s": margin_s, "checked_epoch": now}


# ---- informational: pw-top ERR counters -----------------------------------------
_PW_TOP_ROW = re.compile(r"^\s*\S+\s+(\d+)\s+\S+\s+\S+\s+\S+\s+\S+\s+\S+\s+\S+\s+(\d+)\s+(.*)$")


def parse_pw_top(text: str) -> list[dict]:
    """Rows [{"id", "name", "err"}] of the last table of ``pw-top -b -n N``."""
    tables: list[list[str]] = [[]]
    name_column = None
    for line in text.splitlines():
        if line.lstrip().startswith("S") and "ERR" in line and "NAME" in line:
            tables.append([])
            name_column = line.index("NAME")
        elif line.strip():
            tables[-1].append(line)
    rows = []
    for line in tables[-1]:
        match = _PW_TOP_ROW.match(line)
        if not match:
            continue
        name = line[name_column:].strip() if name_column and len(line) > name_column else match.group(3).strip()
        rows.append({"id": int(match.group(1)), "name": name, "err": int(match.group(2))})
    return rows


def read_pw_top(run: Runner, uid: str, receiver: str | None = None, now: float | None = None) -> dict:
    """ERR counters of every PipeWire node, with the receiver's streams and the sinks marked.

    Informational: it never gates a stage. A stream is the receiver's when its
    name contains the receiver binary's name (or ``aa-wired-receiver``); a sink is
    a node named ``*_output.*``. Failure to read is recorded, not raised.
    """
    out = run(["env", f"XDG_RUNTIME_DIR=/run/user/{uid}", "pw-top", "-b", "-n", "2"])
    record: dict = {"time": time.time() if now is None else now, "source": "pw-top -b -n 2"}
    if out.rc != 0:
        return {**record, "error": "pw-top failed" if out.rc is not None else "pw-top could not run"}
    rows = parse_pw_top(out.stdout)
    needles = {"aa-wired-receiver"}
    if receiver:
        needles.add(receiver.rstrip("/").rsplit("/", 1)[-1])
    receiver_rows = [r for r in rows if any(n in r["name"] for n in needles)]
    sink_rows = [r for r in rows if "_output." in r["name"]]
    return {**record, "receiver_streams": receiver_rows, "sinks": sink_rows,
            "nodes": rows[:COUNTER_ROWS_MAX], "nodes_truncated": len(rows) > COUNTER_ROWS_MAX}


# ---- the preflight -------------------------------------------------------------
def preflight(run: Runner, stage_s: float, *, now: Callable[[], float] = time.time,
              sleep: Callable[[float], object] = time.sleep, uid: str | None = None,
              margin_s: float = DEFAULT_TIMER_MARGIN_S,
              maintenance_wait_s: float = DEFAULT_MAINTENANCE_WAIT_S,
              receiver: str | None = None) -> dict:
    """Run the three checks and the informational read; the record says OK or BLOCKED.

    ``sleep`` returns False when the chain is stopped; the wait then ends and the
    record is BLOCKED ``host_maintenance:wait_interrupted``.
    """
    if uid is None:
        out = run(["id", "-u"])
        uid = out.stdout.strip() if out.rc == 0 and out.stdout.strip().isdigit() else None
    record: dict = {"schema": SCHEMA, "stage_s": stage_s, "started_epoch": now()}
    if uid is None:
        record.update(verdict="BLOCKED", reasons=["host_audio:unverifiable (id -u failed)"])
        return record
    record["uid"] = uid
    scheduling = check_pipewire_scheduling(run, uid)
    waited = 0.0
    deadline = now() + maintenance_wait_s
    maintenance = check_package_maintenance(run)
    interrupted = False
    while not maintenance["ok"] and not maintenance["unverifiable"]:  # busy: wait, bounded
        remaining = deadline - now()
        if remaining <= 0:
            break
        step = min(POLL_S, remaining)
        if sleep(step) is False:
            interrupted = True
            break
        waited += step
        maintenance = check_package_maintenance(run)
    maintenance["waited_s"] = waited
    maintenance["wait_limit_s"] = maintenance_wait_s
    if interrupted and not maintenance["ok"]:
        maintenance["reasons"].append("host_maintenance:wait_interrupted")
    # The timers last: the wait above moved "now".
    timers = check_apt_timers(run, now(), stage_s, margin_s)
    reasons = scheduling["reasons"] + maintenance["reasons"] + timers["reasons"]
    record.update(pipewire_scheduling=scheduling, package_maintenance=maintenance, apt_timers=timers,
                  pipewire_err_counters={"start": read_pw_top(run, uid, receiver, now())},
                  verdict="OK" if not reasons else "BLOCKED", reasons=reasons)
    return record
