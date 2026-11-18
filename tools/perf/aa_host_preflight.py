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
with ``--auto-start=no`` and validated unique names, so no service is ever
started), run in this order:

1. **Package maintenance** (first, because it may wait). No ``apt``,
   ``apt-get``, ``dpkg``, ``unattended-upgrades`` or ``apt.systemd.daily``
   process, neither ``apt-daily.service`` nor ``apt-daily-upgrade.service``
   active, and PackageKit idle. PackageKit counts as busy only when it is
   actually working: its bus name has an owner and that daemon reports a
   transaction (``GetTransactionList``) or holds the package lock
   (``Locked``). An idle resident ``packagekitd`` does not count. The bus
   name is checked with ``NameHasOwner`` and the daemon then addressed by its
   unique name (``:N.M``, validated); any reply that does not have its exact
   shape is unverifiable, so BLOCKED. Busy is transient, so the chain
   **waits**: every probe, the first included, and the polls every ``POLL_S``
   share one monotonic deadline of ``maintenance_wait_s`` (default 900 s;
   0 = a single observation with its own bounds); each command's whole budget
   is the time left, and a probe that completes after the deadline is
   rejected. Still busy, or late, is BLOCKED.
2. **Scheduling, then the informational counters.** Every ``pw-data-loop``
   thread of every ``pipewire`` and ``pipewire-pulse`` process must be
   SCHED_RR or SCHED_FF (``ps -L -o cls,rtprio,comm -p PID``); a missing
   process, a missing thread or any other class is BLOCKED. ``pw-top`` then
   runs only when a ``pipewire`` process already exists, and the pids are read
   again after it: a PipeWire that changed (e.g. exited and was
   socket-activated by ``pw-top`` in that small window, an accepted residual
   race) BLOCKs, so the verdict never rests on a daemon the preflight started.
