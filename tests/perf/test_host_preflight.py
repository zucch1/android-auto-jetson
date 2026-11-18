#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# /// script
# requires-python = ">=3.12"
# dependencies = []
# ///
# Run: python3 -B tests/perf/test_host_preflight.py
"""The soak host preflight: PipeWire real-time, package maintenance, apt timers (#154).

Every command is injected canned output (shaped like this host's), so nothing
here reads the real ps, systemctl or busctl.
"""
from __future__ import annotations

import math
import sys
from datetime import datetime, timezone
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools/perf"))
import aa_host_preflight as hp  # noqa: E402

UID = "1000"
NOW = datetime(2026, 11, 18, 0, 45, 0, tzinfo=timezone.utc).timestamp()  # 00:45 UTC
BOOT = NOW - 5 * 86400  # the host's CLOCK_MONOTONIC zero, as an epoch
PS_RR = "CLS RTPRIO COMMAND\n TS      - pipewire\n TS      - module-rt\n RR     20 pw-data-loop\n"
PS_RR_PULSE = PS_RR.replace(" pipewire\n", " pipewire-pulse\n", 1)
PS_TS = PS_RR.replace(" RR     20", " TS      -")
PS_FF = PS_RR.replace(" RR     20", " FF     50")
PS_NO_LOOP = "CLS RTPRIO COMMAND\n TS      - pipewire\n TS      - module-rt\n"
IDLE = "    1 systemd\n  842 sshd\n 2716563 pipewire\n"
HEADER = "S   ID  QUANT   RATE    WAIT    BUSY   W/Q   B/Q  ERR FORMAT           NAME "
PK_IDLE = {"owner": True, "transactions": 0, "locked": False}
PK_GONE = {"owner": False}


def pw_row(status: str, node: int, err: int, name: str, fmt: str = "") -> str:
    row = f"{status}   {node:<3} {256:>6} {48000:>6}    ---     ---   ---   --- {err:>4} "
    return row + f"{fmt:<16} " + name


# pw-top -b -n 2 prints the table twice; the second one holds the measured rows.
PW_TOP = "\n".join([
    HEADER, pw_row("C", 29, 0, "Dummy-Driver"), pw_row("S", 30, 0, "Freewheel-Driver"),
    HEADER, pw_row("R", 46, 0, "alsa_output.platform-sound.analog-stereo", "S16LE 2 48000"),
    pw_row("R", 71, 3, "aa-wired-receiver", "S16LE 2 48000"), pw_row("R", 72, 0, "Dummy-Driver")]) + "\n"


def at(epoch: float) -> str:
    return datetime.fromtimestamp(epoch, timezone.utc).strftime("%a %Y-%m-%d %H:%M:%S UTC")


def span(seconds: float) -> str:
    """A systemd timespan as systemctl prints it ("4d 15min 15.5s")."""
    days, rest = divmod(seconds, 86400)
    minutes, secs = divmod(rest, 60)
    return " ".join(part for part in (f"{days:.0f}d" if days else "", f"{minutes:.0f}min" if minutes else "",
                                      f"{secs:g}s" if secs else "") if part) or "0"


def seq(value):
    """A fixture value that may change call by call: a list is consumed, its last item kept."""
    return value if isinstance(value, list) else [value]


