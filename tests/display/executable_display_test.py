#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Executable-level QA for the SHIPPED aa-dummy-display (task 24 gate round 2).

Cases:
  replay  (F1): the shipped binary's real event loop pumps and renders the
                replay fixture - the pump timer must live across app.exec().
  flow    (F4): start projection -> render -> input -> stop against the
                runnable aa-test-receiver over a private bus (also F2: the
                D-Bus state transitions run WITHOUT the autostart flag).
  absent  (F5): receiver-absent runs (with and without the autostart flag)
                assert no helper process/request and no AP/network change
                via PATH-shim recorder + route snapshot sentinels.
  stalled (gate r3 F1): an UNPACED 401-key burst against a same-UID input
                endpoint that accepts but never reads must never starve the
                GUI - a fresh status reply and F6 stop completion must both
                land within their deadlines during the burst, with the typed
                backpressure result and the delivery-failure count asserted.
"""

import json
import os
import select
import shutil
import socket
import subprocess
import sys
import tempfile
import threading
import time

TIMEOUT = 120.0
HELPER_STUBS = ("aa-system-helper", "nmcli", "iw", "busctl", "ip", "hostapd", "dnsmasq")
ROUTE_SNAPSHOT = ("/proc/net/route", "/proc/net/ipv6_route", "/proc/net/dev")


class Driver:
    def __init__(self, argv, env=None):
        self.proc = subprocess.Popen(argv, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                     stderr=subprocess.PIPE, text=True, bufsize=1, env=env)
        self.events = []
        self.taken = set()
        self.lock = threading.Lock()
        self.reader = threading.Thread(target=self._pump, daemon=True)
        self.reader.start()

    def _pump(self):
        for line in self.proc.stdout:
            try:
                obj = json.loads(line.rstrip("\n"))
            except ValueError:
                continue
            with self.lock:
                self.events.append(obj)

    def send(self, command):
        self.proc.stdin.write(command + "\n")
        self.proc.stdin.flush()

    def await_output(self, predicate, timeout=TIMEOUT):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            with self.lock:
                events = list(self.events)
            for index, obj in enumerate(events):
                if index in self.taken:
                    continue
                if predicate(obj):
                    self.taken.add(index)
                    return obj
            time.sleep(0.05)
        raise AssertionError("timed out waiting for output")

    def await_event(self, kind, timeout=TIMEOUT):
        return self.await_output(lambda o: o.get("event") == kind, timeout)

    def await_status(self, predicate, timeout=TIMEOUT):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            self.send("status")
            try:
                return self.await_output(
                    lambda o: o.get("event") == "status" and predicate(o), 2.0)
            except AssertionError:
                continue
        raise AssertionError("timed out waiting for status predicate")

    def stop(self, quit_command=True):
        if self.proc.poll() is None:
            if quit_command:
                try:
                    self.send("quit")
                except (BrokenPipeError, OSError):
                    pass
            else:
                self.proc.terminate()
        try:
            self.proc.wait(timeout=15)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            self.proc.wait(timeout=10)
        self.reader.join(timeout=5)
        return self.proc.returncode


def route_snapshot():
    # Routing tables plus interface NAMES only: /proc/net/dev byte counters
    # churn with unrelated host traffic and would false-positive.
    out = {}
    for path in ROUTE_SNAPSHOT[:2]:
        with open(path, "rb") as handle:
            out[path] = handle.read()
    with open(ROUTE_SNAPSHOT[2], "r", encoding="utf-8") as handle:
        names = [line.split(":")[0].strip() for line in handle if ":" in line]
    out[ROUTE_SNAPSHOT[2]] = tuple(names)
    return out


def make_path_shim(directory):
    os.makedirs(directory, exist_ok=True)
    record = os.path.join(directory, "invocations.log")
    for name in HELPER_STUBS:
        stub = os.path.join(directory, name)
        with open(stub, "w", encoding="utf-8") as handle:
            handle.write(f'#!/bin/sh\necho "$0 $@" >> "{record}"\nexit 127\n')
        os.chmod(stub, 0o755)
    return record


def start_receiver(args, runtime):
    video = os.path.join(runtime, "android-auto-receiver", "video.sock")
    input_sock = os.path.join(runtime, "android-auto-receiver", "input.sock")
    proc = Driver([args.receiver, "--video-socket", video, "--input-socket", input_sock,
                   "--fixture", args.fixture])
    proc.await_event("ready")
    return proc, video, input_sock


def start_display(args, video, input_sock, autostart, env):
    argv = [args.display, "--test-only-control-stdin", "--video-socket", video,
            "--input-socket", input_sock]
    if autostart:
        argv.append("--test-only-autostart")
    proc = Driver(argv, env=env)
    proc.await_event("ready")
    return proc


def case_replay(args):
    """F1: the shipped event loop pumps and renders without any manual pump."""
    runtime = tempfile.mkdtemp(prefix="aa-exec-replay-")
    try:
        receiver, video, input_sock = start_receiver(args, runtime)
        display = start_display(args, video, input_sock, True, os.environ.copy())
        status = display.await_status(lambda s: s.get("frames_presented") == 90)
        assert status["state"] == "projecting", status
        assert status["helper_started"] is True, status
        assert status["render_ok"] is True and status["render_width"] == 1280, status
        assert display.stop() == 0
        assert receiver.stop(False) == 0
    finally:
        shutil.rmtree(runtime, ignore_errors=True)


def case_flow(args):
    """F4/F2: start -> render -> input -> stop against the runnable receiver."""
    runtime = tempfile.mkdtemp(prefix="aa-exec-flow-")
    try:
        receiver, video, input_sock = start_receiver(args, runtime)
        display = start_display(args, video, input_sock, False, os.environ.copy())
        idle = display.await_status(lambda s: s.get("state") == "idle")
        assert idle["helper_started"] is False, idle

        display.send("start")
        display.await_status(lambda s: s.get("state") == "projecting")
        receiver.await_output(lambda o: o.get("event") == "state"
                              and o.get("projection") == "projecting")

        display.await_status(lambda s: s.get("frames_presented") == 90)
        assert receiver.await_output(lambda o: o.get("event") == "video_sent"
                                     and o.get("units") == 90)["units"] == 90

        display.send("key up press")
        display.send("key up release")
        display.send("key a press")
        display.send("touch down 10 20")
        display.send("touch move 40 50")
        display.send("touch up 40 50")
        expected = [
            {"kind": "key", "keycode": 19, "pressed": True},
            {"kind": "key", "keycode": 19, "pressed": False},
            {"kind": "key", "keycode": 29, "pressed": True},
            {"kind": "touch", "x": 10, "y": 20, "action": "down"},
            {"kind": "touch", "x": 40, "y": 50, "action": "move"},
            {"kind": "touch", "x": 40, "y": 50, "action": "up"},
        ]
        for wanted in expected:
            got = receiver.await_output(lambda o: o.get("event") == "input")
            for key, value in wanted.items():
                assert got.get(key) == value, (wanted, got)

        display.send("stop")
        display.await_status(lambda s: s.get("state") == "idle")
        receiver.await_output(lambda o: o.get("event") == "state"
                              and o.get("projection") == "stopped")
        assert display.stop() == 0
        assert receiver.stop(False) == 0
    finally:
        shutil.rmtree(runtime, ignore_errors=True)


def case_absent(args):
    """F5: receiver absent - no helper process/request, no AP/network op."""
    runtime = tempfile.mkdtemp(prefix="aa-exec-absent-")
    try:
        shim = os.path.join(runtime, "shim")
        record = make_path_shim(shim)
        before = route_snapshot()
        for autostart in (False, True):
            env = os.environ.copy()
            env["PATH"] = shim + os.pathsep + env.get("PATH", "")
            display = start_display(args, os.path.join(runtime, "absent-video.sock"),
                                    os.path.join(runtime, "absent-input.sock"), autostart, env)
            status = display.await_status(lambda s: s.get("state") == "unavailable")
            assert status["helper_started"] is False, status
            assert status["frames_presented"] == 0, status
            assert display.stop() == 0
        assert route_snapshot() == before, "route snapshot changed with no receiver"
        assert not os.path.exists(record) or os.path.getsize(record) == 0, \
            "helper binary was invoked with no receiver"
    finally:
        shutil.rmtree(runtime, ignore_errors=True)


def case_stalled(args):
    """Gate r3 F1: an unpaced 401-key burst never starves status or F6 stop."""
    runtime = tempfile.mkdtemp(prefix="aa-exec-stalled-")
    listener = None
    acceptor = None
    held = []
    stop_accept = threading.Event()
    try:
        # A same-UID SOCK_SEQPACKET endpoint that accepts but never reads.
        stalled = os.path.join(runtime, "stalled-input.sock")
        listener = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)
        listener.bind(stalled)
        listener.listen(16)

        def accept_held():
            listener.settimeout(0.2)
            while not stop_accept.is_set():
                try:
                    conn, _ = listener.accept()
                except (socket.timeout, OSError):
                    continue
                held.append(conn)

        acceptor = threading.Thread(target=accept_held, daemon=True)
        acceptor.start()

        receiver, video, _ = start_receiver(args, runtime)
        display = start_display(args, video, stalled, False, os.environ.copy())
        idle = display.await_status(lambda s: s.get("state") == "idle")
        assert idle["helper_started"] is False, idle

        display.send("start")
        display.await_status(lambda s: s.get("state") == "projecting")

        # Rendering completes while the input endpoint is already stalled.
        render = display.await_status(
            lambda s: s.get("frames_presented") == 90
            and s.get("render_ok") is True
            and s.get("render_width") == 1280, timeout=30)
        assert render["state"] == "projecting", render

        # UNPACED 401-key burst (the gate r3 reproduction size): no pacing and
        # never abandoned at the first drop. The status and F6 stop commands
        # ride the same unpaced stream, so both dispatch while the burst is
        # processed and the endpoint stays stalled.
        for index in range(401):
            display.send("key a press" if index % 2 == 0 else "key a release")
        display.send("status")
        display.send("stop")

        # Fresh status responsiveness during the burst: sent+dropped==401 can
        # only hold once all 401 events were dispatched, so a stale reply
        # cannot match; the render probe proves the render path is alive.
        fresh = display.await_output(
            lambda o: o.get("event") == "status"
            and o.get("input_sent", 0) + o.get("input_dropped", 0) == 401,
            timeout=5.0)
        assert fresh["input_last_error"] == "transport_queue_full", fresh
        assert fresh["input_dropped"] >= 1, fresh
        assert fresh["input_sent"] >= 1, fresh
        assert fresh["render_ok"] is True and fresh["render_width"] == 1280, fresh

        # F6 stop completes and reaches idle within the existing deadline
        # while the endpoint is still stalled (the idle+count predicate
        # cannot match any pre-stop status).
        stopped = display.await_status(
            lambda s: s.get("state") == "idle" and s.get("input_dropped", 0) >= 1,
            timeout=10)
        assert stopped["input_last_error"] == "transport_queue_full", stopped

        assert display.stop() == 0
        assert receiver.stop(False) == 0
    finally:
        stop_accept.set()
        if acceptor is not None:
            acceptor.join(timeout=5)
        for conn in held:
            conn.close()
        if listener is not None:
            listener.close()
        shutil.rmtree(runtime, ignore_errors=True)


def main():
    import argparse
    parser = argparse.ArgumentParser()
    parser.add_argument("--display", required=True)
    parser.add_argument("--receiver", required=True)
    parser.add_argument("--fixture", required=True)
    parser.add_argument("--case", required=True,
                        choices=("replay", "flow", "absent", "stalled"))
    args = parser.parse_args()
    {"replay": case_replay, "flow": case_flow, "absent": case_absent,
     "stalled": case_stalled}[args.case](args)
    print(f"PASS: {args.case}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