3. **Timers.** For ``apt-daily.timer`` and ``apt-daily-upgrade.timer`` both
   next elapses are read: ``NextElapseUSecRealtime`` (OnCalendar=) and
   ``NextElapseUSecMonotonic`` (OnBootSec=, OnUnitActiveSec=, ...; converted
   through the host's own monotonic and realtime clocks, and bounded to a
   century). The earlier one must be later than the stage's planned end plus
   a margin (default 900 s, covering the launcher's setup and cleanup),
   counted from the host clock read after the timer queries. Otherwise the
   stage is **refused** (BLOCKED), with the earliest safe start in the
   reason; it does not wait, because the wait could be hours. An overdue
   timer counts as inside the window. A missing property, an unparsable,
   impossible or out-of-range value, or an unreadable host clock is BLOCKED;
   an empty, ``n/a`` or ``0`` value means that trigger has no next elapse.
4. **Package maintenance again**, without waiting: work that began during 2
   and 3 BLOCKs the launch. Then **the timers again**, last, on a fresh host
   clock, so the final package read cannot age their cutoff.

An ssh failure (exit 255, or no answer) of any gating probe is reported as
``host_transport:...`` with ssh's own message, not as the probe's own failure. Every command goes
through an injected runner, so the checks are tested with canned output.
Informational only: ``pw-top -b -n 2`` ERR counters per node
(``read_pw_top``), never gated.
"""
from __future__ import annotations

import math
import os
import re
import shlex
import signal
import subprocess
import threading
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
PW_TOP_TIMEOUT_S = 30.0
KILL_DRAIN_S = 0.5
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
    stderr: str = ""


# runner(argv, timeout=None) -> Output. ``timeout`` is the whole subprocess
# budget (for ssh: connection included); None means the command's default.
Runner = Callable[..., Output]
SSH_FAILED = 255  # ssh's own exit status: connection, host key or authentication


def finite_seconds(value: float, name: str) -> float:
    """A duration option: finite and not negative (``inf``/``nan`` would remove a bound)."""
    value = float(value)
    if not math.isfinite(value) or value < 0:
        raise ValueError(f"{name} must be a finite number of seconds >= 0, not {value!r}")
    return value


def local_runner(argv: Sequence[str], timeout: float | None = None,
                 cancel: threading.Event | None = None) -> Output:
    """Run a command, bounded by ``timeout``; a set ``cancel`` kills it at once."""
    budget = COMMAND_TIMEOUT_S if timeout is None else timeout
    if budget <= 0 or (cancel is not None and cancel.is_set()):
        return Output(None)
    try:  # its own process group, so a kill reaches its helpers too
        process = subprocess.Popen(list(argv), stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                   stdin=subprocess.DEVNULL, text=True, start_new_session=True)
    except OSError:
        return Output(None)
    deadline = time.monotonic() + budget
    while True:
        step = min(0.2, deadline - time.monotonic())
        try:
            stdout, stderr = process.communicate(timeout=max(step, 0.01))
            return Output(process.returncode, stdout, stderr)
        except subprocess.TimeoutExpired:
            if time.monotonic() >= deadline or (cancel is not None and cancel.is_set()):
                _kill(process)
                return Output(None)


def _kill(process: subprocess.Popen) -> None:
    """KILL the command's group; never wait unboundedly on pipes a detached helper still holds."""
    try:
        os.killpg(process.pid, signal.SIGKILL)
    except (ProcessLookupError, PermissionError):
        pass
    try:
        process.communicate(timeout=KILL_DRAIN_S)
    except subprocess.TimeoutExpired:
        for pipe in (process.stdout, process.stderr):
            try:
                pipe.close()
            except OSError:
                pass
        try:
            process.wait(timeout=KILL_DRAIN_S)
        except subprocess.TimeoutExpired:
            pass


def cancellable_runner(cancel: threading.Event) -> Runner:
    """``local_runner`` that stops its command as soon as ``cancel`` is set."""
    def run(argv: Sequence[str], timeout: float | None = None) -> Output:
        return local_runner(argv, timeout, cancel)
    return run


def ssh_runner(host: str, local: Runner = local_runner) -> Runner:
    """The launcher's transport: bounded, key-only ssh to ``host`` (also for localhost).

    A given ``timeout`` is the whole budget, connection included; the default
    is the command's own budget plus the connection timeout.
    """
    def run(argv: Sequence[str], timeout: float | None = None) -> Output:
        command = " ".join(shlex.quote(part) for part in argv)
        budget = COMMAND_TIMEOUT_S + SSH_CONNECT_TIMEOUT_S if timeout is None else timeout
        return local(["ssh", *SSH_OPTS, host, command], timeout=budget)
    return run


def transport_failure(out: Output) -> str | None:
    """A reason when ``out`` is ssh failing (exit 255), not the remote command."""
    if out.rc is None:
        return "host_transport:unreachable (no answer within the bound)"
    if out.rc != SSH_FAILED:
        return None
    lines = [line.strip() for line in out.stderr.splitlines() if line.strip()]
    detail = lines[-1][:160] if lines else "connection, host key or authentication"
    return f"host_transport:ssh_failed ({detail})"


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
    processes, reasons, all_pids = [], [], {}
    for name in PIPEWIRE_PROCESSES:
        pids = run(["pgrep", "-u", uid, "-x", name])
        found = pids.stdout.split() if pids.rc == 0 else []
        all_pids[name] = sorted(found)
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
    return {"ok": not reasons, "reasons": reasons, "processes": processes, "pids": all_pids}


def pipewire_pids(run: Runner, uid: str) -> dict[str, list[str]] | None:
    """{process name: sorted pids} of the user's PipeWire daemons, or None if unreadable."""
    found = {}
    for name in PIPEWIRE_PROCESSES:
        out = run(["pgrep", "-u", uid, "-x", name])
        if out.rc not in (0, 1):
            return None
        found[name] = sorted(out.stdout.split())
    return found


# ---- package maintenance -------------------------------------------------------
_UNIQUE_NAME = re.compile(r'^":[0-9]+\.[0-9]+"$')


def _busctl_bool(out: Output, *prefix: str) -> bool | None:
    """Exactly ``[prefix...] b true|false``; anything else is None (fail closed)."""
    parts = out.stdout.split() if out.rc == 0 else []
    if parts[:-1] != [*prefix, "b"] or parts[-1] not in ("true", "false"):
        return None
    return parts[-1] == "true"


def _busctl_unique_name(out: Output) -> str | None:
    """Exactly ``s ":N.M"``: a unique bus name, which a message can never activate."""
    parts = out.stdout.split() if out.rc == 0 else []
    return parts[1].strip('"') if len(parts) == 2 and parts[0] == "s" and _UNIQUE_NAME.match(parts[1]) else None


def _busctl_paths(out: Output) -> int | None:
    """Exactly ``ao N path...`` with N paths: the count, else None."""
    parts = out.stdout.split() if out.rc == 0 else []
    if len(parts) < 2 or parts[0] != "ao" or not parts[1].isdigit() or len(parts) != 2 + int(parts[1]):
        return None
    return int(parts[1])


BUSCTL = ("busctl", "--system", "--auto-start=no")


def check_packagekit(run: Runner, timeout: Callable[[], float | None] = lambda: None) -> dict:
    """PackageKit is busy only with a transaction or the package lock; never activates it.

    Every reply must have its exact shape, and the daemon is addressed only by
    a validated unique name (``:N.M``), with ``--auto-start=no`` on every call
    as well; a malformed reply is unverifiable (BLOCKED), never a pass.
    """
    def owner_present() -> bool | None:
        return _busctl_bool(run([*BUSCTL, "call", "org.freedesktop.DBus", "/org/freedesktop/DBus",
                                 "org.freedesktop.DBus", "NameHasOwner", "s", PACKAGEKIT_NAME],
                                timeout=timeout()))

    unverifiable = {"ok": False, "unverifiable": True,
                    "reasons": ["host_maintenance:packagekit:unverifiable (busctl failed)"]}
    present = owner_present()
    if present is None:
        return unverifiable
    if not present:
        return {"ok": True, "unverifiable": False, "reasons": [], "daemon": False}
    owner = run([*BUSCTL, "call", "org.freedesktop.DBus", "/org/freedesktop/DBus",
                 "org.freedesktop.DBus", "GetNameOwner", "s", PACKAGEKIT_NAME], timeout=timeout())
    name = _busctl_unique_name(owner)
    if name is None:  # it may just have exited on idle; a malformed reply is unverifiable
        return {"ok": True, "unverifiable": False, "reasons": [], "daemon": False} \
            if owner.rc != 0 and owner_present() is False else unverifiable
    count = _busctl_paths(run([*BUSCTL, "call", name, PACKAGEKIT_PATH, PACKAGEKIT_NAME,
                               "GetTransactionList"], timeout=timeout()))
    is_locked = _busctl_bool(run([*BUSCTL, "call", name, PACKAGEKIT_PATH, "org.freedesktop.DBus.Properties",
                                  "Get", "ss", PACKAGEKIT_NAME, "Locked"], timeout=timeout()), "v")
    if count is None or is_locked is None:
        return unverifiable
    reasons = ([f"host_maintenance:packagekit_transactions ({count})"] if count else []) + \
        (["host_maintenance:packagekit_locked"] if is_locked else [])
    return {"ok": not reasons, "unverifiable": False, "reasons": reasons, "daemon": True,
            "transactions": count, "locked": is_locked}


def check_package_maintenance(run: Runner, timeout: Callable[[], float | None] = lambda: None) -> dict:
    """Maintenance processes running, an apt-daily service active, or PackageKit working.

    ``timeout()`` is asked before every command, so a poll shares one deadline.
    """
    reasons, running, active = [], [], []
    unverifiable = False
    ps = run(["ps", "-e", "-o", "pid=,comm="], timeout=timeout())
    if ps.rc != 0:
        unverifiable = True
        reasons.append("host_maintenance:unverifiable (ps failed)")
    else:
        for line in ps.stdout.splitlines():
            parts = line.split(None, 1)
            if len(parts) == 2 and parts[1].strip() in MAINTENANCE_PROCESSES:
                running.append({"pid": int(parts[0]), "comm": parts[1].strip()})
    states = run(["systemctl", "is-active", *MAINTENANCE_SERVICES], timeout=timeout())
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
_SPAN_PART = re.compile(r"(\d{1,12}(?:\.\d{1,9})?)([a-zA-Z]+)")
MONOTONIC_MAX_S = 100 * 31557600.0  # a CLOCK_MONOTONIC point beyond a century is not a real elapse
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
    if not math.isfinite(total) or total > MONOTONIC_MAX_S:
        return "unparsable", None
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
        if not all(math.isfinite(value) for value in elapses):  # never write a non-JSON value
            item["next_elapse_epoch"] = None
            reasons.append(f"host_maintenance:{item['unit']}:next_elapse_not_finite")
            continue
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
    record: dict = {"time": time.time() if now is None else now, "source": "pw-top -b -n 2"}
    # pw-top connects to the PipeWire socket, and a listening pipewire.socket would
    # start the daemon: read only from a daemon that is already running.
    running = run(["pgrep", "-u", uid, "-x", "pipewire"])
    if running.rc != 0 or not running.stdout.split():
        return {**record, "skipped": "pipewire_not_running" if running.rc == 1 else "pgrep failed"}
    out = run(["env", f"XDG_RUNTIME_DIR=/run/user/{uid}", "pw-top", "-b", "-n", "2"],
              timeout=PW_TOP_TIMEOUT_S)
    if out.rc != 0:
        return {**record, "error": transport_failure(out) or "pw-top failed"}
    rows = parse_pw_top(out.stdout)
    needles = {"aa-wired-receiver"}
    if receiver:
        needles.add(receiver.rstrip("/").rsplit("/", 1)[-1])
    receiver_rows = [r for r in rows if any(n in r["name"] for n in needles)]
    sink_rows = [r for r in rows if "_output." in r["name"]]
    return {**record, "receiver_streams": receiver_rows, "sinks": sink_rows,
            "nodes": rows[:COUNTER_ROWS_MAX], "nodes_truncated": len(rows) > COUNTER_ROWS_MAX}


def soak_uid(run: Runner) -> tuple[str | None, str | None]:
    """(uid, None) for the user the runner's commands run as, else (None, reason).

    The first command over the transport: an ssh failure is reported as one.
    """
    out = run(["id", "-u"])
    uid = out.stdout.strip()
    if out.rc == 0 and uid.isdigit():
        return uid, None
    return None, transport_failure(out) or "host_audio:unverifiable (id -u failed)"


# ---- the preflight -----------------------------------------------------------------
def preflight(run: Runner, stage_s: float, *, now: Callable[[], float] = time.time,
              monotonic: Callable[[], float] = time.monotonic,
              sleep: Callable[[float], object] = time.sleep, uid: str | None = None,
              margin_s: float = DEFAULT_TIMER_MARGIN_S,
              maintenance_wait_s: float = DEFAULT_MAINTENANCE_WAIT_S,
              receiver: str | None = None) -> dict:
    """Run the checks; the record says OK or BLOCKED.

    Order: the maintenance check, waiting while it is busy; scheduling; the
    informational counters (only from a PipeWire that is already running),
    then a check that the PipeWire pids are the ones scheduling judged; the
    timers; a second, non-waiting maintenance read; and the timers again, from
    a fresh host clock, so neither gate is aged by the other.

    With a positive ``maintenance_wait_s`` every maintenance probe, the first
    included, shares one deadline on ``monotonic`` (each command's whole
    budget, ssh connection included, is the time left), and a probe that
    completes after it is rejected (BLOCKED
    ``host_maintenance:wait_limit_exceeded``) whatever it saw. 0 is a single
    observation with the commands' own bounds. ``sleep`` returns False when the
    chain is stopped; the wait then ends and the record is BLOCKED
    ``host_maintenance:wait_interrupted``.

    Residual race (accepted): PipeWire may exit between the pid guard and
    ``pw-top``, which may then socket-activate the user's own audio server.
    The stage still BLOCKs, because the pids no longer match the ones whose
    real-time scheduling was judged (``host_audio:pipewire_changed_during_preflight``).
    """
    margin_s = finite_seconds(margin_s, "timer margin")
    maintenance_wait_s = finite_seconds(maintenance_wait_s, "maintenance wait")
    record: dict = {"schema": SCHEMA, "stage_s": stage_s, "started_epoch": now()}
    if uid is None:
        uid, failure = soak_uid(run)
        if uid is None:
            record.update(verdict="BLOCKED", reasons=[failure])
            return record
    record["uid"] = uid
    transport: list[str] = []

    def gate(argv, timeout=None) -> Output:
        """The runner for the gating probes: an ssh failure keeps its own label."""
        out = run(argv, timeout=timeout)
        failure = transport_failure(out) if out.rc in (None, SSH_FAILED) else None
        if failure and failure not in transport:
            transport.append(failure)
        return out

    began = monotonic()
    deadline = began + maintenance_wait_s
    bounded = maintenance_wait_s > 0
    budget = (lambda: deadline - monotonic()) if bounded else (lambda: None)
    maintenance = check_package_maintenance(gate, timeout=budget)
    late = bounded and monotonic() > deadline
    interrupted = False
    while not late and not maintenance["ok"] and not maintenance["unverifiable"]:  # busy: wait
        remaining = deadline - monotonic()
        if remaining <= 0:
            break
        if sleep(min(POLL_S, remaining)) is False:
            interrupted = True
            break
        if deadline - monotonic() <= 0:  # the bound is spent: no probe that could admit the stage late
            break
        maintenance = check_package_maintenance(gate, timeout=budget)
        late = monotonic() > deadline  # finished late: what it saw no longer counts
    maintenance["waited_s"] = round(monotonic() - began, 3)
    maintenance["wait_limit_s"] = maintenance_wait_s
    if late:
        maintenance.update(ok=False)
        maintenance["reasons"].append("host_maintenance:wait_limit_exceeded")
    if interrupted and not maintenance["ok"]:
        maintenance["reasons"].append("host_maintenance:wait_interrupted")

    scheduling = check_pipewire_scheduling(gate, uid)
    counters = read_pw_top(run, uid, receiver, now())  # informational: never gates
    after = pipewire_pids(gate, uid)
    if scheduling["ok"] and after != scheduling["pids"]:
        scheduling.update(ok=False, pids_after_counters=after)
        scheduling["reasons"].append("host_audio:pipewire_changed_during_preflight")
    timers = check_apt_timers(gate, stage_s, margin_s)
    final = check_package_maintenance(gate)  # nothing may have started since
    final_timers = check_apt_timers(gate, stage_s, margin_s)  # last, on a fresh clock
    reasons = transport + maintenance["reasons"] + scheduling["reasons"] + timers["reasons"]
    if maintenance["ok"]:
        reasons += [f"{reason} (at the final check)" for reason in final["reasons"]]
    reasons += [reason for reason in final_timers["reasons"] if reason not in reasons]
    record.update(package_maintenance=maintenance, pipewire_scheduling=scheduling, apt_timers=timers,
                  package_maintenance_final=final, apt_timers_final=final_timers,
                  pipewire_err_counters={"start": {**counters, "phase": "before_launch"}},
                  checked_epoch=now(), verdict="OK" if not reasons else "BLOCKED", reasons=reasons)
    return record
