#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# /// script
# requires-python = ">=3.12"
# dependencies = []
# ///
# Run: python3 -B tests/perf/test_host_preflight.py
"""The soak host preflight: PipeWire real-time, package maintenance, apt timers (#154).

Every command is injected canned output (shaped like this host's), so nothing
here reads the real ps or systemctl.
"""
from __future__ import annotations

import sys
from datetime import datetime, timezone
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools/perf"))
sys.path.insert(0, str(Path(__file__).resolve().parent))
import aa_host_preflight as hp  # noqa: E402

UID = "1000"
NOW = datetime(2026, 11, 18, 0, 45, 0, tzinfo=timezone.utc).timestamp()  # 00:45 UTC
PS_RR = "CLS RTPRIO COMMAND\n TS      - pipewire\n TS      - module-rt\n RR     20 pw-data-loop\n"
PS_RR_PULSE = PS_RR.replace(" pipewire\n", " pipewire-pulse\n", 1)
PS_TS = PS_RR.replace(" RR     20", " TS      -")
PS_FF = PS_RR.replace(" RR     20", " FF     50")
PS_NO_LOOP = "CLS RTPRIO COMMAND\n TS      - pipewire\n TS      - module-rt\n"
IDLE = "    1 systemd\n  842 sshd\n 2716563 pipewire\n"
HEADER = "S   ID  QUANT   RATE    WAIT    BUSY   W/Q   B/Q  ERR FORMAT           NAME "


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


class Host:
    """A canned host: argv (joined) -> Output, with per-call overrides and a call log."""

    def __init__(self, *, ps_pipewire=PS_RR, ps_pulse=PS_RR_PULSE, processes=IDLE, services="inactive\ninactive\n",
                 timers=(NOW + 3 * 3600, NOW + 3.5 * 3600), pw_top=PW_TOP) -> None:
        self.ps_pipewire, self.ps_pulse, self.processes = ps_pipewire, ps_pulse, [processes]
        self.services, self.timers, self.pw_top = [services], list(timers), pw_top
        self.calls: list[str] = []
        self.pids = {"pipewire": "100\n", "pipewire-pulse": "200\n"}

    def __call__(self, argv) -> hp.Output:
        line = " ".join(argv)
        self.calls.append(line)
        if line == "id -u":
            return hp.Output(0, UID + "\n")
        if line.startswith("pgrep -u 1000 -x "):
            pids = self.pids[argv[-1]]
            return hp.Output(0 if pids else 1, pids)
        if line.startswith("ps -L -o cls,rtprio,comm -p "):
            return hp.Output(0, self.ps_pipewire if argv[-1] == "100" else self.ps_pulse)
        if line == "ps -e -o pid=,comm=":
            return hp.Output(0, self.processes[0] if len(self.processes) == 1 else self.processes.pop(0))
        if line.startswith("systemctl is-active"):
            return hp.Output(3, self.services[0] if len(self.services) == 1 else self.services.pop(0))
        if "NextElapseUSecRealtime" in line:
            epoch = self.timers[0 if argv[-1] == "apt-daily.timer" else 1]
            value = "" if epoch is None else at(epoch)
            return hp.Output(0, f"NextElapseUSecRealtime={value}\n")
        if "pw-top" in line:
            return hp.Output(0, self.pw_top)
        raise AssertionError(f"unexpected command {line!r}")


def clock(start: float = NOW):
    """A fake time: sleep advances it; the list records the sleeps."""
    state = {"now": start, "slept": []}

    def sleep(seconds: float) -> bool:
        state["slept"].append(seconds)
        state["now"] += seconds
        return True
    return state, (lambda: state["now"]), sleep


def check(host: Host, stage_s: int = 1800, **kwargs) -> dict:
    state, now, sleep = clock()
    return hp.preflight(host, stage_s, now=now, sleep=sleep, **kwargs)


# ---- 1. PipeWire scheduling ------------------------------------------------------
def test_realtime_pipewire_passes_and_is_recorded() -> None:
    for ps in (PS_RR, PS_FF):  # RR and FF are both real-time
        record = check(Host(ps_pipewire=ps))
        assert record["verdict"] == "OK" and record["reasons"] == [], record["reasons"]
        scheduling = record["pipewire_scheduling"]
        assert [(p["process"], p["pid"]) for p in scheduling["processes"]] == \
            [("pipewire", 100), ("pipewire-pulse", 200)]
        assert scheduling["processes"][0]["data_loops"] == [
            {"cls": ps.split("\n")[3].split()[0], "rtprio": ps.split("\n")[3].split()[1],
             "comm": "pw-data-loop"}]
        assert record["schema"] == "aa-host-preflight/1" and record["uid"] == UID


def test_a_non_realtime_data_loop_blocks_the_stage() -> None:
    for host, who in ((Host(ps_pulse=PS_TS), "pipewire-pulse[200]"), (Host(ps_pipewire=PS_TS), "pipewire[100]")):
        record = check(host)
        assert record["verdict"] == "BLOCKED"
        assert any(r.startswith(f"host_audio:{who}:pw-data-loop_not_realtime (cls=TS") for r in record["reasons"]), \
            record["reasons"]
        assert record["pipewire_scheduling"]["ok"] is False


def test_a_missing_process_or_data_loop_thread_blocks_the_stage() -> None:
    host = Host(ps_pulse=PS_NO_LOOP)
    assert any(":no_pw-data-loop_thread" in r for r in check(host)["reasons"])
    host = Host()
    host.pids["pipewire-pulse"] = ""
    record = check(host)
    assert record["verdict"] == "BLOCKED" and "host_audio:pipewire-pulse:not_running" in record["reasons"]


