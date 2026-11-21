# SPDX-License-Identifier: GPL-3.0-or-later
# /// script
# requires-python = ">=3.12"
# dependencies = []
# ///
# Run: python3 -B -m unittest discover -s tests/wireless_qa -v
# (its own CTest entry, wireless_qa_transport, beside wired_drive_smoke)
"""Wireless transport for the drive smoke and the soak chain, with fakes only.

No phone, Bluetooth, Wi-Fi, AP or helper is touched: the wireless receiver,
the display, adb, ``ip neigh`` and the Jetson's USB bus (a fake sysfs tree) are
fakes. The fake receiver prints the diagnostic line exactly as
``aa-wireless-receiver`` on main does: the serializer's bare object
(src/diagnostics/Serialize.cpp to_json via tools/wireless-receiver/Media.hpp),
not the ``"event":"diag"`` wrapper the wired receiver (and #163) use.
"""
from __future__ import annotations

import argparse
import json
import os
import signal
import subprocess
import sys
import tempfile
import time
import unittest
import unittest.mock
from datetime import datetime, timezone
from pathlib import Path
from time import sleep as time_sleep

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
sys.path.insert(0, str(ROOT / "tests" / "wired_qa"))  # the drive smoke's fixtures
sys.path.insert(0, str(ROOT / "tools" / "perf"))
sys.path.insert(0, str(ROOT / "tools" / "qualification"))
import test_drive_smoke as base  # noqa: E402
import aa_transport as transport  # noqa: E402
import aa_drive_smoke as smoke  # noqa: E402
import aa_projection_collect as collect  # noqa: E402
import aa_perf_report as report  # noqa: E402
import aa_soak_chain as chain  # noqa: E402

WIFI = ('Wifi is enabled\nWifi is connected to "AP"\nWifiInfo: SSID: "AP", BSSID: 02:00:00:00:00:00, '
        'MAC: 02:00:00:00:00:00, RSSI: -48, Link speed: 433Mbps, Frequency: 5180MHz, Net ID: 3')
PHONE = "FAKESERIAL"
STATION = "10.42.0.23"


def bare_state(session: int, mono_ns: int, state: str) -> str:
    """One session_state_changed line exactly as aa-wireless-receiver on main prints it."""
    return (f'{{"kind":"session_state_changed","session":{session},"mono_ns":{mono_ns},'
            f'"to_state":"{state}","transport":"usb"}}')


def wrapped_state(session: int, mono_ns: int, state: str) -> str:
    """The same line wrapped as the wired receiver (and #163's wireless receiver) prints it."""
    return '{"event":"diag",' + bare_state(session, mono_ns, state)[1:]


# The receiver's Android Auto session as procfs shows it (written by the fake
# receivers): one ESTABLISHED socket per FAKE_SESSION_PEERS address (default the
# station 10.42.0.23; empty: no session socket), plus its listener and an
# established socket of another process that must both be ignored.
FAKE_SESSION = """
def fake_session():
    import socket as _socket, struct as _struct
    root = os.environ.get("FAKE_PROC")
    if not root:
        return
    base = os.path.join(root, str(os.getpid()))
    os.makedirs(os.path.join(base, "fd"))
    os.makedirs(os.path.join(base, "net"))
    hexip = lambda ip: "%08X" % _struct.unpack("=I", _socket.inet_aton(ip))[0]
    row = "   %d: %s:14A5 %s:%s %s 00000000:00000000 00:00000000 00000000  1000        0 %d 1"
    rows = ["  sl  local_address rem_address   st tx_queue rx_queue tr tm->when retrnsmt   uid  timeout inode",
            row % (0, hexip("10.42.0.1"), "00000000", "0000", "0A", 500),
            row % (1, hexip("10.42.0.1"), hexip("10.42.0.77"), "C351", "01", 999)]
    os.symlink("socket:[500]", os.path.join(base, "fd", "3"))
    os.symlink("/dev/null", os.path.join(base, "fd", "0"))
    peers = os.environ.get("FAKE_SESSION_PEERS", "10.42.0.23")
    for index, peer in enumerate(p for p in peers.split(",") if p):
        rows.append(row % (2 + index, hexip("10.42.0.1"), hexip(peer), "C350", "01", 600 + index))
        os.symlink("socket:[%d]" % (600 + index), os.path.join(base, "fd", str(10 + index)))
    with open(os.path.join(base, "net", "tcp"), "w") as handle:
        handle.write("\\n".join(rows) + "\\n")
"""

# Startup as on main: ready, admission, then the bare diagnostic; no "active" line.
FAKE_WIRELESS_RECEIVER = """#!/usr/bin/env python3
import os, subprocess, sys, time
""" + FAKE_SESSION + """
directory = os.environ["FAKE_DIR"]
with open(os.path.join(directory, "receiver.argv"), "w") as handle:
    handle.write("\\n".join(sys.argv[1:]))
grandchild = subprocess.Popen(["sleep", "300"])
with open(os.path.join(directory, "receiver.pids"), "w") as handle:
    handle.write(f"{os.getpid()} {grandchild.pid}")
print('{"event":"ready","state":"waiting_for_consumer"}', flush=True)
time.sleep(0.5)
print('{"event":"wireless_admission"}', flush=True)
if not os.environ.get("FAKE_RECEIVER_DIE_AFTER_ACTIVE"):
    fake_session()
print('{"kind":"session_state_changed","session":1,"mono_ns":%d,"to_state":"active","transport":"usb"}'
      % time.monotonic_ns(), flush=True)
print('{"event":"video_profile","width":1280,"height":720,"fps":30,"codec":"h264"}', flush=True)
if os.environ.get("FAKE_USB_PLUG_AFTER_S"):
    # The operator plugs the phone's cable in during the drive (an AOA device appears).
    time.sleep(float(os.environ["FAKE_USB_PLUG_AFTER_S"]))
    device = os.path.join(os.environ["FAKE_USB"], "1-9")
    os.makedirs(device)
    for name, value in (("idVendor", "18d1"), ("idProduct", "2d01")):
        with open(os.path.join(device, name), "w") as handle:
            handle.write(value + "\\n")
if os.environ.get("FAKE_RECEIVER_DIE_AFTER_ACTIVE"):
    sys.exit(7)
if os.environ.get("FAKE_READMIT"):
    time.sleep(2.0)
    print('{"event":"wireless_admission"}', flush=True)
    print('{"kind":"session_state_changed","session":2,"mono_ns":%d,"to_state":"active","transport":"usb"}'
          % time.monotonic_ns(), flush=True)
time.sleep(300)
"""

# `ip -4 neigh show dev IFACE`: one station on the AP (the MAC is never recorded).
FAKE_IP = """#!/bin/sh
printf '%s\\n' "${FAKE_NEIGH:-10.42.0.23 lladdr 02:00:00:00:00:01 REACHABLE}"
"""

# A display that keeps painting (every 25 ms), so the drive window after the
# phone's preparation really starts and is judged (the base fake paints once).
FAKE_WIRELESS_DISPLAY = """#!/usr/bin/env python3
import os, sys, threading, time
with open(os.path.join(os.environ["FAKE_DIR"], "display.pids"), "w") as handle:
    handle.write(str(os.getpid()))
threading.Thread(target=lambda: [None for _ in sys.stdin], daemon=True).start()
frame = 0
while True:
    frame += 1
    now = time.monotonic_ns()
    print('{"event":"video_presented","frame":%d,"scheduled_ns":%d,"completed_ns":%d,"count":%d}'
          % (frame, now - 30000000, now, frame), flush=True)
    time.sleep(0.025)
"""

# The runner with the Jetson's USB bus, the AP lease file and procfs pointed at
# fakes, and its fixed waits shortened (phone setup pauses, host USB period).
DRIVER = """import os, signal, subprocess, sys, time
from pathlib import Path
sys.path.insert(0, sys.argv[1])
import aa_drive_smoke
aa_drive_smoke.transport.USB_DEVICES = Path(os.environ["FAKE_USB"])
aa_drive_smoke.transport.NM_LEASES = os.environ["FAKE_LEASES"]
aa_drive_smoke.transport.PROC = Path(os.environ["FAKE_PROC"])
aa_drive_smoke.ADB_BIND_TIMEOUT_S = 2.0
aa_drive_smoke.HOST_USB_PERIOD_S = 0.3
_real_sleep = aa_drive_smoke.StopFlag.sleep
aa_drive_smoke.StopFlag.sleep = lambda self, seconds: _real_sleep(self, min(seconds, 0.2))
""" + base.DRIVER.split("\n", 2)[2]


def usb_device(root: Path, name: str, vendor: str, product: str, serial: str | None = None) -> None:
    device = root / name
    device.mkdir(parents=True)
    (device / "idVendor").write_text(vendor + "\n")
    (device / "idProduct").write_text(product + "\n")
    if serial:
        (device / "serial").write_text(serial + "\n")


