# SPDX-License-Identifier: GPL-3.0-or-later
# /// script
# requires-python = ">=3.12"
# dependencies = []
# ///
# Run: python3 -B -m unittest discover -s tests/wired_qa -v
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
import unittest
from datetime import datetime, timezone
from pathlib import Path
from time import sleep as time_sleep

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
sys.path.insert(0, str(HERE))
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


# Startup as on main: ready, admission, then the bare diagnostic; no "active" line.
FAKE_WIRELESS_RECEIVER = """#!/usr/bin/env python3
import os, subprocess, sys, time
directory = os.environ["FAKE_DIR"]
with open(os.path.join(directory, "receiver.argv"), "w") as handle:
    handle.write("\\n".join(sys.argv[1:]))
grandchild = subprocess.Popen(["sleep", "300"])
with open(os.path.join(directory, "receiver.pids"), "w") as handle:
    handle.write(f"{os.getpid()} {grandchild.pid}")
print('{"event":"ready","state":"waiting_for_consumer"}', flush=True)
time.sleep(0.5)
print('{"event":"wireless_admission"}', flush=True)
print('{"kind":"session_state_changed","session":1,"mono_ns":%d,"to_state":"active","transport":"usb"}'
      % time.monotonic_ns(), flush=True)
print('{"event":"video_profile","width":1280,"height":720,"fps":30,"codec":"h264"}', flush=True)
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

# The runner with the Jetson's USB bus and the AP lease file pointed at fakes.
DRIVER = """import os, signal, subprocess, sys, time
from pathlib import Path
sys.path.insert(0, sys.argv[1])
import aa_drive_smoke
aa_drive_smoke.transport.USB_DEVICES = Path(os.environ["FAKE_USB"])
aa_drive_smoke.transport.NM_LEASES = os.environ["FAKE_LEASES"]
aa_drive_smoke.ADB_BIND_TIMEOUT_S = 2.0
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

    def test_ap_interface(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            conf = Path(directory) / "ap.conf"
            conf.write_text("v1 wlan0 US jurisdiction-unconfirmed 5277\n")
            self.assertEqual(transport.ap_interface(conf), "wlan0")
            conf.write_text("v2 wlan0\n")
            self.assertIsNone(transport.ap_interface(conf))

    def test_bind_adb_by_serial(self) -> None:
        calls: list[tuple] = []

        def adb(*args):
            calls.append(args)
            target = args[1] if args[0] == "-s" else args[-1]
            if args[0] == "connect":
                return subprocess.CompletedProcess(args, 0, f"connected to {target}\n", "")
            if args[-1] == "get-serialno":
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

    def check(self, record: dict, name: str, phone_id: int | None = None) -> str | None:
        return smoke.check_record(record, receiver_sha256="a" * 64, display_sha256="b" * 64,
                                  phone_serial="SER", now=datetime.now(timezone.utc),
                                  transport_name=name, phone_id=phone_id)[0]

    def test_matching_transport_admits(self) -> None:
        self.assertIsNone(self.check(self.record(transport="wireless", phone_id=1), "wireless", 1))
        self.assertIsNone(self.check(self.record(transport="wired"), "wired"))

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
        self.assertEqual(self.check(self.record(transport="wireless", phone_id=1), "wireless", 2),
                         "drive_smoke_phone_mismatch")
        self.assertEqual(self.check(self.record(transport="wireless", phone_id=1), "wireless", None),
                         "drive_smoke_phone_mismatch")
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
                           (build / "aa-dummy-display", base.FAKE_DISPLAY),
                           (bin_dir / "adb", base.FAKE_ADB), (bin_dir / "ip", FAKE_IP)):
            path.write_text(body)
            path.chmod(0o755)
        state = root / "state"
        state.mkdir()
        (state / "approved-phones.json").write_text("{}")
        (root / "ap.conf").write_text("v1 wlan0 US jurisdiction-unconfirmed 5277\n")
        env = dict(os.environ, FAKE_DIR=str(root), DRIVE_HOOK="", PATH=f"{bin_dir}:/usr/bin:/bin",
                   FAKE_ADB_LOG=str(root / "adb.log"), FAKE_ADB_WIFI_STATUS=WIFI,
                   FAKE_USB=str(usb), FAKE_LEASES=str(root / "absent-{interface}.leases"))
        env.update(env_extra)
        runner = subprocess.Popen(
            [sys.executable, "-B", "-c", DRIVER, str(base.SMOKE.parent), "run",
             "--transport", "wireless", "--phone-id", "7", "--ap-conf", str(root / "ap.conf"),
             "--serial", PHONE, "--build-dir", str(build),
             "--store", store or str(state / "approved-phones.json"),
             "--display-env", ":99", "--media-uri", "file:///sdcard/Music/fake.opus",
             "--media-component", "org.example.player/.Play", "--duration", "3",
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
        # The bare active diagnostic (main's shape) started the drive.
        self.assertEqual(run["active_marker"], "diag_active", output)
        self.assertGreater(run["drive_from_ns"], run["active_ns"])
        # adb over the AP link, bound by get-serialno before anything is sent.
        target = f"{STATION}:5555"
        self.assertEqual(adb[:2], [f"connect {target}", f"-s {target} get-serialno"], adb)
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
        # The same criteria judge it: this fake plays no audio, so the A/V contract fails.
        self.assertFalse(base.criterion(result, "av_signed_contract")["passed"])
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
print('{"kind":"session_state_changed","session":1,"mono_ns":%d,"to_state":"active","transport":"usb"}'
      % time.monotonic_ns(), flush=True)
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
        (root / "ap.conf").write_text("v1 wlan0 US jurisdiction-unconfirmed 5277\n")
        env = {"PATH": f"{bin_dir}:{os.environ['PATH']}", "FAKE_ADB_LOG": str(root / "adb.log"),
               "FAKE_RX_ARGV": str(root / "rx.argv"), "FAKE_ADB_WIFI_STATUS": WIFI, **env_extra}
        args = argparse.Namespace(
            receiver=str(bin_dir / "rx"), display=str(bin_dir / "display"), no_display=False,
            port="1-2.1", duration_seconds=3, interval=1, startup_timeout_seconds=10,
            store="state/approved-phones.json", video_socket=None, input_socket=None,
            state_dir=str(root / "logs"), phone_serial=PHONE, workload="moving-map",
            media_uri="file:///sdcard/Music/fixture.opus", media_type=None,
            media_component="org.example.player/.Play", geo_uri=None,
            thermal_policy="strict-all-zones", transport="wireless", phone_id=7,
            ap_conf=str(root / "ap.conf"))
        real_sleep = time_sleep
        with patch.dict(os.environ, env), \
                patch.object(collect, "_display_connected", return_value=True), \
                patch.object(collect, "build_processes", return_value=[]), \
                patch.object(collect, "_ADB_BIND_TIMEOUT_SECONDS", 2.0), \
                patch.object(collect, "_HOST_USB_PERIOD_SECONDS", 0.5), \
                patch.object(transport, "USB_DEVICES", usb), \
                patch.object(transport, "NM_LEASES", str(root / "absent-{interface}.leases")), \
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
        target = f"{STATION}:5555"
        self.assertEqual(adb[:2], [f"connect {target}", f"-s {target} get-serialno"], adb)
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
        self.assertFalse([line for line in adb if " shell " in line], adb)


class ReporterTransportTests(unittest.TestCase):
    host = {"readable": True, "phone_attached": False, "aoa_attached": False}

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
                      display_env=None, phone_id=None, transport="wired")
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

    def test_wireless_chain_needs_phone_id(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            self.assertEqual(chain.main(["--out", str(Path(directory) / "c"), "--phone-serial", "S",
                                         "--drive-smoke", "d", "--media-uri", "u",
                                         "--media-component", "p/.a", "--transport", "wireless"]), 2)


if __name__ == "__main__":
    unittest.main()
