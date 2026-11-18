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

Three checks, all read-only (``ps``, ``systemctl show/is-active``, ``busctl``
calls addressed so they never D-Bus-activate a service):

1. **Package maintenance** (first, because it may wait). No ``apt``,
   ``apt-get``, ``dpkg``, ``unattended-upgrades`` or ``apt.systemd.daily``
   process, neither ``apt-daily.service`` nor ``apt-daily-upgrade.service``
   active, and PackageKit idle. PackageKit counts as busy only when it is
   actually working: its bus name has an owner and that daemon reports a
   transaction (``GetTransactionList``) or holds the package lock
   (``Locked``). An idle resident ``packagekitd`` does not count; one whose
   state cannot be read is BLOCKED. The bus name is checked with
   ``NameHasOwner`` and the daemon is then addressed by its unique name, so the
   query never starts PackageKit. This check is transient, so the chain
   **waits** for it: at most ``maintenance_wait_s`` (default 900 s,
   0 = BLOCK at once) on a monotonic clock, polling every ``POLL_S``, with
   each probe's timeout capped to the time left. Still busy after that is
   BLOCKED.
2. **Scheduling.** Checked after the wait, so a change during the wait is
   seen. Every ``pw-data-loop`` thread of every ``pipewire`` and
   ``pipewire-pulse`` process of the soak user must be SCHED_RR or SCHED_FF
   (``ps -L -o cls,rtprio,comm -p PID``). A missing process, a missing
   thread or any other class is BLOCKED.