class TransportUnitTests(unittest.TestCase):
    def test_wifi_status_keeps_only_numbers(self) -> None:
        parsed = transport.parse_wifi_status(WIFI)
        self.assertEqual(parsed, {"connected": True, "frequency_mhz": 5180, "channel": 36,
                                  "rssi_dbm": -48})
        self.assertNotIn("AP", json.dumps(parsed))
        self.assertEqual(transport.parse_wifi_status(None)["rssi_dbm"], None)
        self.assertEqual(transport.parse_wifi_status("RSSI: -127")["rssi_dbm"], None)

    def test_channels(self) -> None:
        for mhz, channel in ((2412, 1), (2484, 14), (5180, 36), (5240, 48), (5955, 1), (1, None)):
            self.assertEqual(transport.frequency_to_channel(mhz), channel)

    def test_a_record_without_transport_is_wired(self) -> None:
        self.assertEqual(transport.record_transport({}), "wired")
        self.assertEqual(transport.record_transport({"transport": "wireless"}), "wireless")

    def test_diag_kind_accepts_the_bare_and_the_wrapped_line(self) -> None:
        # P1-2: main's wireless receiver prints the bare form; #163 wraps it.
        for line in (bare_state(1, 5, "active"), wrapped_state(1, 5, "active")):
            self.assertEqual(transport.diag_kind(json.loads(line)), "session_state_changed", line)
        self.assertIsNone(transport.diag_kind({"event": "video_profile", "kind": "x"}))
        self.assertIsNone(transport.diag_kind({"event": "ready"}))

    def test_host_usb_evidence(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            usb_device(root, "1-1", "0bda", "8153", "OTHER")  # an unrelated device
            clean = transport.host_usb_evidence(PHONE, root)
            self.assertEqual(clean, {"readable": True, "phone_attached": False, "aoa_attached": False})
            self.assertIsNone(transport.host_usb_refusal(clean))
            usb_device(root, "1-2", "04e8", "6860", PHONE)  # the phone, plain mtp
            self.assertEqual(transport.host_usb_refusal(transport.host_usb_evidence(PHONE, root)),
                             "usb_attached:phone_on_usb")
            usb_device(root, "1-3", "18d1", "2d01")  # any AOA device, any port
            self.assertEqual(transport.host_usb_refusal(transport.host_usb_evidence(PHONE, root)),
                             "usb_attached:usb_in_aoa")
        missing = transport.host_usb_evidence(PHONE, Path(directory) / "gone")
        self.assertEqual(transport.host_usb_refusal(missing), "usb_state_unreadable")

    def test_station_addresses_never_return_macs(self) -> None:
        leases = "1763500000 02:00:00:00:00:01 10.42.0.23 phone 01:02:00:00:00:00:01\n"
        neigh = ("10.42.0.23 lladdr 02:00:00:00:00:01 REACHABLE\n"
                 "10.42.0.99 lladdr 02:00:00:00:00:09 STALE\n10.42.0.50 FAILED\n")
        addresses = transport.ap_station_addresses("wlan0", leases, neigh)
        self.assertEqual(addresses, ["10.42.0.23", "10.42.0.99"])
        self.assertNotIn("02:00", json.dumps(addresses))

    def test_the_ap_interface_is_named_never_read_from_ap_conf(self) -> None:
        # Neon P1-1: ap.conf is root:root 0600 (src/helper/nm/Files.cpp): never read.
        self.assertFalse(hasattr(transport, "ap_interface"))
        self.assertFalse(hasattr(transport, "AP_CONF"))
        for name in ("wlan0", "wlP1p1s0", "ap0.1"):
            self.assertTrue(transport.valid_interface(name), name)
        for name in (None, "", "-x", "../etc", "wlan0/x", "a" * 16, ".hidden"):
            self.assertFalse(transport.valid_interface(name), name)
        for tool in (smoke, collect, chain):
            self.assertNotIn("ap.conf\")", Path(tool.__file__).read_text())
        self.assertNotIn("/etc/android-auto-receiver/ap.conf", Path(transport.__file__).read_text())

    def test_bind_adb_by_device_serial(self) -> None:
        calls: list[tuple] = []

        def adb(*args):
            # As the real adb: get-serialno is the host's transport serial, which
            # for TCP is the endpoint; only the device knows its hardware serial.
            calls.append(args)
            target = args[1] if args[0] == "-s" else args[-1]
            if args[0] == "connect":
                return subprocess.CompletedProcess(args, 0, f"connected to {target}\n", "")
            if args[-1] == "get-serialno":
                return subprocess.CompletedProcess(args, 0, target + "\n", "")
            if args[2:] == ("shell", "getprop", "ro.serialno"):
                return subprocess.CompletedProcess(
                    args, 0, (PHONE if target.startswith("10.42.0.23") else "OTHERPHONE") + "\n", "")
            return subprocess.CompletedProcess(args, 0, "", "")
        self.assertEqual(transport.bind_adb(PHONE, ["10.42.0.99", "10.42.0.23"], adb),
                         ("10.42.0.23:5555", None))
        self.assertIn(("disconnect", "10.42.0.99:5555"), calls)  # the other phone is dropped
        self.assertEqual(transport.bind_adb(PHONE, ["10.42.0.99"], adb), (None, "adb_phone_mismatch"))
        self.assertEqual(transport.bind_adb(PHONE, [], adb), (None, "adb_unreachable"))
        refused = lambda *a: subprocess.CompletedProcess(a, 1, "failed to connect to x", "")  # noqa: E731
        self.assertEqual(transport.bind_adb(PHONE, ["10.42.0.23"], refused), (None, "adb_unreachable"))

    def test_session_peers_from_real_procfs(self) -> None:
        # The parser against this kernel's own /proc (byte order, fd links).
        import socket as sock
        with sock.socket() as listener:
            listener.bind(("127.0.0.1", 0))
            listener.listen(1)
            client = sock.create_connection(listener.getsockname())
            server, _ = listener.accept()
            try:
                peers = transport.receiver_session_peers(os.getpid())
            finally:
                client.close()
                server.close()
        self.assertEqual(peers, ["127.0.0.1"])
        self.assertIsNone(transport.receiver_session_peers(2**22 + 12345))  # no such process

    def test_session_peers_ignore_listeners_and_other_processes(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            namespace: dict = {"os": os}
            exec(FAKE_SESSION, namespace)
            with unittest.mock.patch.dict(os.environ, {"FAKE_PROC": directory,
                                                       "FAKE_SESSION_PEERS": "10.42.0.23"}):
                namespace["fake_session"]()
            peers = transport.receiver_session_peers(os.getpid(), Path(directory))
            tcp6 = Path(directory) / str(os.getpid()) / "net" / "tcp6"
            # A v4-mapped IPv6 session (a dual-stack listener) is the same peer.
            mapped = "".join("%08X" % int.from_bytes(bytes(b), sys.byteorder) for b in (
                [0, 0, 0, 0], [0, 0, 0, 0], [0, 0, 0xff, 0xff], [10, 42, 0, 31]))
            tcp6.write_text("  sl  local rem st\n   0: %s:14A5 %s:C350 01 0:0 0:0 0 1000 0 600 1\n"
                            % ("0" * 32, mapped))
            with_tcp6 = transport.receiver_session_peers(os.getpid(), Path(directory))
        self.assertEqual(peers, ["10.42.0.23"])
        self.assertEqual(with_tcp6, ["10.42.0.23", "10.42.0.31"])

    def test_native_ipv6_or_unreadable_tables_fail_closed(self) -> None:
        # Neon P2-6: a native IPv6 session is never dropped, an unreadable
        # IPv4 table is never "no peer", a malformed address is never decoded.
        import socket as sock
        with sock.socket(sock.AF_INET6) as listener:
            try:
                listener.bind(("::1", 0))
            except OSError:
                self.skipTest("no IPv6 loopback on this host")
            listener.listen(2)
            with sock.socket() as v4_listener:
                v4_listener.bind(("127.0.0.1", 0))
                v4_listener.listen(1)
                clients = [sock.create_connection(listener.getsockname()[:2]),
                           sock.create_connection(v4_listener.getsockname())]
                servers = [listener.accept()[0], v4_listener.accept()[0]]
                try:
                    peers = transport.receiver_session_peers(os.getpid())
                finally:
                    for handle in clients + servers:
                        handle.close()
        self.assertIn("::1", peers)
        self.assertIn("127.0.0.1", peers)
        self.assertEqual(transport.session_candidate(["::1"], ["10.42.0.23"]),
                         (None, "adb_session_ambiguous"))
        self.assertEqual(transport.session_candidate(["10.42.0.23", "::1"], ["10.42.0.23"]),
                         (None, "adb_session_ambiguous"))
        with tempfile.TemporaryDirectory() as directory:
            namespace: dict = {"os": os}
            exec(FAKE_SESSION, namespace)
            with unittest.mock.patch.dict(os.environ, {"FAKE_PROC": directory}):
                namespace["fake_session"]()
            net = Path(directory) / str(os.getpid()) / "net"
            (net / "tcp6").write_text("  sl  local rem st\n")
            (net / "tcp").rename(net / "tcp.away")
            self.assertIsNone(transport.receiver_session_peers(os.getpid(), Path(directory)))
            (net / "tcp").mkdir()  # present but unreadable as a file
            self.assertIsNone(transport.receiver_session_peers(os.getpid(), Path(directory)))
        self.assertIsNone(transport._proc_address("01:14A5"))
        self.assertIsNone(transport._proc_address("0100007F00:14A5"))
        self.assertEqual(transport._proc_address("0100007F:14A5" if sys.byteorder == "little"
                                                 else "7F000001:14A5"), "127.0.0.1")

    def test_adb_connect_output_must_say_connected(self) -> None:
        # Neon P3-10: "disconnected"/"failed to authenticate" are no connection,
        # and the failed transport is dropped again.
        for answer in ("failed to authenticate to 10.42.0.23:5555", "disconnected 10.42.0.23:5555",
                       "not connected", "cannot connect to 10.42.0.23:5555: refused"):
            calls: list[tuple] = []

            def adb(*args, answer=answer):
                calls.append(args)
                return subprocess.CompletedProcess(args, 0, answer + "\n", "")
            self.assertEqual(transport.bind_adb(PHONE, ["10.42.0.23"], adb), (None, "adb_unreachable"),
                             answer)
            self.assertEqual(calls, [("connect", "10.42.0.23:5555"), ("disconnect", "10.42.0.23:5555")],
                             answer)

    def test_session_candidate(self) -> None:
        stations = ["10.42.0.23", "10.42.0.99"]
        self.assertEqual(transport.session_candidate(["10.42.0.99"], stations), ("10.42.0.99", None))
        self.assertEqual(transport.session_candidate(None, stations), (None, "adb_session_unreadable"))
        self.assertEqual(transport.session_candidate([], stations), (None, "adb_session_peer_unknown"))
        self.assertEqual(transport.session_candidate(stations, stations), (None, "adb_session_ambiguous"))
        self.assertEqual(transport.session_candidate(["192.168.1.5"], stations),
                         (None, "adb_session_not_on_ap"))


class AnalyzerWirelessDropTests(unittest.TestCase):
    """The drive smoke's no_session_drop sees wireless drops in either line shape."""

    def analyze(self, extra: list[str], stamped: list[dict] | None = None,
                shape=bare_state) -> dict:
        run = base.Run(duration=60)
        run.receiver = [line for line in run.receiver if line != {"event": "active"}]
        run.receiver.insert(2, json.loads(shape(1, base.ACTIVE, "active")))
        run.receiver += [json.loads(line) for line in extra]
        run.run["transport"] = "wireless"
        run.stamped += stamped or []
        with tempfile.TemporaryDirectory() as directory:
            result = smoke.analyze_dir(run.write(Path(directory)))
        return base.criterion(result, "no_session_drop")

    def test_clean_wireless_session_has_no_drop(self) -> None:
        for shape in (bare_state, wrapped_state):
            self.assertTrue(self.analyze([], shape=shape)["passed"], shape)

    def test_bare_disconnect_inside_the_window_is_a_drop(self) -> None:
        # P1-2: on 4a80062 the bare line (no "event":"diag") was invisible here.
        for shape in (bare_state, wrapped_state):
            drop = self.analyze([shape(1, base.ACTIVE + 20 * base.S, "disconnected")], shape=shape)
            self.assertFalse(drop["passed"], shape)
            self.assertEqual(drop["disconnects"], 1)

    def test_reconnecting_and_a_new_session_id_are_drops(self) -> None:
        self.assertFalse(self.analyze([bare_state(1, base.ACTIVE + 9 * base.S, "reconnecting")])["passed"])
        self.assertFalse(self.analyze([bare_state(2, base.ACTIVE + 9 * base.S, "active")])["passed"])

    def test_a_second_admission_is_a_drop(self) -> None:
        readmitted = [{"observed_ns": base.ACTIVE + 15 * base.S, "log": "receiver",
                       "event": {"event": "wireless_admission"}}]
        self.assertFalse(self.analyze([], stamped=readmitted)["passed"])
        expired = [{"observed_ns": base.ACTIVE + 15 * base.S, "log": "receiver",
                    "event": {"event": "wireless_admission_expired"}}]
        self.assertFalse(self.analyze([], stamped=expired)["passed"])

    def test_wired_keeps_its_own_rule(self) -> None:
        # Wired is unchanged: only a wrapped disconnect counts there.
        run = base.Run(duration=60)
        run.receiver.append(json.loads(wrapped_state(1, base.ACTIVE + 9 * base.S, "reconnecting")))
        with tempfile.TemporaryDirectory() as directory:
            drop = base.criterion(smoke.analyze_dir(run.write(Path(directory))), "no_session_drop")
        self.assertTrue(drop["passed"])


class GateTransportTests(unittest.TestCase):
    """A wired smoke never qualifies a wireless soak, nor the reverse; wireless binds the phone id."""

    def record(self, **fields) -> dict:
        record = {"schema": smoke.SCHEMA, "verdict": "PASS", "duration_s": 600,
                  "finished_utc": datetime.now(timezone.utc).isoformat(), "video_profiles": None,
                  "phone_serial": "SER", "configuration": dict(smoke.DEFAULT_CONFIGURATION),
                  "binaries": {"receiver": {"sha256": "a" * 64}, "display": {"sha256": "b" * 64}}}
        record.update(fields)
        return record

    def check(self, record: dict, name: str, phone_id: int | None = None,
              ap_interface: str | None = None) -> str | None:
        if name == "wireless" and ap_interface is None:
            ap_interface = "wlan0"
        return smoke.check_record(record, receiver_sha256="a" * 64, display_sha256="b" * 64,
                                  phone_serial="SER", now=datetime.now(timezone.utc),
                                  transport_name=name, phone_id=phone_id,
                                  ap_interface=ap_interface)[0]

    CLEAN = {"readable": True, "phone_attached": False, "aoa_attached": False}

    def link(self, **fields) -> dict:
        link = {"adb_bound": True, "before": None, "after": None,
                "host_usb": {"before": self.CLEAN, "during": [self.CLEAN], "after": self.CLEAN}}
        link.update(fields)
        return link

    def wireless(self, **fields) -> dict:
        return self.record(**{"transport": "wireless", "phone_id": 1, "ap_interface": "wlan0",
                              "ap_channel": 36, "link": self.link(), **fields})

    def test_matching_transport_admits(self) -> None:
        self.assertIsNone(self.check(self.wireless(), "wireless", 1))
        self.assertIsNone(self.check(self.record(transport="wired"), "wired"))
        self.assertIsNone(self.check(self.record(), "wired"))  # legacy wired: no link needed

    def test_wireless_pass_needs_bound_cable_out_link_evidence(self) -> None:
        dirty = dict(self.CLEAN, phone_attached=True)
        aoa = dict(self.CLEAN, aoa_attached=True)
        unreadable = {"readable": False, "phone_attached": None, "aoa_attached": None}
        cases = {
            "no link": self.record(transport="wireless", phone_id=1, ap_interface="wlan0",
                                   ap_channel=36),
            "link not an object": self.wireless(link=[]),
            "adb unbound": self.wireless(link=self.link(adb_bound=False)),
            "adb_bound not a bool": self.wireless(link=self.link(adb_bound="yes")),
            "no host_usb": self.wireless(link=self.link(host_usb=None)),
            "no samples during": self.wireless(link=self.link(
                host_usb={"before": self.CLEAN, "during": [], "after": self.CLEAN})),
            "phone on USB before": self.wireless(link=self.link(
                host_usb={"before": dirty, "during": [self.CLEAN], "after": self.CLEAN})),
            "AOA during": self.wireless(link=self.link(
                host_usb={"before": self.CLEAN, "during": [self.CLEAN, aoa], "after": self.CLEAN})),
            "unreadable after": self.wireless(link=self.link(
                host_usb={"before": self.CLEAN, "during": [self.CLEAN], "after": unreadable})),
            "after missing": self.wireless(link=self.link(
                host_usb={"before": self.CLEAN, "during": [self.CLEAN]})),
        }
        for name, record in cases.items():
            self.assertEqual(self.check(record, "wireless", 1), "drive_smoke_link_unproven", name)

    def test_cross_transport_is_refused(self) -> None:
        self.assertEqual(self.check(self.record(transport="wired"), "wireless", 1),
                         "drive_smoke_transport_mismatch")
        self.assertEqual(self.check(self.record(transport="wireless", phone_id=1), "wired"),
                         "drive_smoke_transport_mismatch")
        # A legacy record (no field) is wired, so it never qualifies wireless.
        self.assertEqual(self.check(self.record(), "wireless", 1), "drive_smoke_transport_mismatch")
        self.assertIsNone(self.check(self.record(), "wired"))

    def test_wireless_binds_the_approved_phone_id(self) -> None:
        # P2-2: a smoke on phone id 1 never qualifies a soak on phone id 2.
        self.assertEqual(self.check(self.wireless(), "wireless", 2), "drive_smoke_phone_mismatch")
        self.assertEqual(self.check(self.wireless(), "wireless", None), "drive_smoke_phone_mismatch")
        self.assertEqual(self.check(self.record(transport="wireless"), "wireless", 1),
                         "drive_smoke_malformed")

    def test_wireless_harness_flags_are_not_configuration(self) -> None:
        config = smoke.configuration(
            ["rx", "--store-dir", "/s", "--phone-id", "3", "--diagnostics", "--video-socket", "/v",
             "--input-socket", "/i", "--duration-ms", "1"], ["d", "--test-only-autostart"])
        self.assertEqual(config, smoke.DEFAULT_CONFIGURATION)


class WirelessRunnerTests(unittest.TestCase):
    """The real runner, --transport wireless, against fakes (cable out, adb over the AP)."""

    def run_smoke(self, root: Path, *, attached: tuple = (), store: str | None = None,
                  **env_extra: str) -> tuple[int, str]:
        build, bin_dir, usb = root / "build", root / "bin", root / "usb"
        build.mkdir()
        bin_dir.mkdir()
        usb.mkdir()
        for index, (vendor, product, serial) in enumerate(attached):
            usb_device(usb, f"1-{index + 1}", vendor, product, serial)
        (build / "CMakeCache.txt").write_text("CMAKE_BUILD_TYPE:STRING=Release\n")
        for path, body in ((build / "aa-wireless-receiver", FAKE_WIRELESS_RECEIVER),
                           (build / "aa-dummy-display", FAKE_WIRELESS_DISPLAY),
                           (bin_dir / "adb", base.FAKE_ADB), (bin_dir / "ip", FAKE_IP)):
            path.write_text(body)
            path.chmod(0o755)
        state = root / "state"
        state.mkdir()
        (state / "approved-phones.json").write_text("{}")
        env = dict(os.environ, FAKE_DIR=str(root), DRIVE_HOOK="", PATH=f"{bin_dir}:/usr/bin:/bin",
                   FAKE_ADB_LOG=str(root / "adb.log"), FAKE_ADB_WIFI_STATUS=WIFI,
                   FAKE_USB=str(usb), FAKE_LEASES=str(root / "absent-{interface}.leases"),
                   FAKE_PROC=str(root / "proc"))
        env.update(env_extra)
        runner = subprocess.Popen(
            [sys.executable, "-B", "-c", DRIVER, str(base.SMOKE.parent), "run",
             "--transport", "wireless", "--phone-id", "7", "--ap-interface", "wlan0",
             "--ap-channel", "44",
             "--serial", PHONE, "--build-dir", str(build),
             "--store", store or str(state / "approved-phones.json"),
             "--display-env", ":99", "--media-uri", "file:///sdcard/Music/fake.opus",
             "--media-component", "org.example.player/.Play", "--duration", "2",
             "--out", str(root / "out")], env=env, stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT, text=True, cwd=root)
        try:
            output, _ = runner.communicate(timeout=90)
        finally:
            if runner.poll() is None:
                runner.kill()
            if (root / "receiver.pids").exists():
                for pid in (int(x) for x in (root / "receiver.pids").read_text().split()):
                    try:
                        os.kill(pid, signal.SIGKILL)
                    except ProcessLookupError:
                        pass
        return runner.returncode, output

    def test_wireless_session_launch_bind_record_and_teardown(self) -> None:
        with tempfile.TemporaryDirectory(prefix="drive-smoke-wireless-") as directory:
            root = Path(directory)
            code, output = self.run_smoke(root)
            argv = (root / "receiver.argv").read_text().splitlines()
            adb = (root / "adb.log").read_text().splitlines()
            result = json.loads((root / "out" / "result.json").read_text())
            run = json.loads((root / "out" / "run.json").read_text())
        # The wireless receiver for the approved phone, with its diagnostics; no USB port.
        self.assertEqual(argv[:5], ["--store-dir", str(root / "state"), "--phone-id", "7",
                                    "--diagnostics"], output)
        self.assertNotIn("--port", argv)
        # Neon P2-5: the AP channel ap.conf names reaches the receiver; it is a
        # link parameter, not the decode configuration the gate compares.
        self.assertEqual(argv[argv.index("--ap-channel") + 1], "44")
        self.assertEqual(result["configuration"], smoke.DEFAULT_CONFIGURATION)
        # The bare active diagnostic (main's shape) started the drive.
        self.assertEqual(run["active_marker"], "diag_active", output)
        self.assertGreater(run["drive_from_ns"], run["active_ns"])
        # adb to the receiver's session peer, bound by the device's ro.serialno first.
        target = f"{STATION}:5555"
        self.assertEqual(adb[:2], [f"connect {target}", f"-s {target} shell getprop ro.serialno"], adb)
        self.assertFalse([line for line in adb if "setFunctions" in line], adb)
        self.assertEqual(adb[-1], f"disconnect {target}", adb)
        # The player is stopped and verified before the session is torn down.
        self.assertIn(f"-s {target} shell am force-stop org.example.player", adb)
        self.assertEqual((result["transport"], result["phone_id"], result["phone_serial"]),
                         ("wireless", 7, PHONE))
        link = result["link"]
        self.assertTrue(link["adb_bound"])
        clean = {"readable": True, "phone_attached": False, "aoa_attached": False}
        self.assertEqual(link["host_usb"]["before"], clean)
        self.assertEqual(link["host_usb"]["after"], clean)
        self.assertTrue(link["host_usb"]["during"])
        for moment in ("before", "after"):
            self.assertEqual(link[moment]["wifi"], {"connected": True, "frequency_mhz": 5180,
                                                    "channel": 36, "rssi_dbm": -48})
        self.assertNotIn("02:00:00", json.dumps(result))  # no MAC anywhere in the record
        self.assertEqual(result["cleanup_failures"], [], output)
        # The same criteria judge it over a real drive window that starts with a
        # paint after the phone's preparation (Neon P2-3; the single-frame fake
        # never got there). The A/V contract fails: this fake plays no audio. A
        # 2 s drive has no full 10 s fps window, so that criterion cannot pass
        # here, but it was judged on post-preparation paints.
        failed = sorted(c["name"] for c in result["criteria"] if not c["passed"])
        self.assertEqual(failed, ["av_signed_contract", "painted_fps_per_window"], output)
        fps = base.criterion(result, "painted_fps_per_window")
        self.assertIn("expected_windows", fps, output)
        self.assertNotIn("reason", fps, output)
        self.assertTrue(base.criterion(result, "no_session_drop")["passed"], output)
        self.assertEqual((code, result["verdict"]), (1, "FAIL"), output)

    def test_a_cabled_phone_blocks_before_launch(self) -> None:
        for attached, reason in ((("04e8", "6860", PHONE),), "usb_attached:phone_on_usb"), \
                                ((("18d1", "2d00", None),), "usb_attached:usb_in_aoa"):
            with self.subTest(reason=reason), \
                    tempfile.TemporaryDirectory(prefix="drive-smoke-wireless-usb-") as directory:
                root = Path(directory)
                code, output = self.run_smoke(root, attached=attached)
                result = json.loads((root / "out" / "result.json").read_text())
                launched = (root / "receiver.argv").exists()
                self.assertEqual((code, result["verdict"], result["blocked_on"]),
                                 (3, "BLOCKED", reason), output)
                self.assertFalse(launched)

    def test_unreachable_adb_blocks_and_sends_nothing(self) -> None:
        with tempfile.TemporaryDirectory(prefix="drive-smoke-wireless-adb-") as directory:
            root = Path(directory)
            code, output = self.run_smoke(root, FAKE_ADB_CONNECT="fail")
            result = json.loads((root / "out" / "result.json").read_text())
            adb = (root / "adb.log").read_text().splitlines()
        self.assertEqual((code, result["verdict"], result["blocked_on"]),
                         (3, "BLOCKED", "adb_unreachable"), output)
        self.assertFalse([line for line in adb if " shell " in line], adb)

    def test_another_phone_on_the_ap_blocks(self) -> None:
        with tempfile.TemporaryDirectory(prefix="drive-smoke-wireless-other-") as directory:
            root = Path(directory)
            code, output = self.run_smoke(root, FAKE_ADB_SERIALNO="SOMEONEELSE")
            result = json.loads((root / "out" / "result.json").read_text())
        self.assertEqual((code, result["blocked_on"]), (3, "adb_phone_mismatch"), output)

    def test_cable_plugged_in_during_the_drive_never_passes(self) -> None:
        with tempfile.TemporaryDirectory(prefix="drive-smoke-wireless-plug-") as directory:
            root = Path(directory)
            code, output = self.run_smoke(root, FAKE_USB_PLUG_AFTER_S="1.0")
            result = json.loads((root / "out" / "result.json").read_text())
        during = result["link"]["host_usb"]["during"]
        self.assertTrue(any(sample["aoa_attached"] for sample in during), during)
        self.assertTrue(result["link"]["host_usb"]["after"]["aoa_attached"])
        self.assertTrue([f for f in result["cleanup_failures"] if f.startswith("usb_attached")],
                        result["cleanup_failures"])
        self.assertNotEqual(result["verdict"], "PASS", output)
        self.assertEqual(transport.link_gap(result["link"]), "wireless_usb_in_aoa_during")

    def test_a_receiver_that_dies_before_the_bind_fails_not_blocks(self) -> None:
        # Neon P3-8: a receiver crash is a product FAIL, not an adb harness block.
        with tempfile.TemporaryDirectory(prefix="drive-smoke-wireless-dead-") as directory:
            root = Path(directory)
            code, output = self.run_smoke(root, FAKE_RECEIVER_DIE_AFTER_ACTIVE="1")
            result = json.loads((root / "out" / "result.json").read_text())
        self.assertEqual((code, result["verdict"]), (1, "FAIL"), output)
        self.assertEqual(result["failure"], "receiver exited before adb was bound")
        self.assertNotIn("blocked_on", result)

    def test_wireless_needs_an_ap_interface(self) -> None:
        # Neon P1-1: the interface is named by the operator; ap.conf is never read.
        for extra in ([], ["--ap-interface", "../x"], ["--ap-interface", "-x"]):
            with self.assertRaises(SystemExit) as raised:
                smoke.main(["run", "--transport", "wireless", "--phone-id", "7", "--serial", "S",
                            "--build-dir", "/b", "--store", "/s/approved-phones.json",
                            "--media-uri", "u", "--media-component", "p/.a", "--out", "/tmp/x",
                            *extra])
            self.assertEqual(raised.exception.code, 2, extra)

    def test_adb_binds_the_receivers_session_peer_not_any_station(self) -> None:
        # P1: two stations on the AP; the receiver's session is with 10.42.0.99.
        neigh = "10.42.0.23 lladdr 02:00:00:00:00:01 REACHABLE\n10.42.0.99 lladdr 02:00:00:00:00:09 REACHABLE"
        with tempfile.TemporaryDirectory(prefix="drive-smoke-wireless-peer-") as directory:
            root = Path(directory)
            self.run_smoke(root, FAKE_NEIGH=neigh, FAKE_SESSION_PEERS="10.42.0.99")
            adb = (root / "adb.log").read_text().splitlines()
        self.assertEqual(adb[:2], ["connect 10.42.0.99:5555",
                                   "-s 10.42.0.99:5555 shell getprop ro.serialno"], adb)
        self.assertFalse([line for line in adb if "10.42.0.23" in line], adb)

    def test_no_or_foreign_session_peer_blocks_before_adb(self) -> None:
        for peers, reason in (("", "adb_session_peer_unknown"),
                              ("10.42.0.23,10.42.0.99", "adb_session_ambiguous"),
                              ("192.168.7.7", "adb_session_not_on_ap")):
            with tempfile.TemporaryDirectory(prefix="drive-smoke-wireless-nopeer-") as directory:
                root = Path(directory)
                code, output = self.run_smoke(root, FAKE_SESSION_PEERS=peers)
                result = json.loads((root / "out" / "result.json").read_text())
                log = root / "adb.log"
                adb = log.read_text().splitlines() if log.exists() else []
            self.assertEqual((code, result["verdict"], result["blocked_on"]), (3, "BLOCKED", reason),
                             output)
            self.assertFalse([line for line in adb if "connect" in line or " shell " in line], adb)

    def test_a_relative_store_is_made_absolute(self) -> None:
        with tempfile.TemporaryDirectory(prefix="drive-smoke-wireless-rel-") as directory:
            root = Path(directory)
            self.run_smoke(root, store="state/approved-phones.json")
            argv = (root / "receiver.argv").read_text().splitlines()
        self.assertEqual(argv[1], str(root / "state"))

    def test_wireless_needs_a_phone_id(self) -> None:
        with self.assertRaises(SystemExit) as raised:
            smoke.main(["run", "--transport", "wireless", "--serial", "S", "--build-dir", "/b",
                        "--media-uri", "u", "--media-component", "p/.a", "--out", "/tmp/x"])
        self.assertEqual(raised.exception.code, 2)


class CollectorTransportTests(unittest.TestCase):
    def parser(self) -> collect.EventParser:
        parser = collect.EventParser()
        parser.wireless = True
        parser.receiver_started_ns = 1_000_000_000
        return parser

    def test_wireless_active_diagnostic_in_either_shape(self) -> None:
        for shape in (bare_state, wrapped_state):
            parser = self.parser()
            parser._consume('{"event":"ready","state":"waiting_for_consumer"}')
            parser._consume('{"event":"wireless_admission"}')
            self.assertFalse(parser.projection_active)
            parser._consume(shape(1, 3_500_000_000, "active"))
            self.assertTrue(parser.projection_active, shape)
            self.assertEqual(parser.startup_ms, 2500)
            parser._consume(shape(1, 4_000_000_000, "disconnected"))
            self.assertFalse(parser.projection_active)

    def test_wireless_readmission_ends_the_active_state(self) -> None:
        for second in ('{"event":"wireless_admission"}', '{"event":"wireless_admission_expired"}',
                       bare_state(2, 5_000_000_000, "active"),
                       bare_state(1, 5_000_000_000, "reconnecting")):
            parser = self.parser()
            parser._consume('{"event":"wireless_admission"}')
            parser._consume(bare_state(1, 3_500_000_000, "active"))
            parser._consume(second)
            self.assertFalse(parser.projection_active, second)
            # A re-admitted session never makes it active again: continuity failed.
            parser._consume(bare_state(2, 6_000_000_000, "active"))
            self.assertFalse(parser.projection_active, second)

    def test_admission_retries_before_active_are_startup(self) -> None:
        # Neon P2-7: the receiver retries an admission after 15 s; that is
        # still startup, not a drop (the drive smoke counts only after active).
        for retry in ('{"event":"wireless_admission"}', '{"event":"wireless_admission_expired"}'):
            parser = self.parser()
            parser._consume('{"event":"wireless_admission"}')
            parser._consume(retry)
            parser._consume('{"event":"wireless_admission"}')
            parser._consume(bare_state(1, 3_500_000_000, "active"))
            self.assertTrue(parser.projection_active, retry)

    def test_wired_parsing_is_unchanged(self) -> None:
        # Review P3: the diag rules are wireless-only; wired keeps main's behaviour.
        parser = collect.EventParser()
        parser._consume('{"event":"active","startup_ms":812}')
        parser._consume(wrapped_state(1, 9_000_000_000, "failed"))
        self.assertEqual((parser.projection_active, parser.startup_ms), (True, 812))
        parser = collect.EventParser()
        parser.receiver_started_ns = 0
        parser._consume(wrapped_state(1, 9_000_000_000, "active"))
        self.assertEqual((parser.projection_active, parser.startup_ms), (False, None))

    def test_wireless_cli_needs_phone_id_and_store(self) -> None:
        self.assertEqual(collect._cli(["--receiver", "r", "--port", "x", "--duration-seconds", "1",
                                       "--transport", "wireless"]), 2)


COLLECTOR_RECEIVER = """#!/usr/bin/env python3
import os, signal, sys, time
""" + FAKE_SESSION + """
argv = sys.argv[1:]
with open(os.environ["FAKE_RX_ARGV"], "w") as handle:
    handle.write("\\n".join(argv))
def stopped(*_):
    # Records whether the phone was already restored when the session ended.
    with open(os.environ["FAKE_RX_ARGV"] + ".term", "w") as handle:
        log = open(os.environ["FAKE_ADB_LOG"]).read()
        handle.write("restored-before-teardown" if "force-stop" in log else "not-restored")
    sys.exit(0)
signal.signal(signal.SIGTERM, stopped)
print('{"event":"ready","state":"waiting_for_consumer"}', flush=True)
time.sleep(0.3)
print('{"event":"wireless_admission"}', flush=True)
if not os.environ.get("FAKE_RECEIVER_DIE_AFTER_ACTIVE"):
    fake_session()
print('{"kind":"session_state_changed","session":1,"mono_ns":%d,"to_state":"active","transport":"usb"}'
      % time.monotonic_ns(), flush=True)
if os.environ.get("FAKE_RECEIVER_DIE_AFTER_ACTIVE"):
    time.sleep(float(os.environ["FAKE_RECEIVER_DIE_AFTER_ACTIVE"]))
    sys.exit(7)
units = 0
while True:
    units += 6
    print('{"event":"video_sent","units":%d}' % units, flush=True)
    print('{"event":"audio_chunks","count":%d}' % units, flush=True)
    time.sleep(0.2)
"""


class CollectorWirelessRunTests(unittest.TestCase):
    """The real collector, --transport wireless, with fakes (cable out, adb over the AP)."""

    def collect(self, root: Path, attached: tuple = (), **env_extra: str):
        from unittest.mock import patch
        import aa_moving_map as mm
        bin_dir, usb = root / "bin", root / "usb"
        bin_dir.mkdir()
        usb.mkdir()
        for index, (vendor, product, serial) in enumerate(attached):
            usb_device(usb, f"1-{index + 1}", vendor, product, serial)
        display = '#!/usr/bin/env python3\nimport sys\nfor _ in sys.stdin: pass\n'
        for name, body in (("adb", base.FAKE_ADB), ("ip", FAKE_IP), ("rx", COLLECTOR_RECEIVER),
                           ("display", display)):
            (bin_dir / name).write_text(body)
            (bin_dir / name).chmod(0o755)
        (root / "state").mkdir()
        env = {"PATH": f"{bin_dir}:{os.environ['PATH']}", "FAKE_ADB_LOG": str(root / "adb.log"),
               "FAKE_RX_ARGV": str(root / "rx.argv"), "FAKE_ADB_WIFI_STATUS": WIFI,
               "FAKE_PROC": str(root / "proc"), **env_extra}
        args = argparse.Namespace(
            receiver=str(bin_dir / "rx"), display=str(bin_dir / "display"), no_display=False,
            port="1-2.1", duration_seconds=3, interval=1, startup_timeout_seconds=10,
            store="state/approved-phones.json", video_socket=None, input_socket=None,
            state_dir=str(root / "logs"), phone_serial=PHONE, workload="moving-map",
            media_uri="file:///sdcard/Music/fixture.opus", media_type=None,
            media_component="org.example.player/.Play", geo_uri=None,
            thermal_policy="strict-all-zones", transport="wireless", phone_id=7,
            ap_interface="wlan0", ap_channel=44)
        real_sleep = time_sleep
        with patch.dict(os.environ, env), \
                patch.object(collect, "_display_connected", return_value=True), \
                patch.object(collect, "build_processes", return_value=[]), \
                patch.object(collect, "_ADB_BIND_TIMEOUT_SECONDS", 2.0), \
                patch.object(collect, "_HOST_USB_PERIOD_SECONDS", 0.5), \
                patch.object(transport, "USB_DEVICES", usb), \
                patch.object(transport, "NM_LEASES", str(root / "absent-{interface}.leases")), \
                patch.object(transport, "PROC", root / "proc"), \
                patch.object(mm, "MEDIA_RELAUNCH_SILENCE_S", 1.0), \
                patch.object(collect.time, "sleep", side_effect=lambda s: real_sleep(min(s, 0.5))):
            old = os.getcwd()
            os.chdir(root)
            try:
                return collect.collect(args)
            finally:
                os.chdir(old)

    def test_wireless_stage_binds_prepares_after_active_and_restores_before_teardown(self) -> None:
        with tempfile.TemporaryDirectory(prefix="collect-wireless-") as directory:
            root = Path(directory)
            capture = self.collect(root)
            adb = (root / "adb.log").read_text().splitlines()
            argv = (root / "rx.argv").read_text().splitlines()
            term = (root / "rx.argv.term").read_text()
        self.assertEqual(argv[:5], ["--store-dir", str(root / "state"), "--phone-id", "7",
                                    "--diagnostics"])
        self.assertNotIn("--require-usb-serial", argv)
        self.assertEqual(argv[argv.index("--ap-channel") + 1], "44")
        # Neon P2-4: the receiver outlives the bind and the phone setup too.
        # duration 3 + startup 10 + 10, plus the (patched, 2 s) bind and the headroom.
        self.assertEqual(int(argv[argv.index("--duration-ms") + 1]),
                         (3 + 10 + 10 + 2 + collect._WIRELESS_PREPARE_HEADROOM_SECONDS) * 1000)
        target = f"{STATION}:5555"
        self.assertEqual(adb[:2], [f"connect {target}", f"-s {target} shell getprop ro.serialno"], adb)
        self.assertFalse([line for line in adb if "setFunctions" in line], adb)
        self.assertEqual(term, "restored-before-teardown")
        self.assertEqual(adb[-1], f"disconnect {target}", adb)
        self.assertEqual((capture["mode"], capture["startup_basis"]),
                         ("wireless", "harness_launch_to_active"))
        self.assertIsInstance(capture["events"]["startup_ms"], int)
        link = capture["link"]
        self.assertTrue(link["adb_bound"])
        self.assertTrue(link["host_usb"]["during"])
        self.assertIsNone(report.wireless_link_gap(capture))
        self.assertEqual(capture["workload"]["cleanup_failures"], [])
        self.assertTrue(capture["workload"]["stimulus_started"])

    def test_wireless_stage_refused_when_the_cable_is_in(self) -> None:
        with tempfile.TemporaryDirectory(prefix="collect-wireless-usb-") as directory:
            root = Path(directory)
            with self.assertRaises(collect.PhoneSetupError) as raised:
                self.collect(root, attached=(("18d1", "2d01", None),))
            launched = (root / "rx.argv").exists()
        self.assertTrue(str(raised.exception).startswith("usb_attached:usb_in_aoa"))
        self.assertFalse(launched)

    def test_wireless_stage_refused_when_adb_cannot_bind(self) -> None:
        with tempfile.TemporaryDirectory(prefix="collect-wireless-adb-") as directory:
            root = Path(directory)
            with self.assertRaises(collect.PhoneSetupError) as raised:
                self.collect(root, FAKE_ADB_SERIALNO="SOMEONEELSE")
            adb = (root / "adb.log").read_text().splitlines()
        self.assertTrue(str(raised.exception).startswith("adb_phone_mismatch"), raised.exception)
        # Only the identity query reached the other phone: nothing was driven.
        self.assertFalse([line for line in adb if " shell " in line
                          and not line.endswith("shell getprop ro.serialno")], adb)


    def test_a_receiver_that_dies_during_the_bind_is_an_early_exit(self) -> None:
        with tempfile.TemporaryDirectory(prefix="collect-wireless-dead-") as directory:
            capture = self.collect(Path(directory), FAKE_RECEIVER_DIE_AFTER_ACTIVE="1.5")
        self.assertTrue(capture["early_exit"])
        self.assertFalse(capture["link"]["adb_bound"])

    def test_wireless_stage_binds_only_the_receivers_session_peer(self) -> None:
        with tempfile.TemporaryDirectory(prefix="collect-wireless-peer-") as directory:
            root = Path(directory)
            with self.assertRaises(collect.PhoneSetupError) as raised:
                self.collect(root, FAKE_SESSION_PEERS="")
            log = root / "adb.log"
            adb = log.read_text().splitlines() if log.exists() else []
        self.assertTrue(str(raised.exception).startswith("adb_session_peer_unknown"), raised.exception)
        self.assertFalse([line for line in adb if "connect" in line or " shell " in line], adb)

# The capture aa-projection-perf.sh's blocked_result() writes, for wireless.
BLOCKED_CAPTURE = {
    "schema": "aa-projection-perf-capture/3",
    "av_series": {"pair_t_ns": [], "pair_skew_ns": [], "audio_intervals_ns": []},
    "workload": {"kind": "none"}, "monotonic_origin_ns": 0, "cpu_ticks_per_second": 1,
    "video_evidence": {"blocks": [], "ipc_latency_ns": [], "decode_latency_ns": []},
    "mode": "wireless", "duration_seconds": 0, "crashes": 0, "samples": [],
    "events": {"helper_rss_mb": None},
    "build": {"build_type": None, "build_dir": None, "allow_unoptimized_build": False,
              "verified_on": "none"},
}


class ReporterTransportTests(unittest.TestCase):
    host = {"readable": True, "phone_attached": False, "aoa_attached": False}

    def test_blocked_and_malformed_wireless_captures_still_write_valid_v3(self) -> None:
        # Neon P1-2: a missing or malformed link must never crash the reporter.
        import aa_perf_schema as schema
        links = {"no link": None, "link null": {"link": None},
                 "during null": {"link": {"adb_bound": True, "host_usb": {"during": None}}},
                 "garbage": {"link": {"adb_bound": "yes", "before": [1], "after": {"wifi": 3},
                                      "host_usb": {"before": {"readable": "x"}, "during": [7, None],
                                                   "after": "y"}}}}
        for name, extra in links.items():
            with tempfile.TemporaryDirectory() as directory:
                capture, out = Path(directory) / "capture.json", Path(directory) / "result.json"
                capture.write_text(json.dumps({**BLOCKED_CAPTURE, **(extra or {})}))
                code = report._cli(["--live", "--capture", str(capture), "--out", str(out),
                                    "--duration-requested", "1800s", "--now", "2026-11-20T00:00:00Z",
                                    "--blocked-on", "phone_setup_failed"])
                self.assertTrue(out.exists(), name)
                doc = json.loads(out.read_text())
            self.assertEqual(schema.validate(doc, schema.load_schema(schema.schema_for(doc["schema"]))),
                             [], name)
            self.assertEqual((code, doc["verdict"], doc["mode"]), (3, "BLOCKED", "wireless"), name)
            self.assertFalse(doc["link"]["adb_bound"] if name != "during null" else False, name)

    def capture(self, **link) -> dict:
        value = {"phone_id": 7, "adb_bound": True, "before": None, "after": None,
                 "host_usb": {"before": self.host, "during": [self.host], "after": self.host}}
        value.update(link)
        return {"mode": "wireless", "link": value}

    def test_wireless_link_gap(self) -> None:
        gap = report.wireless_link_gap
        self.assertIsNone(gap({"mode": "wired"}))
        self.assertEqual(gap({"mode": "wireless"}), "wireless_link_unrecorded")
        self.assertIsNone(gap(self.capture()))
        self.assertEqual(gap(self.capture(adb_bound=False)), "wireless_adb_unbound")
        attached = dict(self.host, phone_attached=True)
        self.assertEqual(gap(self.capture(host_usb={"before": self.host, "during": [attached],
                                                    "after": self.host})),
                         "wireless_phone_on_usb_during")
        aoa = dict(self.host, aoa_attached=True)
        self.assertEqual(gap(self.capture(host_usb={"before": self.host, "during": [self.host],
                                                    "after": aoa})), "wireless_usb_in_aoa_after")
        self.assertEqual(gap(self.capture(host_usb={"before": self.host, "during": [],
                                                    "after": self.host})),
                         "wireless_host_usb_unrecorded")

    def test_result_link_drops_the_phone_id(self) -> None:
        link = report._result_link(self.capture()["link"])
        self.assertNotIn("phone_id", json.dumps(link))
        self.assertTrue(link["adb_bound"])


class LauncherTransportTests(unittest.TestCase):
    def launch(self, *extra: str) -> subprocess.CompletedProcess:
        return subprocess.run(["bash", str(base.LAUNCHER), "--duration", "30s", "--out", "/dev/null",
                               *extra], capture_output=True, text=True, timeout=30)

    def test_unknown_mode_is_a_usage_error(self) -> None:
        result = self.launch("--mode", "bluetooth")
        self.assertEqual(result.returncode, 2, result.stderr)

    def test_a_blocked_wireless_stage_writes_a_valid_v3_result(self) -> None:
        # Neon P1-2 end to end: blocked_result() for --mode wireless.
        import aa_perf_schema as schema
        with tempfile.TemporaryDirectory(prefix="launcher-wireless-blocked-") as directory:
            root = Path(directory)
            process, _ = base.LauncherGateTests.launch(
                self, root, "--mode", "wireless", "--phone-id", "7", "--ap-interface", "wlan0",
                "--store", "/s/approved-phones.json")
            self.assertEqual(process.returncode, 3, process.stderr)
            doc = json.loads((root / "out.json").read_text())
        self.assertEqual((doc["verdict"], doc["mode"]), ("BLOCKED", "wireless"))
        self.assertEqual(schema.validate(doc, schema.load_schema(schema.schema_for(doc["schema"]))), [])

    def test_wireless_needs_an_ap_interface_and_a_valid_channel(self) -> None:
        base_args = ("--mode", "wireless", "--phone-id", "3", "--store", "/s/approved-phones.json")
        result = self.launch(*base_args)
        self.assertEqual(result.returncode, 2, result.stderr)
        self.assertIn("--ap-interface", result.stderr)
        result = self.launch(*base_args, "--ap-interface", "wlan0", "--ap-channel", "52")
        self.assertEqual(result.returncode, 2, result.stderr)
        self.assertIn("--ap-channel", result.stderr)

    def test_wireless_readiness_needs_no_stage_arguments(self) -> None:
        # Neon P3-9: readiness only probes the target.
        with tempfile.TemporaryDirectory() as directory:
            ssh = Path(directory) / "ssh"
            ssh.write_text("#!/bin/sh\nexit 255\n")
            ssh.chmod(0o755)
            result = subprocess.run(["bash", str(base.LAUNCHER), "--mode", "wireless",
                                     "--readiness-only", "--host", "fake.local"],
                                    capture_output=True, text=True, timeout=60,
                                    env={"PATH": f"{directory}:/usr/bin:/bin"})
        self.assertNotEqual(result.returncode, 2, result.stderr)
        self.assertNotIn("usage error", result.stderr)

    def test_wireless_needs_phone_id_and_store(self) -> None:
        result = self.launch("--mode", "wireless")
        self.assertEqual(result.returncode, 2, result.stderr)
        self.assertIn("--phone-id", result.stderr)
        result = self.launch("--mode", "wireless", "--phone-id", "3", "--store", "/s/other.json")
        self.assertEqual(result.returncode, 2, result.stderr)


class SoakChainTransportTests(unittest.TestCase):
    def args(self, **fields) -> argparse.Namespace:
        values = dict(launcher="L", host="h", port="1-2.1", phone_serial="S", drive_smoke="d",
                      media_uri="u", interval=5, out=Path("/o"), media_type=None,
                      media_component="p/.a", geo_uri=None, receiver=None, display=None,
                      store=None, build_dir=None, thermal_policy=None, provenance=None,
                      display_env=None, phone_id=None, transport="wired", ap_interface=None,
                      ap_channel=None)
        values.update(fields)
        return argparse.Namespace(**values)

    def test_default_stage_is_wired(self) -> None:
        command = chain.stage_command(self.args(), 1800)
        self.assertEqual(command[1:3], ["--mode", "wired"])
        self.assertNotIn("--phone-id", command)

    def test_wireless_stage(self) -> None:
        command = chain.stage_command(self.args(transport="wireless", phone_id=7,
                                                store="/s/approved-phones.json"), 1800)
        self.assertEqual(command[1:3], ["--mode", "wireless"])
        self.assertEqual(command[command.index("--phone-id") + 1], "7")
        command = chain.stage_command(self.args(transport="wireless", phone_id=7, ap_interface="wlan0",
                                                ap_channel=44, store="/s/approved-phones.json"), 1800)
        self.assertEqual(command[command.index("--ap-interface") + 1], "wlan0")
        self.assertEqual(command[command.index("--ap-channel") + 1], "44")

    def test_wireless_chain_needs_phone_id(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            self.assertEqual(chain.main(["--out", str(Path(directory) / "c"), "--phone-serial", "S",
                                         "--drive-smoke", "d", "--media-uri", "u",
                                         "--media-component", "p/.a", "--transport", "wireless"]), 2)
            self.assertEqual(chain.main(["--out", str(Path(directory) / "d"), "--phone-serial", "S",
                                         "--drive-smoke", "d", "--media-uri", "u",
                                         "--media-component", "p/.a", "--transport", "wireless",
                                         "--phone-id", "7", "--store", "/s/approved-phones.json"]), 2)


class Issue171StartupBudgetAndApTests(unittest.TestCase):
    """Issue #171 and PR #184 Sol review fixes (P2 startup budget, v3 schema, P3 AP & CLI bounds)."""

    @staticmethod
    def _wireless_record(**fields) -> dict:
        gate = GateTransportTests()
        return gate.wireless(**fields)

    @staticmethod
    def _live_wireless_capture(ap_interface: str | None = "wlan0",
                               ap_channel: int | None = 44,
                               *, origin: str = "live") -> dict:
        perf_tests = str(ROOT / "tests" / "perf")
        if perf_tests not in sys.path:
            sys.path.insert(0, perf_tests)
        import test_v3_av_contract as v3_contract  # noqa: E402
        capture = v3_contract.v3(seconds=30)
        capture["mode"] = "wireless"
        capture["measurement_origin"] = origin
        capture["startup_wait_seconds"] = 1.2
        capture["active_projection_seconds"] = 30.0
        capture["link"] = {
            "phone_id": 7,
            "adb_bound": True,
            "before": None,
            "after": None,
            "host_usb": {
                "before": GateTransportTests.CLEAN,
                "during": [GateTransportTests.CLEAN],
                "after": GateTransportTests.CLEAN,
            },
        }
        if ap_interface is not None:
            capture["ap_interface"] = ap_interface
        if ap_channel is not None:
            capture["ap_channel"] = ap_channel
        return capture

    def test_derived_wireless_startup_budget_includes_post_tcp_negotiation_and_wired_unchanged(self) -> None:
        # Wired budgets stay strictly unchanged at 25 s (smoke) and 60 s (collector).
        self.assertEqual(smoke.READY_TIMEOUT_S, 25.0)
        self.assertEqual(collect._PROJECTION_STARTUP_TIMEOUT_SECONDS, 60)
        self.assertEqual(smoke.startup_budget("wired", None), 25.0)
        self.assertEqual(collect.startup_timeout_seconds("wired", None), 60)

        # Derived wireless floor must include: attempt1 (15) + backoff cap (5) + attempt2 (15)
        # + bootstrap (40) + connect (5) + post-TCP negotiation (10, WirelessLink.cpp:58)
        # + 2x helper RPC bounds (4 * 1 s per admission, BusClient.cpp)
        # + ready poll (5) + display probe fallback (5) + safety margin -> 120 s floor.
        self.assertEqual(transport.WIRELESS_ADMISSION_ATTEMPT_S, 15)
        self.assertEqual(transport.WIRELESS_BACKOFF_CAP_S, 5)
        self.assertEqual(transport.WIRELESS_BOOTSTRAP_S, 40)
        self.assertEqual(transport.WIRELESS_CONNECT_S, 5)
        self.assertEqual(transport.WIRELESS_NEGOTIATION_S, 10)
        self.assertEqual(transport.WIRELESS_HELPER_RPC_S, 1)
        self.assertEqual(transport.WIRELESS_ADMISSION_HELPER_S, 4)
        self.assertEqual(transport.WIRELESS_READY_POLL_S, 5)
        self.assertEqual(transport.WIRELESS_DISPLAY_PROBE_S, 5)
        self.assertGreater(transport.WIRELESS_STARTUP_MARGIN_S, 0)
        expected_floor = (
            transport.WIRELESS_ADMISSION_ATTEMPT_S
            + transport.WIRELESS_BACKOFF_CAP_S
            + transport.WIRELESS_ADMISSION_ATTEMPT_S
            + transport.WIRELESS_BOOTSTRAP_S
            + transport.WIRELESS_CONNECT_S
            + transport.WIRELESS_NEGOTIATION_S
            + 2 * transport.WIRELESS_ADMISSION_HELPER_S
            + transport.WIRELESS_READY_POLL_S
            + transport.WIRELESS_DISPLAY_PROBE_S
            + transport.WIRELESS_STARTUP_MARGIN_S
        )
        self.assertEqual(transport.WIRELESS_STARTUP_BUDGET_S, expected_floor)
        self.assertEqual(transport.WIRELESS_STARTUP_BUDGET_S, 120)
        self.assertEqual(smoke.WIRELESS_STARTUP_BUDGET_S, float(transport.WIRELESS_STARTUP_BUDGET_S))
        self.assertEqual(collect.WIRELESS_STARTUP_BUDGET_S, transport.WIRELESS_STARTUP_BUDGET_S)
        self.assertEqual(smoke.startup_budget("wireless", None),
                         float(transport.WIRELESS_STARTUP_BUDGET_S))
        self.assertEqual(collect.startup_timeout_seconds("wireless", None),
                         transport.WIRELESS_STARTUP_BUDGET_S)

    def test_virtual_clock_late_tcp_attach_and_late_active_fails_at_85_passes_at_floor(self) -> None:
        # Attempt 1 expires at 17 s -> 5 s backoff -> attempt 2 at 22 s -> late TCP attach
        # at 80 s ("negotiating") -> 10 s negotiation -> ACTIVE at 90 s (plus polling near floor).
        second = 1_000_000_000
        events = {
            1 * second: '{"event":"ready","state":"waiting_for_consumer"}',
            2 * second: '{"event":"wireless_admission"}',
            17 * second: '{"event":"wireless_admission_expired"}',
            22 * second: '{"event":"wireless_admission"}',
            80 * second: '{"kind":"session_state_changed","session":1,"mono_ns":80000000000,'
                         '"to_state":"negotiating","transport":"usb"}',
            90 * second: (
                '{"kind":"session_state_changed","session":1,"mono_ns":90000000000,'
                '"to_state":"active","transport":"usb"}\n'
                '{"event":"video_sent","units":10}\n'
                '{"event":"audio_chunks","count":10}\n'
                '{"event":"audio_write_completed","scheduled_ns":90000000000,'
                '"scheduled_end_ns":90040000000,"completed_ns":90000000000,"presented_ns":90010000000}\n'
                '{"event":"video_presented","frame":1,"scheduled_ns":90000000000,'
                '"completed_ns":90012000000,"count":1}'
            ),
            92 * second: (
                '{"event":"video_sent","units":20}\n'
                '{"event":"audio_chunks","count":20}\n'
                '{"event":"audio_write_completed","scheduled_ns":92000000000,'
                '"scheduled_end_ns":92040000000,"completed_ns":92000000000,"presented_ns":92010000000}\n'
                '{"event":"video_presented","frame":2,"scheduled_ns":92000000000,'
                '"completed_ns":92012000000,"count":1}'
            ),
            94 * second: (
                '{"event":"video_sent","units":30}\n'
                '{"event":"audio_chunks","count":30}\n'
                '{"event":"audio_write_completed","scheduled_ns":94000000000,'
                '"scheduled_end_ns":94040000000,"completed_ns":94000000000,"presented_ns":94010000000}\n'
                '{"event":"video_presented","frame":3,"scheduled_ns":94000000000,'
                '"completed_ns":94012000000,"count":1}'
            ),
        }

        def run_with_startup(startup_timeout_s: int | None) -> dict:
            clock_ns = [0]
            spawned: list = []
            written = {t: False for t in events}

            class FakeProc:
                pid = os.getpid()

                def __init__(self, command=(), *, stdout=None, **kwargs) -> None:
                    self.stdout = stdout
                    self.returncode = None
                    spawned.append(self)

                def poll(self):
                    return self.returncode

                def terminate(self):
                    self.returncode = 0

                def kill(self):
                    self.returncode = -9

                def wait(self, timeout=None):
                    return 0

            def fake_sleep(seconds: float) -> None:
                clock_ns[0] += int(seconds * second)
                for stamp, block in sorted(events.items()):
                    if clock_ns[0] >= stamp and not written[stamp]:
                        written[stamp] = True
                        spawned[0].stdout.write(block + "\n")
                        spawned[0].stdout.flush()

            clean_usb = {"readable": True, "phone_attached": False, "aoa_attached": False}
            with tempfile.TemporaryDirectory(prefix="wireless-post-tcp-") as directory:
                root = Path(directory)
                args = argparse.Namespace(
                    receiver="rx", display="disp", no_display=False, port="1-2.1",
                    duration_seconds=3, interval=1, store=str(root / "approved-phones.json"),
                    video_socket=None, input_socket=None, state_dir=str(root / "state"),
                    phone_serial=PHONE, workload="none", thermal_policy="strict-all-zones",
                    transport="wireless", phone_id=7, ap_interface="wlan0", ap_channel=44,
                    startup_timeout_seconds=startup_timeout_s)
                with (unittest.mock.patch.object(collect.time, "monotonic_ns", side_effect=lambda: clock_ns[0]),
                      unittest.mock.patch.object(collect.time, "monotonic", side_effect=lambda: clock_ns[0] / second),
                      unittest.mock.patch.object(collect.time, "sleep", side_effect=fake_sleep),
                      unittest.mock.patch.object(collect.subprocess, "Popen", FakeProc),
                      unittest.mock.patch.object(collect, "_display_connected", return_value=True),
                      unittest.mock.patch.object(collect, "_thermal_inventory", return_value=()),
                      unittest.mock.patch.object(collect, "_thermal_zones", return_value=()),
                      unittest.mock.patch.object(collect, "_target_hardware_state", return_value={}),
                      unittest.mock.patch.object(collect, "build_processes", return_value=[]),
                      unittest.mock.patch.object(transport, "host_usb_evidence", return_value=clean_usb),
                      unittest.mock.patch.object(transport, "receiver_session_peers", return_value=[STATION]),
                      unittest.mock.patch.object(transport, "read_ap_stations", return_value=[STATION]),
                      unittest.mock.patch.object(transport, "bind_adb", return_value=(f"{STATION}:5555", None)),
                      unittest.mock.patch.object(transport, "link_snapshot", return_value={"wifi": {"channel": 44}}),
                      unittest.mock.patch.object(collect, "_host_adb")):
                    return collect.collect(args)

        old_floor_capture = run_with_startup(85)
        self.assertIsNone(old_floor_capture["startup_wait_seconds"])
        self.assertEqual(old_floor_capture["active_projection_seconds"], 0.0)
        self.assertIsNone(old_floor_capture["events"]["startup_ms"])

        derived_floor_capture = run_with_startup(None)
        self.assertIsNotNone(derived_floor_capture["startup_wait_seconds"])
        self.assertGreaterEqual(derived_floor_capture["startup_wait_seconds"], 90.0)
        self.assertEqual(derived_floor_capture["active_projection_seconds"], 3.0)
        self.assertEqual(derived_floor_capture["events"]["startup_ms"], 90000)

    @staticmethod
    def _run_virtual_clock_scenario(scenario_events: dict, *, startup_timeout_s: int | None,
                                    display_probe_s: float, duration_seconds: int = 3,
                                    oversleep_deadline_ms: int = 0,
                                    pause_after_active_s: float = 0.0) -> dict:
        second = 1_000_000_000
        clock_ns = [0]
        spawned: list = []
        written = {t: False for t in scenario_events}
        target_deadline_s = (
            startup_timeout_s if startup_timeout_s is not None else transport.WIRELESS_STARTUP_BUDGET_S
        )
        paused = [False]

        def flush_due() -> None:
            if not spawned:
                return
            for stamp, block in sorted(scenario_events.items()):
                if clock_ns[0] >= stamp and not written[stamp]:
                    written[stamp] = True
                    spawned[0].stdout.write(block + "\n")
                    spawned[0].stdout.flush()

        class FakeProc:
            pid = os.getpid()

            def __init__(self, command=(), *, stdout=None, **kwargs) -> None:
                self.stdout = stdout
                self.returncode = None
                spawned.append(self)

            def poll(self):
                return self.returncode

            def terminate(self):
                self.returncode = 0

            def kill(self):
                self.returncode = -9

            def wait(self, timeout=None):
                return 0

        def fake_sleep(seconds: float) -> None:
            nxt = clock_ns[0] + int(round(seconds * second))
            if (oversleep_deadline_ms and clock_ns[0] < target_deadline_s * second
                    and nxt == target_deadline_s * second):
                nxt += oversleep_deadline_ms * 1_000_000
            clock_ns[0] = nxt
            flush_due()

        def fake_display_probe() -> bool:
            if (pause_after_active_s and not paused[0]
                    and any(w for t, w in written.items() if '"to_state":"active"' in scenario_events[t])):
                paused[0] = True
                clock_ns[0] += int(round(pause_after_active_s * second))
            clock_ns[0] += int(round(display_probe_s * second))
            flush_due()
            return True

        clean_usb = {"readable": True, "phone_attached": False, "aoa_attached": False}
        with tempfile.TemporaryDirectory(prefix="wireless-sol-recheck-") as directory:
            root = Path(directory)
            args = argparse.Namespace(
                receiver="rx", display="disp", no_display=False, port="1-2.1",
                duration_seconds=duration_seconds, interval=5,
                store=str(root / "approved-phones.json"),
                video_socket=None, input_socket=None, state_dir=str(root / "state"),
                phone_serial=PHONE, workload="none", thermal_policy="strict-all-zones",
                transport="wireless", phone_id=7, ap_interface="wlan0", ap_channel=44,
                startup_timeout_seconds=startup_timeout_s)
            with (unittest.mock.patch.object(collect.time, "monotonic_ns", side_effect=lambda: clock_ns[0]),
                  unittest.mock.patch.object(collect.time, "monotonic", side_effect=lambda: clock_ns[0] / second),
                  unittest.mock.patch.object(collect.time, "sleep", side_effect=fake_sleep),
                  unittest.mock.patch.object(collect.subprocess, "Popen", FakeProc),
                  unittest.mock.patch.object(collect, "_display_connected", side_effect=fake_display_probe),
                  unittest.mock.patch.object(collect, "_thermal_inventory", return_value=()),
                  unittest.mock.patch.object(collect, "_thermal_zones", return_value=()),
                  unittest.mock.patch.object(collect, "_target_hardware_state", return_value={}),
                  unittest.mock.patch.object(collect, "build_processes", return_value=[]),
                  unittest.mock.patch.object(transport, "host_usb_evidence", return_value=clean_usb),
                  unittest.mock.patch.object(transport, "receiver_session_peers", return_value=[STATION]),
                  unittest.mock.patch.object(transport, "read_ap_stations", return_value=[STATION]),
                  unittest.mock.patch.object(transport, "bind_adb", return_value=(f"{STATION}:5555", None)),
                  unittest.mock.patch.object(transport, "link_snapshot", return_value={"wifi": {"channel": 44}}),
                  unittest.mock.patch.object(collect, "_host_adb")):
                return collect.collect(args)

    @staticmethod
    def _sol_active_at_events(active_ns: int, *, steps: int = 8) -> dict:
        second = 1_000_000_000
        base_ev = {
            8 * second: '{"event":"ready","state":"waiting_for_consumer"}',
            12 * second: '{"event":"wireless_admission"}',
            27 * second: '{"event":"wireless_admission_expired"}',
            29 * second: '{"event":"wireless_admission"}',
            86 * second: '{"kind":"session_state_changed","session":1,"mono_ns":86000000000,'
                         '"to_state":"negotiating","transport":"usb"}',
            active_ns: (
                f'{{"kind":"session_state_changed","session":1,"mono_ns":{active_ns},'
                f'"to_state":"active","transport":"usb"}}\n'
                '{"event":"video_sent","units":10}\n'
                '{"event":"audio_chunks","count":10}\n'
                f'{{"event":"audio_write_completed","scheduled_ns":{active_ns},'
                f'"scheduled_end_ns":{active_ns + 40_000_000},"completed_ns":{active_ns},'
                f'"presented_ns":{active_ns + 10_000_000}}}\n'
                f'{{"event":"video_presented","frame":1,"scheduled_ns":{active_ns},'
                f'"completed_ns":{active_ns + 12_000_000},"count":1}}'
            ),
        }
        for step in range(1, steps + 1):
            t_ns = active_ns + step * 5 * second
            base_ev[t_ns] = (
                f'{{"event":"video_sent","units":{10 + step * 10}}}\n'
                f'{{"event":"audio_chunks","count":{10 + step * 10}}}\n'
                f'{{"event":"audio_write_completed","scheduled_ns":{t_ns},'
                f'"scheduled_end_ns":{t_ns + 40_000_000},"completed_ns":{t_ns},'
                f'"presented_ns":{t_ns + 10_000_000}}}\n'
                f'{{"event":"video_presented","frame":{1 + step},"scheduled_ns":{t_ns},'
                f'"completed_ns":{t_ns + 12_000_000},"count":1}}'
            )
        return base_ev

    def test_virtual_clock_sol_scenario_and_active_at_deadline_always_binds(self) -> None:
        # Sol re-check scenario: ready 8 s, admissions 12/29 s, expiry 27 s, TCP 86 s, ACTIVE 96 s,
        # 5 s interval, and 4.9 s display probe per loop.
        events = self._sol_active_at_events(96 * 1_000_000_000)

        # P2-A: 10 s request with 4.9 s probe crossing the measurement deadline MUST record >= 10.0 s.
        cap_default = self._run_virtual_clock_scenario(
            events, startup_timeout_s=None, display_probe_s=4.9, duration_seconds=10)
        self.assertIsNotNone(cap_default["startup_wait_seconds"])
        self.assertGreaterEqual(cap_default["active_projection_seconds"], 10.0)
        self.assertEqual(cap_default["events"]["startup_ms"], 96000)

        # Loop ordering with 100 s deadline and immediate probe: ACTIVE at 96 s parsed at t=100 s binds.
        cap_at_deadline = self._run_virtual_clock_scenario(
            events, startup_timeout_s=100, display_probe_s=0.0, duration_seconds=3)
        self.assertIsNotNone(cap_at_deadline["startup_wait_seconds"])
        self.assertEqual(cap_at_deadline["active_projection_seconds"], 3.0)
        self.assertEqual(cap_at_deadline["events"]["startup_ms"], 96000)

    def test_virtual_clock_in_budget_active_mono_ns_and_age_limit(self) -> None:
        second = 1_000_000_000
        run_scenario = self._run_virtual_clock_scenario
        active_at_events = self._sol_active_at_events

        # P2-B: 4.9 s probe with ACTIVE at 119 s and 120 s must bind & record full requested duration.
        for active_s in (119, 120):
            cap_in = run_scenario(
                active_at_events(active_s * second),
                startup_timeout_s=None, display_probe_s=4.9, duration_seconds=10)
            self.assertTrue(cap_in["link"]["adb_bound"], active_s)
            self.assertIsNotNone(cap_in["startup_wait_seconds"], active_s)
            self.assertGreaterEqual(cap_in["active_projection_seconds"], 10.0, active_s)
            self.assertEqual(cap_in["events"]["startup_ms"], active_s * 1000, active_s)

        # P2-B: ACTIVE at 120.001 s (past deadline on receiver clock) must NOT bind.
        cap_late = run_scenario(
            active_at_events(120 * second + 1_000_000),
            startup_timeout_s=None, display_probe_s=4.9, duration_seconds=10)
        self.assertFalse(cap_late["link"]["adb_bound"])
        self.assertIsNone(cap_late["startup_wait_seconds"])
        self.assertEqual(cap_late["active_projection_seconds"], 0.0)

        # P2-B: immediate probe, ACTIVE at 119.9 s, 1 ms oversleep must bind via mono_ns & final read.
        cap_oversleep = run_scenario(
            active_at_events(119 * second + 900_000_000),
            startup_timeout_s=None, display_probe_s=0.0, duration_seconds=3,
            oversleep_deadline_ms=1)
        self.assertTrue(cap_oversleep["link"]["adb_bound"])
        self.assertIsNotNone(cap_oversleep["startup_wait_seconds"])
        self.assertGreaterEqual(cap_oversleep["active_projection_seconds"], 3.0)
        self.assertEqual(cap_oversleep["events"]["startup_ms"], 119900)

        # P2-B: age limit rejects a stale observation after a 30 s pause (observed at 118.9 s -> 153.8 s).
        cap_stale = run_scenario(
            active_at_events(118 * second),
            startup_timeout_s=None, display_probe_s=4.9, duration_seconds=10,
            pause_after_active_s=30.0)
        self.assertFalse(cap_stale["link"]["adb_bound"])
        self.assertIsNone(cap_stale["startup_wait_seconds"])
        self.assertEqual(cap_stale["active_projection_seconds"], 0.0)

    def test_v3_schema_requires_valid_ap_on_wireless_qualifying_and_offline_not_live(self) -> None:
        import aa_perf_schema as schema
        s = schema.load_schema(schema.SCHEMA_V3)

        # Offline replay of a legacy wireless capture (null AP fields): stays OFFLINE_ONLY, never COMPLETE_LIVE.
        offline_legacy = self._live_wireless_capture(
            ap_interface=None, ap_channel=None, origin="offline")
        offline_res = report.build_result(
            offline_legacy, duration_requested="3s", now="2026-11-20T00:00:00Z",
            capture_path=None, capture_sha256=None)
        self.assertEqual(offline_res["verdict"], "PASS")
        self.assertEqual(offline_res["measurement_origin"], "offline")
        self.assertEqual(offline_res["qualification_status"], "OFFLINE_ONLY")
        self.assertIsNone(offline_res["ap_interface"])
        self.assertIsNone(offline_res["ap_channel"])
        self.assertEqual(schema.validate(offline_res, s), [])

        # Live replay of the same missing-AP capture is BLOCKED by the reporter.
        live_legacy = self._live_wireless_capture(
            ap_interface=None, ap_channel=None, origin="live")
        live_missing_res = report.build_result(
            live_legacy, duration_requested="3s", now="2026-11-20T00:00:00Z",
            capture_path=None, capture_sha256=None)
        self.assertEqual(live_missing_res["verdict"], "BLOCKED")
        self.assertEqual(live_missing_res["qualification_status"], "INCOMPLETE")
        self.assertIn("run_evidence:ap_interface", live_missing_res["blocked_on"])
        self.assertIn("run_evidence:ap_channel", live_missing_res["blocked_on"])
        self.assertEqual(schema.validate(live_missing_res, s), [])

        # Live wireless PASS/COMPLETE_LIVE requires valid non-null ap_interface and ap_channel.
        qualifying_res = report.build_result(
            self._live_wireless_capture(ap_interface="wlan0", ap_channel=44, origin="live"),
            duration_requested="3s", now="2026-11-20T00:00:00Z",
            capture_path=None, capture_sha256=None)
        self.assertEqual((qualifying_res["verdict"], qualifying_res["qualification_status"]),
                         ("PASS", "COMPLETE_LIVE"))
        self.assertEqual(schema.validate(qualifying_res, s), [])

        for mutator in (
            lambda d: d.pop("ap_interface", None),
            lambda d: d.pop("ap_channel", None),
            lambda d: (d.pop("ap_interface", None), d.pop("ap_channel", None)),
            lambda d: d.update(ap_interface=None),
            lambda d: d.update(ap_channel=None),
            lambda d: d.update(ap_interface=None, ap_channel=None),
            lambda d: d.update(ap_interface="bad interface"),
            lambda d: d.update(ap_interface="-wlan0"),
            lambda d: d.update(ap_interface=".wlan0"),
            lambda d: d.update(ap_interface="a" * 16),
            lambda d: d.update(ap_interface=""),
            lambda d: d.update(ap_channel=52),
            lambda d: d.update(ap_channel=True),
        ):
            bad = json.loads(json.dumps(qualifying_res))
            mutator(bad)
            self.assertTrue(schema.validate(bad, s), f"expected schema failure for {bad!r}")

        # Offline capture cannot masquerade as COMPLETE_LIVE in the schema.
        forged_offline = json.loads(json.dumps(offline_res))
        forged_offline["qualification_status"] = "COMPLETE_LIVE"
        self.assertTrue(schema.validate(forged_offline, s))

        # Wired PASS results remain valid with no AP fields.
        wired_res = json.loads(json.dumps(qualifying_res))
        wired_res["mode"] = "wired"
        wired_res.pop("ap_interface", None)
        wired_res.pop("ap_channel", None)
        wired_res.pop("link", None)
        wired_res.pop("startup_basis", None)
        self.assertEqual(schema.validate(wired_res, s), [])

    def test_schema_pattern_ecma_trailing_newline_and_legacy_ap_compat(self) -> None:
        import aa_perf_schema as schema
        s = schema.load_schema(schema.SCHEMA_V3)

        qualifying_res = report.build_result(
            self._live_wireless_capture(ap_interface="wlan0", ap_channel=44, origin="live"),
            duration_requested="3s", now="2026-11-20T00:00:00Z",
            capture_path=None, capture_sha256=None)

        # P2-2: trailing newline bypass ("wlan0\n") must fail ECMA-262 pattern matching.
        nl_iface = json.loads(json.dumps(qualifying_res))
        nl_iface["ap_interface"] = "wlan0\n"
        self.assertTrue(schema.validate(nl_iface, s))

        # Generic ECMA-262 pattern semantics across arbitrary patterns (no $ match before final LF).
        for pattern, good_vals, bad_vals in (
            ("^[A-Za-z0-9_][A-Za-z0-9_.-]{0,14}$", ("wlan0",), ("wlan0\n", "bad interface", "")),
            ("^abc$", ("abc",), ("abc\n", "xabc", "abcd")),
            ("tag$", ("tag", "mytag"), ("tag\n", "tagx")),
            (r"^\$1$", ("$1",), ("$1\n", "1")),
            ("^[a$]+$", ("a$",), ("a$\n",)),
            ("[]$|x$", ("x", "ax"), ("x\n", "", "y")),
            ("[^]$", ("x", "\n", "ax", "x\n"), ("",)),
            ("^[^]$", ("a", "\n"), ("ab", "a\n", "x\n", "")),
        ):
            pat_schema = {"type": "string", "pattern": pattern}
            for good in good_vals:
                self.assertEqual(schema.validate(good, pat_schema), [], (pattern, good))
            for bad in bad_vals:
                self.assertTrue(schema.validate(bad, pat_schema), (pattern, bad))
        with self.assertRaises(schema.SchemaError):
            schema.validate("a", {"type": "string", "pattern": "[abc"})

        # P3: Legacy free-form ap_interface on wired, offline, and incomplete records must still validate.
        wired_legacy = json.loads(json.dumps(qualifying_res))
        wired_legacy["mode"] = "wired"
        wired_legacy.pop("link", None)
        wired_legacy.pop("startup_basis", None)
        wired_legacy["ap_interface"] = "bad interface"
        self.assertEqual(schema.validate(wired_legacy, s), [])

        offline_legacy = report.build_result(
            self._live_wireless_capture(ap_interface=None, ap_channel=None, origin="offline"),
            duration_requested="3s", now="2026-11-20T00:00:00Z",
            capture_path=None, capture_sha256=None)
        offline_legacy["ap_interface"] = "bad interface"
        self.assertEqual(schema.validate(offline_legacy, s), [])

        incomplete_legacy = report.build_result(
            self._live_wireless_capture(ap_interface=None, ap_channel=None, origin="live"),
            duration_requested="3s", now="2026-11-20T00:00:00Z",
            capture_path=None, capture_sha256=None)
        self.assertEqual(incomplete_legacy["qualification_status"], "INCOMPLETE")
        incomplete_legacy["ap_interface"] = "bad interface"
        self.assertEqual(schema.validate(incomplete_legacy, s), [])

    def test_wireless_check_record_requires_expected_ap_interface(self) -> None:
        now = datetime.now(timezone.utc)
        ch36 = self._wireless_record(ap_interface="wlan0", ap_channel=36)
        self.assertEqual(
            smoke.check_record(ch36, receiver_sha256="a" * 64, display_sha256="b" * 64,
                               phone_serial="SER", now=now, transport_name="wireless",
                               phone_id=1, ap_channel=36, ap_interface="wlan0")[0],
            None)
        for missing_or_bad in (None, "", "-wlan0", ".wlan0", "bad iface", "wlan1"):
            self.assertEqual(
                smoke.check_record(ch36, receiver_sha256="a" * 64, display_sha256="b" * 64,
                                   phone_serial="SER", now=now, transport_name="wireless",
                                   phone_id=1, ap_channel=36, ap_interface=missing_or_bad)[0],
                "drive_smoke_ap_interface_mismatch", missing_or_bad)

        # CLI `check --transport wireless` requires matching --ap-interface (fails closed with exit 3).
        with tempfile.TemporaryDirectory(prefix="smoke-check-no-iface-") as directory:
            rec_path = Path(directory) / "result.json"
            rec_path.write_text(json.dumps(ch36))
            for extra in ([], ["--ap-interface", "wlan1"]):
                rc = smoke.main(["check", "--record", str(rec_path),
                                 "--receiver-sha256", "a" * 64, "--display-sha256", "b" * 64,
                                 "--phone-serial", "SER", "--transport", "wireless",
                                 "--phone-id", "1", "--ap-channel", "36", *extra,
                                 "--now", now.isoformat()])
                self.assertEqual(rc, 3, extra)
            rc_ok = smoke.main(["check", "--record", str(rec_path),
                                "--receiver-sha256", "a" * 64, "--display-sha256", "b" * 64,
                                "--phone-serial", "SER", "--transport", "wireless",
                                "--phone-id", "1", "--ap-channel", "36",
                                "--ap-interface", "wlan0", "--now", now.isoformat()])
            self.assertEqual(rc_ok, 0)

        # Wired check behavior remains unchanged without ap_interface.
        wired = GateTransportTests().record(transport="wired")
        self.assertIsNone(
            smoke.check_record(wired, receiver_sha256="a" * 64, display_sha256="b" * 64,
                               phone_serial="SER", now=now, transport_name="wired")[0])

    def test_wireless_channel_mismatch_and_missing_fail_closed(self) -> None:
        now = datetime.now(timezone.utc)
        base_wireless = self._wireless_record(ap_interface="wlan0", ap_channel=44)
        for stage_ch in (36, None):
            self.assertEqual(
                smoke.check_record(base_wireless, receiver_sha256="a" * 64,
                                   display_sha256="b" * 64, phone_serial="SER", now=now,
                                   transport_name="wireless", phone_id=1,
                                   ap_channel=stage_ch, ap_interface="wlan0")[0],
                "drive_smoke_ap_channel_mismatch")
        self.assertIsNone(
            smoke.check_record(base_wireless, receiver_sha256="a" * 64,
                               display_sha256="b" * 64, phone_serial="SER", now=now,
                               transport_name="wireless", phone_id=1,
                               ap_channel=44, ap_interface="wlan0")[0])
        ch36 = self._wireless_record(ap_interface="wlan0", ap_channel=36)
        self.assertIsNone(
            smoke.check_record(ch36, receiver_sha256="a" * 64,
                               display_sha256="b" * 64, phone_serial="SER", now=now,
                               transport_name="wireless", phone_id=1,
                               ap_channel=None, ap_interface="wlan0")[0])
        self.assertEqual(
            smoke.check_record(ch36, receiver_sha256="a" * 64,
                               display_sha256="b" * 64, phone_serial="SER", now=now,
                               transport_name="wireless", phone_id=1,
                               ap_channel=44, ap_interface="wlan0")[0],
            "drive_smoke_ap_channel_mismatch")
        for bad in ({}, {"ap_channel": None}, {"ap_channel": True}, {"ap_channel": 52},
                    {"ap_interface": None}, {"ap_interface": ""}):
            doc = dict(base_wireless)
            if not bad:
                doc.pop("ap_channel")
            else:
                doc.update(bad)
            self.assertEqual(
                smoke.check_record(doc, receiver_sha256="a" * 64,
                                   display_sha256="b" * 64, phone_serial="SER", now=now,
                                   transport_name="wireless", phone_id=1,
                                   ap_channel=44, ap_interface="wlan0")[0],
                "drive_smoke_malformed", bad)

    def test_launcher_channel_mismatch_refused_before_collector(self) -> None:
        now = datetime.now(timezone.utc)
        with tempfile.TemporaryDirectory(prefix="launcher-ch-mismatch-") as directory:
            root = Path(directory)
            ch44_path = root / "ch44-smoke.json"
            ch44_path.write_text(json.dumps(self._wireless_record(
                phone_id=7, phone_serial=base.PHONE, ap_interface="wlan0", ap_channel=44,
                finished_utc=now.isoformat())))
            for extra in ((), ("--ap-channel", "36")):
                proc, ssh = base.LauncherGateTests.launch(
                    self, root, "--mode", "wireless", "--phone-id", "7",
                    "--receiver", "/opt/aa/aa-wired-receiver",
                    "--ap-interface", "wlan0", "--store", "/s/approved-phones.json",
                    "--drive-smoke", str(ch44_path), *extra)
                self.assertEqual(proc.returncode, 3, proc.stderr)
                out_doc = json.loads((root / "out.json").read_text())
                self.assertEqual(out_doc["blocked_on"], "drive_smoke_ap_channel_mismatch", extra)
                self.assertEqual((out_doc["ap_interface"], out_doc["ap_channel"]), ("wlan0", 36))
                self.assertNotIn("aa_projection_collect.py", ssh)
                self.assertNotIn("python3 -", ssh)

    def test_smoke_ap_fields_recorded_deterministically_including_early_blocked(self) -> None:
        # 1. Early-blocked wireless smokes (missing CMakeCache.txt -> unoptimized_build)
        #    must record ap_interface and ap_channel (explicit 44 and default 36).
        for ch_args, expected_ch in ((["--ap-channel", "44"], 44), ([], 36)):
            with tempfile.TemporaryDirectory(prefix="smoke-early-block-") as directory:
                root = Path(directory)
                build, state, out = root / "build", root / "state", root / "out"
                build.mkdir()
                state.mkdir()
                (build / "aa-wireless-receiver").write_text("#!/bin/sh\n")
                (build / "aa-dummy-display").write_text("#!/bin/sh\n")
                (state / "approved-phones.json").write_text("{}")
                rc = smoke.main(["run", "--transport", "wireless", "--phone-id", "7",
                                 "--ap-interface", "wlan0", *ch_args, "--serial", PHONE,
                                 "--build-dir", str(build),
                                 "--store", str(state / "approved-phones.json"),
                                 "--media-uri", "file:///sdcard/Music/fake.opus",
                                 "--media-component", "org.example.player/.Play",
                                 "--duration", "1", "--out", str(out)])
                result = json.loads((out / "result.json").read_text())
                self.assertEqual((rc, result["verdict"], result["blocked_on"]),
                                 (3, "BLOCKED", "unoptimized_build"))
                self.assertEqual((result["ap_interface"], result["ap_channel"]),
                                 ("wlan0", expected_ch))

        # 2. One deterministic wireless smoke integration run: receiver emits ACTIVE and blocks
        #    on signal.pause() until the runner stops it (zero child-exit vs log-thread race), with
        #    process-group cleanup in finally so an outer timeout can never orphan fixtures.
        def run_smoke_with_cleanup(cmd: list[str], *, env: dict, pid_files: tuple[Path, ...],
                                   timeout: float) -> tuple[int, str]:
            proc = subprocess.Popen(
                cmd, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                text=True, start_new_session=True)
            try:
                out_text, _ = proc.communicate(timeout=timeout)
                return proc.returncode, out_text
            finally:
                if proc.stdout is not None:
                    proc.stdout.close()
                for sig, wait_s in ((signal.SIGTERM, 2.0), (signal.SIGKILL, 2.0)):
                    try:
                        os.killpg(proc.pid, sig)
                    except (ProcessLookupError, PermissionError):
                        pass
                    try:
                        proc.wait(timeout=wait_s)
                        break
                    except subprocess.TimeoutExpired:
                        continue
                for pid_file in pid_files:
                    if not pid_file.exists():
                        continue
                    for token in pid_file.read_text().split():
                        try:
                            pid = int(token)
                        except ValueError:
                            continue
                        for kill_fn in (os.killpg, os.kill):
                            try:
                                kill_fn(pid, signal.SIGKILL)
                            except (ProcessLookupError, PermissionError):
                                pass

        with tempfile.TemporaryDirectory(prefix="smoke-controlled-") as directory:
            root = Path(directory)
            build, bin_dir, usb = root / "build", root / "bin", root / "usb"
            build.mkdir()
            bin_dir.mkdir()
            usb.mkdir()
            (build / "CMakeCache.txt").write_text("CMAKE_BUILD_TYPE:STRING=Release\n")
            rx_deterministic = (
                "#!/usr/bin/env python3\nimport os, signal, sys, time\n"
                + FAKE_SESSION
                + 'directory = os.environ["FAKE_DIR"]\n'
                + 'open(os.path.join(directory, "receiver.argv"), "w").write("\\n".join(sys.argv[1:]))\n'
                + 'open(os.path.join(directory, "receiver.pids"), "w").write(str(os.getpid()))\n'
                + 'print(\'{"event":"ready","state":"waiting_for_consumer"}\', flush=True)\n'
                + 'print(\'{"event":"wireless_admission"}\', flush=True)\n'
                + 'fake_session()\n'
                + 'print(\'{"kind":"session_state_changed","session":1,"mono_ns":%d,'
                + '"to_state":"active","transport":"usb"}\' % time.monotonic_ns(), flush=True)\n'
                + 'print(\'{"event":"video_profile","width":1280,"height":720,"fps":30,"codec":"h264"}\', flush=True)\n'
                + 'while True:\n    signal.pause()\n'
            )
            for path, body in ((build / "aa-wireless-receiver", rx_deterministic),
                               (build / "aa-dummy-display", FAKE_WIRELESS_DISPLAY),
                               (bin_dir / "adb", base.FAKE_ADB), (bin_dir / "ip", FAKE_IP)):
                path.write_text(body)
                path.chmod(0o755)
            state = root / "state"
            state.mkdir()
            (state / "approved-phones.json").write_text("{}")
            env = dict(os.environ, FAKE_DIR=str(root), DRIVE_HOOK="", PATH=f"{bin_dir}:/usr/bin:/bin",
                       FAKE_ADB_LOG=str(root / "adb.log"), FAKE_ADB_WIFI_STATUS=WIFI,
                       FAKE_USB=str(usb), FAKE_LEASES=str(root / "absent-{interface}.leases"),
                       FAKE_PROC=str(root / "proc"))
            pid_files = (root / "receiver.pids", root / "display.pids")
            rc, combined = run_smoke_with_cleanup(
                [sys.executable, "-B", "-c", DRIVER, str(base.SMOKE.parent), "run",
                 "--transport", "wireless", "--phone-id", "7", "--ap-interface", "wlan0",
                 "--ap-channel", "44", "--serial", PHONE, "--build-dir", str(build),
                 "--store", str(state / "approved-phones.json"), "--display-env", ":99",
                 "--media-uri", "file:///sdcard/Music/fake.opus",
                 "--media-component", "org.example.player/.Play", "--duration", "2",
                 "--out", str(root / "out")],
                env=env, pid_files=pid_files,
                timeout=int(smoke.READY_TIMEOUT_S + transport.WIRELESS_STARTUP_BUDGET_S + 30))
            result = json.loads((root / "out" / "result.json").read_text())
            run_doc = json.loads((root / "out" / "run.json").read_text())
            failed = sorted(c["name"] for c in result["criteria"] if not c["passed"])
            self.assertEqual((rc, result["verdict"], failed),
                             (1, "FAIL", ["av_signed_contract", "painted_fps_per_window"]),
                             combined)
            self.assertEqual((result["ap_interface"], result["ap_channel"]), ("wlan0", 44))
            self.assertEqual(run_doc["startup_budget_s"], float(transport.WIRELESS_STARTUP_BUDGET_S))

            # Forced-timeout regression: an outer timeout must kill the fake receiver and display
            # sessions too (zero orphaned processes).
            for pf in pid_files:
                pf.unlink(missing_ok=True)
            with self.assertRaises(subprocess.TimeoutExpired):
                run_smoke_with_cleanup(
                    [sys.executable, "-B", "-c", DRIVER, str(base.SMOKE.parent), "run",
                     "--transport", "wireless", "--phone-id", "7", "--ap-interface", "wlan0",
                     "--ap-channel", "44", "--serial", PHONE, "--build-dir", str(build),
                     "--store", str(state / "approved-phones.json"), "--display-env", ":99",
                     "--media-uri", "file:///sdcard/Music/fake.opus",
                     "--media-component", "org.example.player/.Play", "--duration", "30",
                     "--out", str(root / "out-timeout")],
                    env=env, pid_files=pid_files, timeout=2.0)
            recorded_pids = []
            for pf in pid_files:
                self.assertTrue(pf.exists(), f"missing pid file {pf}")
                recorded_pids.extend(int(t) for t in pf.read_text().split())
            self.assertEqual(len(recorded_pids), 2)
            for pid in recorded_pids:
                reap_until = time.monotonic() + 2.0
                while time.monotonic() < reap_until:
                    try:
                        os.kill(pid, 0)
                    except ProcessLookupError:
                        break
                    time_sleep(0.05)
                with self.assertRaises(ProcessLookupError, msg=f"orphaned fixture PID {pid}"):
                    os.kill(pid, 0)

        # 3. Deterministic v3 live capture -> result preserves AP settings and passes schema.
        import aa_perf_schema as schema
        live_cap = self._live_wireless_capture(ap_interface="wlan1", ap_channel=48, origin="live")
        res = report.build_result(live_cap, duration_requested="3s", now="2026-11-20T00:00:00Z",
                                  capture_path=None, capture_sha256=None)
        self.assertEqual((res["verdict"], res["qualification_status"]), ("PASS", "COMPLETE_LIVE"))
        self.assertEqual((res["ap_interface"], res["ap_channel"]), ("wlan1", 48))
        self.assertEqual(schema.validate(res, schema.load_schema(schema.schema_for(res["schema"]))), [])

    def test_startup_timeout_validation_and_derived_ceilings_across_entry_points(self) -> None:
        floor = transport.WIRELESS_STARTUP_BUDGET_S

        # 1. Soak chain: explicit --startup-timeout-seconds 0 is forwarded when calling stage_command,
        #    and invalid / overflowing / out-of-range values are rejected up front by main().
        args = SoakChainTransportTests().args(
            transport="wireless", phone_id=7, ap_interface="wlan0",
            store="/s/approved-phones.json", startup_timeout_seconds=0)
        cmd = chain.stage_command(args, 1800)
        idx = cmd.index("--startup-timeout-seconds")
        self.assertEqual(cmd[idx + 1], "0")

        max_chain_startup = collect.max_wireless_startup_timeout_seconds(max(chain.STAGES))
        with tempfile.TemporaryDirectory(prefix="chain-startup-val-") as directory:
            root = Path(directory)
            for idx_case, (tp, val) in enumerate((
                ("wireless", "0"),
                ("wireless", str(floor - 1)),
                ("wireless", str(max_chain_startup + 1)),
                ("wireless", "86400"),
                ("wired", "0"),
                ("wired", "61"),
            )):
                extra = (["--phone-id", "7", "--ap-interface", "wlan0", "--store", "/s/approved-phones.json"]
                         if tp == "wireless" else [])
                out_dir = root / f"chain-{idx_case}"
                rc = chain.main(["--out", str(out_dir), "--phone-serial", "S", "--drive-smoke", "d",
                                 "--media-uri", "u", "--media-component", "p/.a",
                                 "--transport", tp, *extra, "--startup-timeout-seconds", val])
                self.assertEqual(rc, 2, (tp, val))
                self.assertFalse(out_dir.exists(), (tp, val))

        # 2. Collector: ceiling derived from 86400 - duration - 150; wired stays in [1, 60].
        max_col = collect.max_wireless_startup_timeout_seconds(100)
        self.assertEqual(max_col, 86400 - 100 - 150)
        self.assertEqual(collect.startup_timeout_seconds("wireless", max_col, duration_seconds=100), max_col)
        for bad in (max_col + 1, 86400, floor - 1, 0, -5, 18446744073709551617):
            with self.assertRaises(ValueError):
                collect.startup_timeout_seconds("wireless", bad, duration_seconds=100)
        self.assertEqual(collect.startup_timeout_seconds("wired", 60, duration_seconds=100), 60)
        for bad in (0, -1, 61, 18446744073709551617):
            with self.assertRaises(ValueError):
                collect.startup_timeout_seconds("wired", bad, duration_seconds=100)

        # 3. Drive smoke: ceiling derived from 86400 - (2*duration + 25 + 15 + 30); rejects 1e308 cleanly.
        for bad in ("0", "-5", str(floor - 1), "86400", "1e308", "inf", "nan"):
            with self.assertRaises(SystemExit) as raised:
                smoke.main(["run", "--transport", "wireless", "--phone-id", "7",
                            "--ap-interface", "wlan0", "--serial", "S", "--build-dir", "/b",
                            "--store", "/s/approved-phones.json", "--media-uri", "u",
                            "--media-component", "p/.a", "--duration", "120", "--out", "/tmp/x",
                            "--startup-timeout-seconds", bad])
            self.assertEqual(raised.exception.code, 2, bad)
        with self.assertRaises(SystemExit) as raised:
            smoke.main(["run", "--transport", "wired", "--serial", "S", "--build-dir", "/b",
                        "--store", "/s/approved-phones.json", "--media-uri", "u",
                        "--media-component", "p/.a", "--out", "/tmp/x",
                        "--startup-timeout-seconds", "60"])
        self.assertEqual(raised.exception.code, 2)

        # 4. Perf launcher shell: rejects empty string, non-live modes, 64-bit overflow,
        #    values below derived floor or above duration-derived ceiling.
        with tempfile.TemporaryDirectory(prefix="launcher-startup-") as directory:
            root = Path(directory)
            dummy_cap = root / "cap.json"
            dummy_cap.write_text("{}")
            invalid_invocations = [
                ("--mode", "wireless", "--phone-id", "7", "--ap-interface", "wlan0",
                 "--store", "/s/approved-phones.json", "--duration", "30s",
                 "--startup-timeout-seconds", ""),
                ("--mode", "wireless", "--phone-id", "7", "--ap-interface", "wlan0",
                 "--store", "/s/approved-phones.json", "--duration", "30s",
                 "--startup-timeout-seconds", str(floor - 1)),
                ("--mode", "wireless", "--phone-id", "7", "--ap-interface", "wlan0",
                 "--store", "/s/approved-phones.json", "--duration", "7200s",
                 "--startup-timeout-seconds", "80000"),
                ("--mode", "wireless", "--phone-id", "7", "--ap-interface", "wlan0",
                 "--store", "/s/approved-phones.json", "--duration", "30s",
                 "--startup-timeout-seconds", "18446744073709551617"),
                ("--mode", "wired", "--duration", "30s", "--startup-timeout-seconds", "18446744073709551617"),
                ("--mode", "wired", "--duration", "30s", "--startup-timeout-seconds", "61"),
                ("--mode", "wired", "--duration", "30s", "--startup-timeout-seconds", "1e308"),
                ("--mode", "wired", "--readiness-only", "--startup-timeout-seconds", "30"),
                ("--mode", "wired", "--capture", str(dummy_cap), "--startup-timeout-seconds", "30"),
            ]
            for argv in invalid_invocations:
                proc = subprocess.run(
                    ["bash", str(base.LAUNCHER), "--out", str(root / "bad.json"), *argv],
                    capture_output=True, text=True, timeout=30)
                self.assertEqual(proc.returncode, 2, (argv, proc.stderr))

            good = self._wireless_record(
                phone_id=7, phone_serial=base.PHONE, ap_interface="wlan0", ap_channel=36,
                finished_utc=datetime.now(timezone.utc).isoformat())
            good_path = root / "good-smoke.json"
            good_path.write_text(json.dumps(good))
            proc, ssh = base.LauncherGateTests.launch(
                self, root, "--mode", "wireless", "--phone-id", "7", "--ap-interface", "wlan0",
                "--receiver", "/opt/aa/aa-wired-receiver",
                "--store", "/s/approved-phones.json", "--drive-smoke", str(good_path),
                "--startup-timeout-seconds", str(floor))
            self.assertIn(f"--startup-timeout-seconds '{floor}'", ssh, proc.stderr)


if __name__ == "__main__":
    unittest.main()