class Host:
    """A canned host: argv -> Output, on a shared fake clock, with a call log."""

    def __init__(self, *, ps_pipewire=PS_RR, ps_pulse=PS_RR_PULSE, processes=IDLE, services="inactive\ninactive\n",
                 timers=(NOW + 3 * 3600, NOW + 3.5 * 3600), monotonic=(None, None), timer_text=None,
                 packagekit=PK_IDLE, pw_top=PW_TOP, pw_top_delay=0.0) -> None:
        self.ps = {"100": seq(ps_pipewire), "200": seq(ps_pulse)}
        self.processes, self.services, self.packagekit = seq(processes), seq(services), seq(packagekit)
        self.timers, self.monotonic, self.timer_text = list(timers), list(monotonic), timer_text
        self.pw_top, self.pw_top_delay = pw_top, pw_top_delay
        self.calls: list[tuple[str, float | None]] = []
        self.pids = {"pipewire": "100\n", "pipewire-pulse": "200\n"}
        self.state = {"now": NOW, "mono": NOW - BOOT, "slept": []}

    @staticmethod
    def take(values: list):
        return values.pop(0) if len(values) > 1 else values[0]

    def __call__(self, argv, timeout=None) -> hp.Output:
        line = " ".join(argv)
        self.calls.append((line, timeout))
        if line == "id -u":
            return hp.Output(0, UID + "\n")
        if line.startswith("pgrep -u 1000 -x "):
            pids = self.pids[argv[-1]]
            return hp.Output(0 if pids else 1, pids)
        if line.startswith("ps -L -o cls,rtprio,comm -p "):
            return hp.Output(0, self.take(self.ps[argv[-1]]))
        if line == "ps -e -o pid=,comm=":
            return hp.Output(0, self.take(self.processes))
        if line.startswith("systemctl is-active"):
            return hp.Output(3, self.take(self.services))
        if line.startswith("busctl"):
            return self.busctl(argv)
        if "NextElapseUSecRealtime" in line:
            index = 0 if argv[-1] == "apt-daily.timer" else 1
            if self.timer_text is not None:
                return hp.Output(0, self.timer_text)
            real, mono = self.timers[index], self.monotonic[index]
            return hp.Output(0, f"NextElapseUSecRealtime={'' if real is None else at(real)}\n"
                                f"NextElapseUSecMonotonic={'0' if mono is None else span(mono)}\n")
        if argv[0] == "python3":  # the host clock
            return hp.Output(0, f"{self.state['mono']} {self.state['now']}\n")
        if "pw-top" in line:
            self.advance(self.pw_top_delay)
            return hp.Output(0, self.pw_top)
        raise AssertionError(f"unexpected command {line!r}")

    def busctl(self, argv) -> hp.Output:
        pk = self.take(self.packagekit)
        if pk is None:
            return hp.Output(1, "")
        method = argv[-1] if argv[-2] != "s" else argv[-3]
        if method == "NameHasOwner":
            return hp.Output(0, f"b {'true' if pk['owner'] else 'false'}\n")
        if method == "GetNameOwner":
            return hp.Output(0, 's ":1.87376"\n') if pk["owner"] else hp.Output(1, "")
        assert argv[3] == ":1.87376", f"PackageKit addressed by its activatable name: {argv}"
        if method == "GetTransactionList":
            paths = " ".join(f'"/{n}_abc"' for n in range(pk["transactions"]))
            return hp.Output(0, f"ao {pk['transactions']} {paths}".rstrip() + "\n")
        if method == "Locked":
            return hp.Output(0, f"b {'true' if pk['locked'] else 'false'}\n")
        raise AssertionError(argv)

    def advance(self, seconds: float, wall: float | None = None) -> None:
        self.state["mono"] += seconds
        self.state["now"] += seconds if wall is None else wall

    def sleep(self, seconds: float) -> bool:
        self.state["slept"].append(seconds)
        self.advance(seconds)
        return True


def check(run, stage_s: int = 1800, sleep=None, host: Host | None = None, **kwargs) -> dict:
    """The preflight over ``run`` on ``host``'s fake clock (``run`` itself when it is a Host)."""
    host = host or run
    return hp.preflight(run, stage_s, now=lambda: host.state["now"],
                        monotonic=lambda: host.state["mono"], sleep=sleep or host.sleep, **kwargs)


def wrap(host: Host, override):
    """A runner: ``override(argv)`` when it returns an Output, else the canned host."""
    def run(argv, timeout=None):
        out = override(argv)
        return out if out is not None else host(argv, timeout)
    return run


def raises(call, error=ValueError) -> bool:
    try:
        call()
    except error:
        return True
    return False