# ---- 2. package maintenance ------------------------------------------------------
def test_apt_or_a_maintenance_service_busy_waits_then_blocks() -> None:
    for processes, services, needle in (
            (IDLE + "  4242 dpkg\n", "inactive\ninactive\n", "host_maintenance:dpkg_running (pid 4242)"),
            (IDLE + "  4243 apt-get\n", "inactive\ninactive\n", "host_maintenance:apt-get_running"),
            (IDLE + "  4244 unattended-upgr\n", "inactive\ninactive\n", "host_maintenance:unattended-upgr_running"),
            (IDLE + "  4245 packagekitd\n", "inactive\ninactive\n", "host_maintenance:packagekitd_running"),
            (IDLE, "active\ninactive\n", "host_maintenance:apt-daily.service_active"),
            (IDLE, "inactive\nactivating\n", "host_maintenance:apt-daily-upgrade.service_activating")):
        host = Host(processes=processes, services=services)
        state, now, sleep = clock()
        record = hp.preflight(host, 1800, now=now, sleep=sleep, maintenance_wait_s=60)
        # It waited the whole bound (polling, never longer) and is still busy: BLOCKED.
        assert record["verdict"] == "BLOCKED" and any(needle in r for r in record["reasons"]), record["reasons"]
        assert sum(state["slept"]) == 60 and max(state["slept"]) <= hp.POLL_S, state["slept"]
        assert record["package_maintenance"]["waited_s"] == 60


def test_maintenance_that_finishes_inside_the_wait_lets_the_stage_run() -> None:
    host = Host()
    host.processes = [IDLE + "  4242 dpkg\n", IDLE + "  4242 dpkg\n", IDLE]  # busy twice, then idle
    state, now, sleep = clock()
    record = hp.preflight(host, 1800, now=now, sleep=sleep, maintenance_wait_s=600)
    assert record["verdict"] == "OK" and state["slept"] == [10.0, 10.0], (record["reasons"], state)
    assert record["package_maintenance"]["waited_s"] == 20.0


def test_zero_wait_blocks_at_once_and_a_stop_ends_the_wait() -> None:
    busy = IDLE + "  4242 dpkg\n"
    state, now, sleep = clock()
    record = hp.preflight(Host(processes=busy), 1800, now=now, sleep=sleep, maintenance_wait_s=0)
    assert record["verdict"] == "BLOCKED" and state["slept"] == []
    record = hp.preflight(Host(processes=busy), 1800, now=now, sleep=lambda s: False, maintenance_wait_s=600)
    assert "host_maintenance:wait_interrupted" in record["reasons"]


def test_an_unreadable_maintenance_state_blocks_without_waiting() -> None:
    host = Host()

    def run(argv):
        return hp.Output(None) if argv[:3] == ["ps", "-e", "-o"] else host(argv)
    state, now, sleep = clock()
    record = hp.preflight(run, 1800, now=now, sleep=sleep, maintenance_wait_s=600)
    assert record["verdict"] == "BLOCKED" and state["slept"] == []
    assert any("host_maintenance:unverifiable" in r for r in record["reasons"])


# ---- 3. apt timers ---------------------------------------------------------------
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


def test_timer_elapse_parsing_and_failure_modes() -> None:
    assert hp.parse_elapse("NextElapseUSecRealtime=Wed 2026-11-18 09:00:00 UTC\n") == \
        ("time", datetime(2026, 11, 18, 9, tzinfo=timezone.utc).timestamp())
    assert hp.parse_elapse("NextElapseUSecRealtime=\n") == ("none", None)
    assert hp.parse_elapse("NextElapseUSecRealtime=n/a\n")[0] == "none"
    assert hp.parse_elapse("NextElapseUSecRealtime=Wed 2026-11-18 04:00:00 EST\n")[0] == "unparsable"
    assert check(Host(timers=(None, None)))["verdict"] == "OK"  # no next elapse: it cannot fire
    host = Host()
    record = check(lambda argv: hp.Output(0, "NextElapseUSecRealtime=soon\n")
                   if "NextElapseUSecRealtime" in " ".join(argv) else host(argv))
    assert record["verdict"] == "BLOCKED" and "host_maintenance:apt-daily.timer:next_elapse_unparsable" in record["reasons"]


# ---- informational PipeWire ERR counters -------------------------------------------
def test_pw_top_err_counters_are_recorded_and_never_gate() -> None:
    record = check(Host(), receiver="/opt/aa/aa-wired-receiver")
    start = record["pipewire_err_counters"]["start"]
    assert record["verdict"] == "OK"
    assert [(r["name"], r["err"]) for r in start["receiver_streams"]] == [("aa-wired-receiver", 3)]
    assert [(r["name"], r["err"]) for r in start["sinks"]] == \
        [("alsa_output.platform-sound.analog-stereo", 0)]
    assert len(start["nodes"]) == 3, start["nodes"]  # the last table only
    # An unreadable pw-top is recorded and still does not block.
    host = Host()
    record = check(lambda argv: hp.Output(1, "") if "pw-top" in " ".join(argv) else host(argv))
    assert record["verdict"] == "OK" and record["pipewire_err_counters"]["start"]["error"] == "pw-top failed"
    # Even a huge ERR count gates nothing.
    big = PW_TOP.replace("   3 S16LE", "9999 S16LE")
    assert check(Host(pw_top=big))["verdict"] == "OK"


def test_an_unreadable_user_blocks() -> None:
    record = check(lambda argv: hp.Output(None))
    assert record["verdict"] == "BLOCKED" and record["reasons"] == ["host_audio:unverifiable (id -u failed)"]


if __name__ == "__main__":
    tests = [value for name, value in list(globals().items()) if name.startswith("test_")]
    for test in tests:
        test()
    print(f"PASS {len(tests)} host preflight tests")
