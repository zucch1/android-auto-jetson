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
  stopfail (PR #36 F5/F6): a rejected/lost stop - the receiver stops answering
                mid-projection (SIGSTOP fault injection, no production seam
                touched) - exposes stop_failed plus the TYPED error through
                the F6 status, and F5 must issue no second remote
                StartProjection (proven by the receiver's drained call log).
  stopreject (PR #36 gate): the shipped-executable REJECTED-stop case - the
                test-only scripted-receiver MODE (aa-test-receiver --mode
                reject-first-stop) rejects the FIRST StopProjection with a
                typed error and still reports the same consumer projecting on
                GetState: F6 exposes stop_failed plus the TYPED rejection in
                status/stderr with the decode helper retained, F5 must issue
                no second remote StartProjection, and the accepted stop retry
                lands idle with the widget error cleared.
"""

import argparse
import json
import os
import select
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import threading
import time
from contextlib import ExitStack

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


def start_receiver(args, runtime, mode=None):
    video = os.path.join(runtime, "android-auto-receiver", "video.sock")
    input_sock = os.path.join(runtime, "android-auto-receiver", "input.sock")
    argv = [args.receiver, "--video-socket", video, "--input-socket", input_sock,
            "--fixture", args.fixture]
    if mode is not None:
        argv += ["--mode", mode]
    proc = Driver(argv)
    try:
        proc.await_event("ready")
    except AssertionError:
        proc.stop(False)
        raise
    return proc, video, input_sock


def drain_stderr(driver):
    """Whatever the process has written to stderr so far (non-blocking)."""
    fd = driver.proc.stderr.fileno()
    chunks = []
    while True:
        ready, _, _ = select.select([fd], [], [], 0)
        if not ready:
            break
        chunk = os.read(fd, 4096)
        if not chunk:
            break
        chunks.append(chunk.decode("utf-8", errors="replace"))
    return "".join(chunks)


def start_display(args, video, input_sock, autostart, env):
    argv = [args.display, "--test-only-control-stdin", "--video-socket", video,
            "--input-socket", input_sock]
    if autostart:
        argv.append("--test-only-autostart")
    proc = Driver(argv, env=env)
    try:
        proc.await_event("ready")
    except AssertionError:
        proc.stop(False)
        raise
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


def case_stopfail(args):
    """PR #36 F5/F6: F6 exposes stop_failed + typed error; F5 sends no
    second remote StartProjection.

    The receiver process is frozen with SIGSTOP so the shipped display's real
    StopProjection/GetState exchanges hit the real read timeout - the exact
    never-replying-receiver shape, with no production code seam. SIGCONT at
    the end drains the receiver's queue so its event log proves what did (and
    did not) reach the remote side.
    """
    runtime = tempfile.mkdtemp(prefix="aa-exec-stopfail-")
    try:
        receiver, video, input_sock = start_receiver(args, runtime)
        display = start_display(args, video, input_sock, False, os.environ.copy())
        idle = display.await_status(lambda s: s.get("state") == "idle")
        assert idle["helper_started"] is False, idle

        display.send("start")
        display.await_status(lambda s: s.get("state") == "projecting")
        render = display.await_status(
            lambda s: s.get("frames_presented") == 90
            and s.get("render_ok") is True
            and s.get("render_width") == 1280, timeout=30)
        assert render["state"] == "projecting", render

        # Freeze the receiver: from here the display's contract calls get no
        # reply at all (the BusIo read timeout is the production behavior).
        os.kill(receiver.proc.pid, signal.SIGSTOP)

        # When: F6 requests a stop the receiver never answers.
        display.send("stop")

        # Then: the shipped status exposes stop_failed and the TYPED error
        # (the real read timeout), exactly as the wiring reports it.
        failed = display.await_status(
            lambda s: s.get("state") == "stop_failed"
            and s.get("last_error") == "stop_failed: StopProjection failed (transport_io)",
            timeout=30)
        assert failed["helper_started"] is True, failed

        # When: F5 requests a start while inactivity is unconfirmed.
        display.send("start")

        # Then: the start is rejected LOCALLY (typed illegal-transition, not a
        # remote timeout) and the state stays stop_failed.
        rejected = display.await_status(
            lambda s: s.get("state") == "stop_failed"
            and s.get("last_error")
            == "stop_failed: StartProjection failed (session_illegal_transition)",
            timeout=30)
        assert rejected["state"] == "stop_failed", rejected

        # When: the receiver is resumed and drains its queue.
        os.kill(receiver.proc.pid, signal.SIGCONT)

        # Then: the drained log shows the frozen StopProjection landing - and
        # NEVER a second remote StartProjection (projection is observed
        # exactly once, at the original F5).
        receiver.await_output(
            lambda o: o.get("event") == "state" and o.get("projection") == "stopped",
            timeout=30)
        with receiver.lock:
            states = [o for o in receiver.events if o.get("event") == "state"]
        projected = [o for o in states if o.get("projection") == "projecting"]
        assert len(projected) == 1, states
        assert display.stop() == 0
        assert receiver.stop(False) == 0
    finally:
        # Never leave a SIGSTOPped receiver behind on a failed assertion.
        if "receiver" in locals() and receiver.proc.poll() is None:
            try:
                os.kill(receiver.proc.pid, signal.SIGCONT)
                receiver.stop(False)
            except (ProcessLookupError, OSError):
                pass
        if "display" in locals():
            display.stop()
        shutil.rmtree(runtime, ignore_errors=True)


def case_stopreject(args):
    """PR #36 gate: the shipped-executable REJECTED-stop case.

    The test-only scripted-receiver MODE (aa-test-receiver --mode
    reject-first-stop) answers the wire contract like the
    DummyDisplayStopRecovery controller tests: the FIRST StopProjection is
    REJECTED with a typed error while GetState keeps reporting the same
    consumer still projecting, and the stop retry is accepted (GetState then
    reports stopped). No production code seam is touched.
    """
    runtime = tempfile.mkdtemp(prefix="aa-exec-stopreject-")
    try:
        receiver, video, input_sock = start_receiver(args, runtime,
                                                     mode="reject-first-stop")
        display = start_display(args, video, input_sock, False, os.environ.copy())
        idle = display.await_status(lambda s: s.get("state") == "idle")
        assert idle["helper_started"] is False, idle

        display.send("start")
        display.await_status(lambda s: s.get("state") == "projecting")
        render = display.await_status(
            lambda s: s.get("frames_presented") == 90
            and s.get("render_ok") is True
            and s.get("render_width") == 1280, timeout=30)
        assert render["state"] == "projecting", render

        # When: F6 requests a stop the scripted receiver REJECTS with a typed
        # error while GetState still reports the same consumer projecting.
        display.send("stop")

        # Then: the shipped status exposes stop_failed and the TYPED
        # rejection, in status AND stderr, with the decode helper retained.
        failed = display.await_status(
            lambda s: s.get("state") == "stop_failed"
            and s.get("last_error")
            == "stop_failed: StopProjection failed (session_illegal_transition)",
            timeout=30)
        assert failed["helper_started"] is True, failed
        assert failed["frames_presented"] == 90, failed
        assert "stop_failed: StopProjection failed (session_illegal_transition)"             in drain_stderr(display), "typed rejection missing on stderr"
        # The one reconcile GetState answered "same consumer still projecting"
        # (a different/zero consumer would have reconciled to unavailable).
        receiver.await_output(
            lambda o: o.get("event") == "call" and o.get("member") == "GetState"
            and o.get("projection") == "projecting" and o.get("consumer") == 1,
            timeout=30)

        # When: F5 requests a start while inactivity is unconfirmed.
        display.send("start")

        # Then: the start is rejected LOCALLY (typed illegal-transition, not a
        # remote call) and the state stays stop_failed.
        rejected = display.await_status(
            lambda s: s.get("state") == "stop_failed"
            and s.get("last_error")
            == "stop_failed: StartProjection failed (session_illegal_transition)",
            timeout=30)
        assert rejected["state"] == "stop_failed", rejected

        # Then: EXACTLY ONE remote StartProjection reached the receiver (the
        # original F5 - never a second one from the rejected retry).
        with receiver.lock:
            starts = [o for o in receiver.events
                      if o.get("event") == "call" and o.get("member") == "StartProjection"]
        assert len(starts) == 1, starts

        # When: the stop retry is accepted (second StopProjection succeeds and
        # GetState reports stopped).
        display.send("stop")

        # Then: the display lands idle with the widget error cleared.
        recovered = display.await_status(
            lambda s: s.get("state") == "idle" and s.get("last_error") == ""
            and s.get("frames_presented") == 90, timeout=30)
        assert recovered["helper_started"] is True, recovered
        receiver.await_output(
            lambda o: o.get("event") == "state" and o.get("projection") == "stopped",
            timeout=30)

        # The drained call log: both stops reached the receiver, one start.
        with receiver.lock:
            calls = [o.get("member") for o in receiver.events if o.get("event") == "call"]
        assert calls.count("StartProjection") == 1, calls
        assert calls.count("StopProjection") == 2, calls

        assert display.stop() == 0
        assert receiver.stop(False) == 0
    finally:
        if "display" in locals():
            display.stop()
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


def case_diagnostics(args: argparse.Namespace) -> None:
    """Bound bootstrap errors and give each actual factory attempt fresh credit."""
    with tempfile.TemporaryDirectory(prefix="aa-exec-diagnostics-") as runtime, ExitStack() as children:
        env = os.environ.copy()
        env["XDG_RUNTIME_DIR"] = runtime
        video = os.path.join(runtime, "missing-video.sock")
        input_sock = os.path.join(runtime, "missing-input.sock")
        if args.case != "diagnostics-bootstrap":
            receiver, _, input_sock = start_receiver(args, runtime)
            children.callback(receiver.stop, False)
        if args.case == "diagnostics-factory":
            plugins = os.path.join(runtime, "empty-plugins")
            os.mkdir(plugins)
            env.update(GST_PLUGIN_SYSTEM_PATH_1_0=plugins, GST_PLUGIN_PATH_1_0=plugins,
                       GST_REGISTRY_1_0=os.path.join(runtime, "registry.bin"))
        display = start_display(args, video, input_sock, False, env)
        children.callback(display.stop)
        if args.case == "diagnostics-bootstrap":
            # Given no receiver, no request can invoke the helper factory.
            for command in ["start"] * 8 + ["start", "stop"] * 16:
                display.send(command)
            display.send("status")
            status = display.await_event("status", timeout=10)
            assert status["state"] == "unavailable" and not status["helper_started"], status
            assert display.stop() == 0
            errors = [event for event in display.events if event.get("event") == "display_error"]
            print(f"bootstrap commands=40 errors={len(errors)}", flush=True)
            assert 1 <= len(errors) <= 4, errors
        else:
            code = ("channel_decode_unavailable" if args.case == "diagnostics-factory"
                    else "transport_unavailable")
            for attempt in range(2):
                # When the same failure occurs in two distinct factory attempts.
                with display.lock:
                    offset = len(display.events)
                display.send("start")
                display.send("status")
                status = display.await_event("status", timeout=15)
                assert status["state"] == "idle" and not status["helper_started"], status
                assert code in status["last_error"], status
                with display.lock:
                    errors = [event for event in display.events[offset:]
                              if event.get("event") == "display_error"]
                print(f"{args.case} attempt={attempt + 1} errors={len(errors)}", flush=True)
                assert errors == [{"event": "display_error", "stage": "start", "code": code}], errors
                receiver.await_output(lambda event: event.get("event") == "state"
                                      and event.get("projection") == "stopped", timeout=10)
            assert display.stop() == 0
            assert receiver.stop(False) == 0
            assert not [event for event in receiver.events
                        if event.get("event") == "video_accepted"], receiver.events
        for event in errors:
            assert set(event) == {"event", "stage", "code"}, event


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--display", required=True)
    parser.add_argument("--receiver", required=True)
    parser.add_argument("--fixture", required=True)
    parser.add_argument("--case", required=True,
                        choices=("replay", "flow", "absent", "stalled", "stopfail",
                                 "stopreject", "diagnostics-bootstrap", "diagnostics-factory",
                                 "diagnostics-connect"))
    args = parser.parse_args()
    {"replay": case_replay, "flow": case_flow, "absent": case_absent,
     "stalled": case_stalled, "stopfail": case_stopfail,
     "stopreject": case_stopreject, "diagnostics-bootstrap": case_diagnostics,
     "diagnostics-factory": case_diagnostics, "diagnostics-connect": case_diagnostics}[args.case](args)
    print(f"PASS: {args.case}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