# ---- PipeWire scheduling ------------------------------------------------------
def test_realtime_pipewire_passes_and_is_recorded() -> None:
    for ps, cls in ((PS_RR, "RR"), (PS_FF, "FF")):  # RR and FF are both real-time
        record = check(Host(ps_pipewire=ps))
        assert record["verdict"] == "OK" and record["reasons"] == [], record["reasons"]
        scheduling = record["pipewire_scheduling"]
        assert [(p["process"], p["pid"]) for p in scheduling["processes"]] == \
            [("pipewire", 100), ("pipewire-pulse", 200)]
        assert scheduling["processes"][0]["data_loops"][0]["cls"] == cls
        assert record["schema"] == "aa-host-preflight/2" and record["uid"] == UID


def test_a_non_realtime_data_loop_blocks_the_stage() -> None:
    for host, who in ((Host(ps_pulse=PS_TS), "pipewire-pulse[200]"), (Host(ps_pipewire=PS_TS), "pipewire[100]")):
        record = check(host)
        assert record["verdict"] == "BLOCKED"
        assert any(r.startswith(f"host_audio:{who}:pw-data-loop_not_realtime (cls=TS") for r in record["reasons"]), \
            record["reasons"]
        assert record["pipewire_scheduling"]["ok"] is False


def test_a_missing_process_or_data_loop_thread_blocks_the_stage() -> None:
    assert any(":no_pw-data-loop_thread" in r for r in check(Host(ps_pulse=PS_NO_LOOP))["reasons"])
    host = Host()
    host.pids["pipewire-pulse"] = ""
    record = check(host)
    assert record["verdict"] == "BLOCKED" and "host_audio:pipewire-pulse:not_running" in record["reasons"]


def test_scheduling_is_read_after_the_maintenance_wait() -> None:
    # pipewire-pulse loses RT during the wait, then the host goes package-idle:
    # the launch decision sees the fresh TS row (the review of #157).
    busy = IDLE + "  4242 dpkg\n"
    host = Host(processes=[busy, IDLE], ps_pulse=PS_TS)
    record = check(host, maintenance_wait_s=600)
    assert host.state["slept"] == [10.0] and record["verdict"] == "BLOCKED", record["reasons"]
    assert any("pipewire-pulse[200]:pw-data-loop_not_realtime" in r for r in record["reasons"])
    order = [line.split()[0] + (" " + line.split()[1] if line.startswith("ps") else "") for line, _ in host.calls]
    assert order.index("ps -L") > max(i for i, c in enumerate(order) if c == "ps -e"), order


# ---- package maintenance --------------------------------------------------------
def test_apt_or_a_maintenance_service_or_packagekit_work_waits_then_blocks() -> None:
    for kwargs, needle in (
            ({"processes": IDLE + "  4242 dpkg\n"}, "host_maintenance:dpkg_running (pid 4242)"),
            ({"processes": IDLE + "  4243 apt-get\n"}, "host_maintenance:apt-get_running"),
            ({"processes": IDLE + "  4244 unattended-upgr\n"}, "host_maintenance:unattended-upgr_running"),
            ({"services": "active\ninactive\n"}, "host_maintenance:apt-daily.service_active"),
            ({"services": "inactive\nactivating\n"}, "host_maintenance:apt-daily-upgrade.service_activating"),
            ({"packagekit": {**PK_IDLE, "transactions": 1}}, "host_maintenance:packagekit_transactions (1)"),
            ({"packagekit": {**PK_IDLE, "locked": True}}, "host_maintenance:packagekit_locked")):
        host = Host(**kwargs)
        record = check(host, maintenance_wait_s=60)
        # It waited the whole bound (polling, never longer) and is still busy: BLOCKED.
        assert record["verdict"] == "BLOCKED" and any(needle in r for r in record["reasons"]), record["reasons"]
        assert sum(host.state["slept"]) == 60 and max(host.state["slept"]) <= hp.POLL_S, host.state["slept"]
        assert record["package_maintenance"]["waited_s"] == 60