3. **Timers** (last). For ``apt-daily.timer`` and ``apt-daily-upgrade.timer``
   both next elapses are read: ``NextElapseUSecRealtime`` (OnCalendar=) and
   ``NextElapseUSecMonotonic`` (OnBootSec=, OnUnitActiveSec=, ...; converted
   through the host's own monotonic and realtime clocks). The earlier one
   must be later than the stage's planned end plus a margin (default 900 s,
   covering the launcher's setup and cleanup), counted from the host clock
   read after the timer queries. Otherwise the stage is **refused**
   (BLOCKED), with the earliest safe start in the reason; it does not wait,
   because the wait could be hours. An overdue timer counts as inside the
   window. A missing property, an unparsable or impossible value, or an
   unreadable host clock is BLOCKED; an empty, ``n/a`` or ``0`` value means
   that trigger has no next elapse.

Every command goes through an injected runner, so the checks are tested with
canned output. Informational only: ``pw-top -b -n 2`` ERR counters per node
(``read_pw_top``), never gated.
"""
from __future__ import annotations

import math
import re
import shlex
import subprocess
import time
from collections.abc import Callable, Sequence
from dataclasses import dataclass
from datetime import datetime, timezone

SCHEMA = "aa-host-preflight/2"
PIPEWIRE_PROCESSES = ("pipewire", "pipewire-pulse")
DATA_LOOP_THREAD = "pw-data-loop"
REALTIME_CLASSES = frozenset({"RR", "FF"})
# /proc comm names (15 characters at most): "unattended-upgrades" is "unattended-upgr".
# packagekitd is not here: a resident daemon is idle unless it reports work (check 1).
MAINTENANCE_PROCESSES = frozenset({"apt", "apt-get", "dpkg", "unattended-upgr", "apt.systemd.dai"})
MAINTENANCE_SERVICES = ("apt-daily.service", "apt-daily-upgrade.service")
BUSY_SERVICE_STATES = frozenset({"active", "activating", "reloading", "deactivating"})
APT_TIMERS = ("apt-daily.timer", "apt-daily-upgrade.timer")
PACKAGEKIT_NAME = "org.freedesktop.PackageKit"
PACKAGEKIT_PATH = "/org/freedesktop/PackageKit"
DEFAULT_TIMER_MARGIN_S = 900.0
DEFAULT_MAINTENANCE_WAIT_S = 900.0
POLL_S = 10.0
COMMAND_TIMEOUT_S = 15.0
MIN_PROBE_TIMEOUT_S = 1.0
PW_TOP_TIMEOUT_S = 30.0
SSH_CONNECT_TIMEOUT_S = 10
# The launcher's own ssh options (aa-projection-perf.sh SSH_OPTS): the same
# transport and target selection, so the preflight sees the soak's user.
SSH_OPTS = ("-o", "BatchMode=yes", "-o", f"ConnectTimeout={SSH_CONNECT_TIMEOUT_S}",
            "-o", "ServerAliveInterval=15", "-o", "ServerAliveCountMax=3")
COUNTER_ROWS_MAX = 64
HOST_CLOCK = ("python3", "-c",
              "import time; print(time.clock_gettime(time.CLOCK_MONOTONIC), time.time())")


@dataclass(frozen=True)
class Output:
    """One command's result; ``rc`` is None when it could not run or timed out."""
    rc: int | None
    stdout: str = ""


# runner(argv, timeout=None) -> Output; timeout None means the command's default.
Runner = Callable[..., Output]


def finite_seconds(value: float, name: str) -> float:
    """A duration option: finite and not negative (``inf``/``nan`` would remove a bound)."""
    value = float(value)
    if not math.isfinite(value) or value < 0:
        raise ValueError(f"{name} must be a finite number of seconds >= 0, not {value!r}")
    return value


def local_runner(argv: Sequence[str], timeout: float | None = None) -> Output:
    try:
        done = subprocess.run(list(argv), capture_output=True, text=True, check=False,
                              timeout=COMMAND_TIMEOUT_S if timeout is None else timeout)
    except (OSError, subprocess.SubprocessError):
        return Output(None)
    return Output(done.returncode, done.stdout)


def ssh_runner(host: str, local: Runner = local_runner) -> Runner:
    """The launcher's transport: bounded, key-only ssh to ``host`` (also for localhost)."""
    def run(argv: Sequence[str], timeout: float | None = None) -> Output:
        command = " ".join(shlex.quote(part) for part in argv)
        budget = (COMMAND_TIMEOUT_S if timeout is None else timeout) + SSH_CONNECT_TIMEOUT_S
        return local(["ssh", *SSH_OPTS, host, command], timeout=budget)
    return run


# ---- PipeWire scheduling -----------------------------------------------------
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


# ---- package maintenance -------------------------------------------------------
def _busctl_value(text: str, signature: str) -> list[str] | None:
    """``busctl`` text output ("b true", "ao 0", 's ":1.5"') -> its words after the signature."""
    parts = text.split()
    return parts[1:] if parts and parts[0] == signature else None


def check_packagekit(run: Runner, timeout: float | None = None) -> dict:
    """PackageKit is busy only with a transaction or the package lock; never activates it."""
    def owner_present() -> bool | None:
        out = run(["busctl", "--system", "call", "org.freedesktop.DBus", "/org/freedesktop/DBus",
                   "org.freedesktop.DBus", "NameHasOwner", "s", PACKAGEKIT_NAME], timeout=timeout)
        value = _busctl_value(out.stdout, "b") if out.rc == 0 else None
        return None if not value else value[0] == "true"

    unverifiable = {"ok": False, "unverifiable": True,
                    "reasons": ["host_maintenance:packagekit:unverifiable (busctl failed)"]}
    present = owner_present()
    if present is None:
        return unverifiable
    if not present:
        return {"ok": True, "unverifiable": False, "reasons": [], "daemon": False}
    owner = run(["busctl", "--system", "call", "org.freedesktop.DBus", "/org/freedesktop/DBus",
                 "org.freedesktop.DBus", "GetNameOwner", "s", PACKAGEKIT_NAME], timeout=timeout)
    unique = _busctl_value(owner.stdout, "s") if owner.rc == 0 else None
    if not unique:  # it may just have exited on idle
        return {"ok": True, "unverifiable": False, "reasons": [], "daemon": False} \
            if owner_present() is False else unverifiable
    name = unique[0].strip('"')  # a unique name: a message to it can never activate a service
    transactions = run(["busctl", "--system", "call", name, PACKAGEKIT_PATH, PACKAGEKIT_NAME,
                        "GetTransactionList"], timeout=timeout)
    locked = run(["busctl", "--system", "get-property", name, PACKAGEKIT_PATH, PACKAGEKIT_NAME,
                  "Locked"], timeout=timeout)
    listed = _busctl_value(transactions.stdout, "ao") if transactions.rc == 0 else None
    lock = _busctl_value(locked.stdout, "b") if locked.rc == 0 else None
    if not listed or not listed[0].isdigit() or not lock or lock[0] not in ("true", "false"):
        return unverifiable
    count, is_locked = int(listed[0]), lock[0] == "true"
    reasons = ([f"host_maintenance:packagekit_transactions ({count})"] if count else []) + \
        (["host_maintenance:packagekit_locked"] if is_locked else [])
    return {"ok": not reasons, "unverifiable": False, "reasons": reasons, "daemon": True,
            "transactions": count, "locked": is_locked}


def check_package_maintenance(run: Runner, timeout: float | None = None) -> dict:
    """Maintenance processes running, an apt-daily service active, or PackageKit working."""
    reasons, running, active = [], [], []
    unverifiable = False
    ps = run(["ps", "-e", "-o", "pid=,comm="], timeout=timeout)
    if ps.rc != 0:
        unverifiable = True
        reasons.append("host_maintenance:unverifiable (ps failed)")
    else:
        for line in ps.stdout.splitlines():
            parts = line.split(None, 1)
            if len(parts) == 2 and parts[1].strip() in MAINTENANCE_PROCESSES:
                running.append({"pid": int(parts[0]), "comm": parts[1].strip()})
    states = run(["systemctl", "is-active", *MAINTENANCE_SERVICES], timeout=timeout)
    lines = states.stdout.split()
    if states.rc is None or len(lines) != len(MAINTENANCE_SERVICES):
        unverifiable = True
        reasons.append("host_maintenance:unverifiable (systemctl is-active failed)")
    else:
        # is-active exits non-zero when a unit is inactive, so only the text counts.
        active = [{"unit": unit, "state": state} for unit, state in zip(MAINTENANCE_SERVICES, lines)
                  if state in BUSY_SERVICE_STATES]
    packagekit = check_packagekit(run, timeout)
    unverifiable = unverifiable or packagekit["unverifiable"]
    reasons += [f"host_maintenance:{item['comm']}_running (pid {item['pid']})" for item in running]
    reasons += [f"host_maintenance:{item['unit']}_{item['state']}" for item in active]
    reasons += packagekit["reasons"]
    return {"ok": not reasons, "reasons": reasons, "unverifiable": unverifiable,
            "processes": running, "services": active, "packagekit": packagekit}


# ---- apt timers ------------------------------------------------------------------
_ELAPSE = re.compile(r"^(?:[A-Z][a-z]{2} )?(\d{4})-(\d{2})-(\d{2}) (\d{2}):(\d{2}):(\d{2})(?:\.\d+)? UTC$")
_SPAN_UNITS = {"us": 1e-6, "usec": 1e-6, "ms": 1e-3, "msec": 1e-3, "s": 1.0, "sec": 1.0,
               "m": 60.0, "min": 60.0, "h": 3600.0, "hr": 3600.0, "d": 86400.0,
               "w": 604800.0, "M": 2629800.0, "month": 2629800.0, "y": 31557600.0}
_SPAN_PART = re.compile(r"(\d+(?:\.\d+)?)([a-zA-Z]+)")
_NONE_VALUES = ("", "n/a", "0", "infinity")


def properties(output: str) -> dict[str, str]:
    """``systemctl show`` ``Key=value`` lines -> {key: value}."""
    found = {}
    for line in output.splitlines():
        key, sep, value = line.partition("=")
        if sep:
            found[key.strip()] = value.strip()
    return found


def parse_realtime(value: str) -> tuple[str, float | None]:
    """A ``NextElapseUSecRealtime`` value -> ("none"|"time"|"unparsable", epoch seconds).

    Asked with ``TZ=UTC LC_ALL=C``, systemd prints ``Wed 2026-11-18 09:00:00 UTC``;
    no next elapse is an empty value, ``n/a`` or ``0``.
    """
    if value in _NONE_VALUES:
        return "none", None
    match = _ELAPSE.match(value)
    if not match:
        return "unparsable", None
    try:
        stamp = datetime(*map(int, match.groups()), tzinfo=timezone.utc)
    except ValueError:  # matches the pattern but is no date (Feb 30, hour 25)
        return "unparsable", None
    return "time", stamp.timestamp()


def parse_monotonic(value: str) -> tuple[str, float | None]:
    """A ``NextElapseUSecMonotonic`` timespan ("4d 15min 15.66s") -> seconds on CLOCK_MONOTONIC."""
    if value in _NONE_VALUES:
        return "none", None
    words = value.split()
    total = 0.0
    for word in words:
        match = _SPAN_PART.fullmatch(word)
        if not match or match.group(2) not in _SPAN_UNITS:
            return "unparsable", None
        total += float(match.group(1)) * _SPAN_UNITS[match.group(2)]
    return "time", total


def parse_host_clock(output: Output) -> tuple[float, float] | None:
    """The host's (CLOCK_MONOTONIC, realtime epoch) from ``HOST_CLOCK``, or None."""
    parts = output.stdout.split() if output.rc == 0 else []
    try:
        mono, real = (float(part) for part in parts)
    except ValueError:
        return None
    return (mono, real) if math.isfinite(mono) and math.isfinite(real) else None


def _utc(epoch: float, fmt: str) -> str:
    return datetime.fromtimestamp(epoch, timezone.utc).strftime(fmt)


def check_apt_timers(run: Runner, stage_s: float, margin_s: float = DEFAULT_TIMER_MARGIN_S) -> dict:
    """Refuse a stage whose planned window plus margin contains an apt timer's elapse.

    Both triggers count: calendar (realtime) and monotonic (OnBootSec=,
    OnUnitActiveSec=, ...). The window starts at the host clock read after the
    timer queries, the latest point before the chain launches the stage.
    """
    margin_s = finite_seconds(margin_s, "timer margin")
    timers, reasons = [], []
    for unit in APT_TIMERS:
        out = run(["env", "TZ=UTC", "LC_ALL=C", "systemctl", "show",
                   "-p", "NextElapseUSecRealtime", "-p", "NextElapseUSecMonotonic", unit])
        found = properties(out.stdout) if out.rc == 0 else {}
        item: dict = {"unit": unit}
        for key, parse, label in (("NextElapseUSecRealtime", parse_realtime, "realtime"),
                                  ("NextElapseUSecMonotonic", parse_monotonic, "monotonic")):
            if out.rc != 0:
                kind, value = "unverifiable", None
            elif key not in found:
                kind, value = "missing", None
            else:
                kind, value = parse(found[key])
            item[label] = {"state": kind, "value": value, "raw": found.get(key)}
            if kind not in ("none", "time"):
                reasons.append(f"host_maintenance:{unit}:next_elapse_{label}_{kind}")
        timers.append(item)
    clock = parse_host_clock(run(list(HOST_CLOCK)))
    if clock is None:
        reasons.append("host_maintenance:host_clock_unreadable")
        return {"ok": False, "reasons": reasons, "timers": timers, "window_s": stage_s,
                "margin_s": margin_s}
    host_mono, host_now = clock
    window_end = host_now + stage_s + margin_s
    for item in timers:
        elapses = []
        if item["realtime"]["state"] == "time":
            elapses.append(item["realtime"]["value"])
        if item["monotonic"]["state"] == "time":  # a CLOCK_MONOTONIC point: map it to the epoch
            elapses.append(host_now + item["monotonic"]["value"] - host_mono)
        item["next_elapse_epoch"] = min(elapses) if elapses else None
        epoch = item["next_elapse_epoch"]
        if epoch is not None and epoch <= window_end:
            item["inside_window"] = True
            reasons.append(f"host_maintenance:{item['unit']}_inside_stage_window (elapses "
                           f"{_utc(epoch, '%Y-%m-%d %H:%M:%S UTC')}; stage {stage_s:.0f} s + margin "
                           f"{margin_s:.0f} s from now; start after {_utc(epoch + margin_s, '%H:%M UTC')})")
    return {"ok": not reasons, "reasons": reasons, "timers": timers, "window_s": stage_s,
            "margin_s": margin_s, "checked_epoch": host_now, "window_end_epoch": window_end}


# ---- informational: pw-top ERR counters -------------------------------------------
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
    out = run(["env", f"XDG_RUNTIME_DIR=/run/user/{uid}", "pw-top", "-b", "-n", "2"],
              timeout=PW_TOP_TIMEOUT_S)
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


def soak_uid(run: Runner) -> str | None:
    """The uid the runner's commands run as: the soak's user when it is the launcher's transport."""
    out = run(["id", "-u"])
    uid = out.stdout.strip()
    return uid if out.rc == 0 and uid.isdigit() else None


# ---- the preflight -----------------------------------------------------------------
def preflight(run: Runner, stage_s: float, *, now: Callable[[], float] = time.time,
              monotonic: Callable[[], float] = time.monotonic,
              sleep: Callable[[float], object] = time.sleep, uid: str | None = None,
              margin_s: float = DEFAULT_TIMER_MARGIN_S,
              maintenance_wait_s: float = DEFAULT_MAINTENANCE_WAIT_S,
              receiver: str | None = None) -> dict:
    """Run the checks; the record says OK or BLOCKED.

    Order: the (possibly waiting) maintenance check first, then the
    informational counters, then the scheduling and timer checks, so the ones
    a launch depends on are as fresh as possible when the chain launches.
    ``sleep`` returns False when the chain is stopped; the wait then ends and
    the record is BLOCKED ``host_maintenance:wait_interrupted``.
    """
    margin_s = finite_seconds(margin_s, "timer margin")
    maintenance_wait_s = finite_seconds(maintenance_wait_s, "maintenance wait")
    record: dict = {"schema": SCHEMA, "stage_s": stage_s, "started_epoch": now()}
    uid = uid if uid is not None else soak_uid(run)
    if uid is None:
        record.update(verdict="BLOCKED", reasons=["host_audio:unverifiable (id -u failed)"])
        return record
    record["uid"] = uid

    began = monotonic()
    deadline = began + maintenance_wait_s
    maintenance = check_package_maintenance(run)
    interrupted = False
    while not maintenance["ok"] and not maintenance["unverifiable"]:  # busy: wait, bounded
        remaining = deadline - monotonic()
        if remaining <= 0:
            break
        if sleep(min(POLL_S, remaining)) is False:
            interrupted = True
            break
        remaining = deadline - monotonic()
        if remaining <= 0:  # the bound is spent: no probe that could admit the stage late
            break
        maintenance = check_package_maintenance(
            run, timeout=max(MIN_PROBE_TIMEOUT_S, min(COMMAND_TIMEOUT_S, remaining)))
    maintenance["waited_s"] = round(monotonic() - began, 3)
    maintenance["wait_limit_s"] = maintenance_wait_s
    if interrupted and not maintenance["ok"]:
        maintenance["reasons"].append("host_maintenance:wait_interrupted")

    counters = read_pw_top(run, uid, receiver, now())  # informational, before the fresh checks
    scheduling = check_pipewire_scheduling(run, uid)
    timers = check_apt_timers(run, stage_s, margin_s)
    reasons = maintenance["reasons"] + scheduling["reasons"] + timers["reasons"]
    record.update(package_maintenance=maintenance, pipewire_scheduling=scheduling, apt_timers=timers,
                  pipewire_err_counters={"start": {**counters, "phase": "before_launch"}},
                  checked_epoch=now(), verdict="OK" if not reasons else "BLOCKED", reasons=reasons)
    return record