def test_an_idle_resident_packagekitd_does_not_block() -> None:
    # The daemon exists (no idle shutdown) but has no transaction and no lock.
    host = Host(processes=IDLE + "  3036517 packagekitd\n", packagekit=PK_IDLE)
    record = check(host, maintenance_wait_s=600)
    assert record["verdict"] == "OK" and host.state["slept"] == [], record["reasons"]
    assert record["package_maintenance"]["packagekit"] == {
        "ok": True, "unverifiable": False, "reasons": [], "daemon": True, "transactions": 0, "locked": False}
    # No daemon on the bus: idle, and only NameHasOwner was asked (nothing activated).
    host = Host(packagekit=PK_GONE)
    assert check(host)["verdict"] == "OK"
    assert [c for c, _ in host.calls if c.startswith("busctl")] == [
        "busctl --system call org.freedesktop.DBus /org/freedesktop/DBus org.freedesktop.DBus NameHasOwner s "
        "org.freedesktop.PackageKit"]
    # It exits on idle between NameHasOwner and GetNameOwner: idle too.
    host = Host(packagekit=[PK_IDLE, PK_GONE])
    assert check(host)["verdict"] == "OK"


def test_unreadable_packagekit_state_blocks_without_waiting() -> None:
    host = Host(packagekit=None)
    record = check(host, maintenance_wait_s=600)
    assert record["verdict"] == "BLOCKED" and host.state["slept"] == []
    assert "host_maintenance:packagekit:unverifiable (busctl failed)" in record["reasons"]


def test_maintenance_that_finishes_inside_the_wait_lets_the_stage_run() -> None:
    host = Host(processes=[IDLE + "  4242 dpkg\n", IDLE + "  4242 dpkg\n", IDLE])  # busy twice, then idle
    record = check(host, maintenance_wait_s=600)
    assert record["verdict"] == "OK" and host.state["slept"] == [10.0, 10.0], (record["reasons"], host.state)
    assert record["package_maintenance"]["waited_s"] == 20.0


def test_the_wait_is_bounded_on_the_monotonic_clock() -> None:
    # The wall clock steps back 30 s on every poll: the bound still ends the wait.
    host = Host(processes=IDLE + "  4242 dpkg\n")

    def sleep(seconds: float) -> bool:
        host.state["slept"].append(seconds)
        host.advance(seconds, wall=-30.0)
        return len(host.state["slept"]) < 100  # a stop that should never be needed
    record = check(host, sleep=sleep, maintenance_wait_s=20)
    assert record["verdict"] == "BLOCKED" and sum(host.state["slept"]) == 20, host.state["slept"]
    assert "host_maintenance:wait_interrupted" not in record["reasons"]
    # A probe after a poll is capped to the time left (here 5 s, never the 15 s default),
    # and once the bound is spent no probe runs that could admit the stage late.
    host = Host(processes=IDLE + "  4242 dpkg\n")
    check(host, maintenance_wait_s=15)
    probes = [t for c, t in host.calls if c == "ps -e -o pid=,comm="]
    assert probes == [None, 5.0], probes


def test_zero_wait_blocks_at_once_and_a_stop_ends_the_wait() -> None:
    busy = IDLE + "  4242 dpkg\n"
    host = Host(processes=busy)
    assert check(host, maintenance_wait_s=0)["verdict"] == "BLOCKED" and host.state["slept"] == []
    record = check(Host(processes=busy), sleep=lambda s: False, maintenance_wait_s=600)
    assert "host_maintenance:wait_interrupted" in record["reasons"]


def test_non_finite_or_negative_durations_are_rejected() -> None:
    for value in (math.inf, math.nan, -1.0):
        assert raises(lambda: check(Host(), maintenance_wait_s=value)), value
        assert raises(lambda: check(Host(), margin_s=value)), value
    assert hp.finite_seconds(0, "x") == 0.0


def test_an_unreadable_maintenance_state_blocks_without_waiting() -> None:
    host = Host()
    run = wrap(host, lambda argv: hp.Output(None) if argv[:3] == ["ps", "-e", "-o"] else None)
    record = hp.preflight(run, 1800, now=lambda: NOW, monotonic=lambda: host.state["mono"],
                          sleep=host.sleep, maintenance_wait_s=600)
    assert record["verdict"] == "BLOCKED" and host.state["slept"] == []
    assert any("host_maintenance:unverifiable" in r for r in record["reasons"])


# ---- apt timers ------------------------------------------------------------------
def test_a_timer_inside_the_stage_window_refuses_the_stage() -> None:
    # 1800 s stage + 900 s margin from 00:45 ends 01:30; apt-daily elapses 01:00.
    for timers, unit in (((NOW + 900, NOW + 5 * 3600), "apt-daily.timer"),
                         ((NOW + 5 * 3600, NOW + 2400), "apt-daily-upgrade.timer"),
                         ((NOW - 60, NOW + 5 * 3600), "apt-daily.timer")):  # overdue counts as inside
        record = check(Host(timers=timers))
        assert record["verdict"] == "BLOCKED", timers
        assert any(f"host_maintenance:{unit}_inside_stage_window" in r and "start after" in r
                   for r in record["reasons"]), record["reasons"]
        assert [t["unit"] for t in record["apt_timers"]["timers"] if t.get("inside_window")] == [unit]


def test_a_timer_outside_the_window_passes() -> None:
    # The same stage, timers at 03:45 / 04:15 (outside), and one second past the window's end.
    record = check(Host(timers=(NOW + 3 * 3600, NOW + 3.5 * 3600)))
    assert record["verdict"] == "OK", record["reasons"]
    assert [t["next_elapse_epoch"] for t in record["apt_timers"]["timers"]] == [NOW + 10800, NOW + 12600]
    edge = 1800 + 900
    assert check(Host(timers=(NOW + edge + 1, NOW + edge + 1)))["verdict"] == "OK"
    assert check(Host(timers=(NOW + edge, NOW + edge + 1)))["verdict"] == "BLOCKED"  # the boundary is inside
    # A 7200 s stage reaches further (to 03:00 with the margin): a timer at 02:30 is inside it.
    assert check(Host(timers=(NOW + 6300, NOW + 3 * 3600)), 7200)["verdict"] == "BLOCKED"
    assert check(Host(timers=(NOW + 3 * 3600, NOW + 3.5 * 3600)), 7200)["verdict"] == "OK"


def test_the_timer_window_starts_after_the_other_reads() -> None:
    # A timer one second past the window measured at the preflight's start: a 3 s
    # counter read moves the launch, so it is inside the window that counts.
    edge = NOW + 1800 + 900 + 1
    assert check(Host(timers=(edge, edge)))["verdict"] == "OK"
    record = check(Host(timers=(edge, edge), pw_top_delay=3.0))
    assert record["verdict"] == "BLOCKED" and record["apt_timers"]["checked_epoch"] == NOW + 3, record["reasons"]


def test_a_monotonic_timer_trigger_counts() -> None:
    # OnUnitActiveSec= / OnBootSec=: no calendar elapse, a monotonic one 20 min from now.
    host_mono = NOW - BOOT
    record = check(Host(timers=(None, None), monotonic=(host_mono + 1200, None)))
    assert record["verdict"] == "BLOCKED", record["reasons"]
    item = record["apt_timers"]["timers"][0]
    assert item["next_elapse_epoch"] == NOW + 1200 and item["monotonic"]["state"] == "time"
    # The earlier of the two triggers wins.
    record = check(Host(timers=(NOW + 5 * 3600, None), monotonic=(host_mono + 600, None)))
    assert record["apt_timers"]["timers"][0]["next_elapse_epoch"] == NOW + 600
    # A monotonic elapse past the window passes.
    assert check(Host(timers=(None, None), monotonic=(host_mono + 4 * 3600, None)))["verdict"] == "OK"
    assert hp.parse_monotonic("4d 15min 15.660736s") == ("time", 4 * 86400 + 15 * 60 + 15.660736)
    assert hp.parse_monotonic("0") == ("none", None) and hp.parse_monotonic("2 fortnights")[0] == "unparsable"


def test_timer_elapse_parsing_and_failure_modes() -> None:
    assert hp.parse_realtime("Wed 2026-11-18 09:00:00 UTC") == \
        ("time", datetime(2026, 11, 18, 9, tzinfo=timezone.utc).timestamp())
    assert hp.parse_realtime("") == ("none", None) and hp.parse_realtime("n/a")[0] == "none"
    assert hp.parse_realtime("Wed 2026-11-18 04:00:00 EST")[0] == "unparsable"
    assert hp.parse_realtime("Mon 2026-02-30 04:00:00 UTC")[0] == "unparsable"  # no such date
    assert check(Host(timers=(None, None)))["verdict"] == "OK"  # no next elapse: it cannot fire
    for text, needle in (
            ("NextElapseUSecRealtime=soon\nNextElapseUSecMonotonic=0\n", "next_elapse_realtime_unparsable"),
            ("NextElapseUSecRealtime=Mon 2026-02-30 04:00:00 UTC\nNextElapseUSecMonotonic=0\n",
             "next_elapse_realtime_unparsable"),
            ("", "next_elapse_realtime_missing"),  # exit 0, empty stdout
            ("unexpected=\n", "next_elapse_realtime_missing"),
            ("NextElapseUSecRealtime=\n", "next_elapse_monotonic_missing")):
        record = check(Host(timer_text=text))
        assert record["verdict"] == "BLOCKED" and f"host_maintenance:apt-daily.timer:{needle}" in \
            record["reasons"], (text, record["reasons"])


def test_an_unreadable_host_clock_or_timer_query_blocks() -> None:
    host = Host()
    record = check(wrap(host, lambda argv: hp.Output(1, "") if argv[0] == "python3" else None), host=host)
    assert record["verdict"] == "BLOCKED" and "host_maintenance:host_clock_unreadable" in record["reasons"]
    record = check(wrap(host, lambda argv: hp.Output(None) if "NextElapseUSecRealtime" in argv else None), host=host)
    assert "host_maintenance:apt-daily.timer:next_elapse_realtime_unverifiable" in record["reasons"]


# ---- transport ------------------------------------------------------------------------
def test_the_ssh_runner_uses_the_launchers_transport_and_bounds() -> None:
    seen = []

    def local(argv, timeout=None):
        seen.append((argv, timeout))
        return hp.Output(0, "1000\n")
    run = hp.ssh_runner("localhost", local)
    assert hp.soak_uid(run) == UID
    argv, timeout = seen[0]
    launcher = (ROOT / "tools/perf/aa-projection-perf.sh").read_text()
    assert "SSH_OPTS=(" + " ".join(hp.SSH_OPTS) + ")" in launcher, "keep the launcher's ssh options"
    assert argv == ["ssh", *hp.SSH_OPTS, "localhost", "id -u"] and timeout == hp.COMMAND_TIMEOUT_S + 10
    run(["pw-top"], timeout=hp.PW_TOP_TIMEOUT_S)
    assert seen[-1][1] == hp.PW_TOP_TIMEOUT_S + 10


# ---- informational PipeWire ERR counters -------------------------------------------
def test_pw_top_err_counters_are_recorded_and_never_gate() -> None:
    record = check(Host(), receiver="/opt/aa/aa-wired-receiver")
    start = record["pipewire_err_counters"]["start"]
    assert record["verdict"] == "OK" and start["phase"] == "before_launch"
    assert [(r["name"], r["err"]) for r in start["receiver_streams"]] == [("aa-wired-receiver", 3)]
    assert [(r["name"], r["err"]) for r in start["sinks"]] == \
        [("alsa_output.platform-sound.analog-stereo", 0)]
    assert len(start["nodes"]) == 3, start["nodes"]  # the last table only
    # An unreadable pw-top is recorded and still does not block.
    host = Host()
    record = check(wrap(host, lambda argv: hp.Output(1, "") if "pw-top" in argv else None), host=host)
    assert record["verdict"] == "OK" and record["pipewire_err_counters"]["start"]["error"] == "pw-top failed"
    # Even a huge ERR count gates nothing.
    big = PW_TOP.replace("   3 S16LE", "9999 S16LE")
    assert check(Host(pw_top=big))["verdict"] == "OK"


def test_an_unreadable_user_blocks() -> None:
    record = check(lambda argv, timeout=None: hp.Output(None), host=Host())
    assert record["verdict"] == "BLOCKED" and record["reasons"] == ["host_audio:unverifiable (id -u failed)"]


if __name__ == "__main__":
    tests = [value for name, value in list(globals().items()) if name.startswith("test_")]
    for test in tests:
        test()
    print(f"PASS {len(tests)} host preflight tests")
