# SPDX-License-Identifier: GPL-3.0-or-later
# /// script
# requires-python = ">=3.12"
# dependencies = []
# ///
# Run: python3 -B -m unittest discover -s tests/wired_qa -v
"""Wireless transport for the drive smoke and the soak chain, with fakes only.

No phone, Bluetooth, Wi-Fi, AP or helper is touched: the wireless receiver,
the display and adb are fakes, and the event streams are recorded shapes.
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

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(ROOT / "tools" / "perf"))
import test_drive_smoke as base  # noqa: E402
import aa_transport as transport  # noqa: E402
import aa_drive_smoke as smoke  # noqa: E402
import aa_projection_collect as collect  # noqa: E402
import aa_perf_report as report  # noqa: E402
import aa_soak_chain as chain  # noqa: E402

WIFI = ('Wifi is enabled\nWifi is connected to "AP"\nWifiInfo: SSID: "AP", BSSID: 02:00:00:00:00:00, '
        'MAC: 02:00:00:00:00:00, RSSI: -48, Link speed: 433Mbps, Frequency: 5180MHz, Net ID: 3')

# The wireless receiver's recorded startup shape: ready, then (with --diagnostics)
# the session's own state change to active; no "active" line.
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
print('{"event":"diag","kind":"session_state_changed","mono_ns":%d,"to_state":"active",'
      '"transport":"usb"}' % time.monotonic_ns(), flush=True)
print('{"event":"video_profile","width":1280,"height":720,"fps":30,"codec":"h264"}', flush=True)
time.sleep(300)
"""


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

    def test_usb_state_refusals(self) -> None:
        self.assertIsNone(transport.wireless_usb_refusal({"usb_aoa": False}))
        self.assertEqual(transport.wireless_usb_refusal({"usb_aoa": True}), "usb_in_aoa")
        self.assertEqual(transport.wireless_usb_refusal({"usb_aoa": None}), "usb_functions_unknown")
        self.assertTrue(transport.usb_in_aoa(transport.usb_functions("accessory,adb")))
        self.assertIsNone(transport.usb_in_aoa(transport.usb_functions("")))

    def test_adb_link(self) -> None:
        self.assertEqual(transport.adb_link("SER device usb:1-2.1 model:X"), "usb")
        self.assertEqual(transport.adb_link("192.168.4.2:5555 device model:X"), "tcp")

    def test_a_record_without_transport_is_wired(self) -> None:
        self.assertEqual(transport.record_transport({}), "wired")
        self.assertEqual(transport.record_transport({"transport": "wireless"}), "wireless")


class GateTransportTests(unittest.TestCase):
    """A wired smoke never qualifies a wireless soak, nor the reverse."""

    def record(self, **fields) -> dict:
        record = {"schema": smoke.SCHEMA, "verdict": "PASS", "duration_s": 600,
                  "finished_utc": datetime.now(timezone.utc).isoformat(), "video_profiles": None,
                  "phone_serial": "SER", "configuration": dict(smoke.DEFAULT_CONFIGURATION),
                  "binaries": {"receiver": {"sha256": "a" * 64}, "display": {"sha256": "b" * 64}}}
        record.update(fields)
        return record

    def check(self, record: dict, name: str) -> str | None:
        return smoke.check_record(record, receiver_sha256="a" * 64, display_sha256="b" * 64,
                                  phone_serial="SER", now=datetime.now(timezone.utc),
                                  transport_name=name)[0]

    def test_matching_transport_admits(self) -> None:
        self.assertIsNone(self.check(self.record(transport="wireless"), "wireless"))
        self.assertIsNone(self.check(self.record(transport="wired"), "wired"))

    def test_cross_transport_is_refused(self) -> None:
        self.assertEqual(self.check(self.record(transport="wired"), "wireless"),
                         "drive_smoke_transport_mismatch")
        self.assertEqual(self.check(self.record(transport="wireless"), "wired"),
                         "drive_smoke_transport_mismatch")
        # A legacy record (no field) is wired, so it never qualifies wireless.
        self.assertEqual(self.check(self.record(), "wireless"), "drive_smoke_transport_mismatch")
        self.assertIsNone(self.check(self.record(), "wired"))

    def test_wireless_harness_flags_are_not_configuration(self) -> None:
        config = smoke.configuration(
            ["rx", "--store-dir", "/s", "--phone-id", "3", "--diagnostics", "--video-socket", "/v",
             "--input-socket", "/i", "--duration-ms", "1"], ["d", "--test-only-autostart"])
        self.assertEqual(config, smoke.DEFAULT_CONFIGURATION)


class WirelessRunnerTests(unittest.TestCase):
    """The real runner, --transport wireless, against fakes."""

    def run_smoke(self, root: Path, **env_extra: str) -> tuple[int, str]:
        build, bin_dir = root / "build", root / "bin"
        build.mkdir()
        bin_dir.mkdir()
        (build / "CMakeCache.txt").write_text("CMAKE_BUILD_TYPE:STRING=Release\n")
        for path, body in ((build / "aa-wireless-receiver", FAKE_WIRELESS_RECEIVER),
                           (build / "aa-dummy-display", base.FAKE_DISPLAY), (bin_dir / "adb", base.FAKE_ADB)):
            path.write_text(body)
            path.chmod(0o755)
        store = root / "state" / "approved-phones.json"
        store.parent.mkdir()
        store.write_text("{}")
        env = dict(os.environ, FAKE_DIR=str(root), DRIVE_HOOK="", PATH=f"{bin_dir}:/usr/bin:/bin",
                   FAKE_ADB_LOG=str(root / "adb.log"), FAKE_ADB_WIFI_STATUS=WIFI,
                   FAKE_ADB_DEVICE_LINE="FAKESERIAL device usb:3-1 model:FAKE")
        env.update(env_extra)
        runner = subprocess.Popen(
            [sys.executable, "-B", "-c", base.DRIVER, str(base.SMOKE.parent), "run",
             "--transport", "wireless", "--phone-id", "7",
             "--serial", "FAKESERIAL", "--build-dir", str(build), "--store", str(store),
             "--display-env", ":99", "--media-uri", "file:///sdcard/Music/fake.opus",
             "--media-component", "org.example.player/.Play", "--duration", "3",
             "--out", str(root / "out")], env=env, stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT, text=True)
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

    def test_wireless_session_launch_record_and_teardown(self) -> None:
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
        self.assertNotIn("--require-video-consumer", argv)
        # USB is never switched (no AOA/mtp restore); it is read before and after.
        self.assertFalse([line for line in adb if "setFunctions" in line], adb)
        self.assertEqual(sum("getprop sys.usb.config" in line for line in adb), 2, adb)
        self.assertEqual(result["transport"], "wireless")
        self.assertIsNone(result["port"])
        self.assertEqual(result["adb_link"], "usb")
        for moment in ("before", "after"):
            link = result["link"][moment]
            self.assertEqual((link["usb_functions"], link["usb_aoa"], link["bluetooth_on"]),
                             (["mtp", "adb"], False, True))
            self.assertEqual(link["wifi"], {"connected": True, "frequency_mhz": 5180,
                                            "channel": 36, "rssi_dbm": -48})
        self.assertEqual(run["active_marker"], "diag_active")
        self.assertEqual(result["configuration"], smoke.DEFAULT_CONFIGURATION)
        # The same criteria judge it: this fake has no audio, so the A/V contract fails.
        self.assertNotEqual(result["verdict"], "PASS")
        self.assertFalse(base.criterion(result, "av_signed_contract")["passed"])
        self.assertEqual(code, 1, output)

    def test_phone_in_aoa_blocks_before_launch(self) -> None:
        with tempfile.TemporaryDirectory(prefix="drive-smoke-wireless-aoa-") as directory:
            root = Path(directory)
            code, output = self.run_smoke(root, FAKE_ADB_USB_CONFIG="accessory,adb")
            result = json.loads((root / "out" / "result.json").read_text())
            launched = (root / "receiver.argv").exists()
        self.assertEqual((code, result["verdict"], result["blocked_on"]), (3, "BLOCKED", "usb_in_aoa"),
                         output)
        self.assertEqual(result["transport"], "wireless")
        self.assertFalse(launched)

    def test_unknown_usb_state_blocks(self) -> None:
        with tempfile.TemporaryDirectory(prefix="drive-smoke-wireless-usb-") as directory:
            root = Path(directory)
            code, output = self.run_smoke(root, FAKE_ADB_USB_CONFIG="")
            result = json.loads((root / "out" / "result.json").read_text())
        self.assertEqual((code, result["blocked_on"]), (3, "usb_functions_unknown"), output)

    def test_wireless_needs_a_phone_id(self) -> None:
        with self.assertRaises(SystemExit) as raised:
            smoke.main(["run", "--transport", "wireless", "--serial", "S", "--build-dir", "/b",
                        "--media-uri", "u", "--media-component", "p/.a", "--out", "/tmp/x"])
        self.assertEqual(raised.exception.code, 2)


class CollectorTransportTests(unittest.TestCase):
    def test_wireless_active_diagnostic_sets_startup_and_state(self) -> None:
        parser = collect.EventParser()
        parser.receiver_started_ns = 1_000_000_000
        parser._consume('{"event":"ready","state":"waiting_for_consumer"}')
        self.assertFalse(parser.projection_active)
        parser._consume('{"event":"diag","kind":"session_state_changed","mono_ns":3500000000,'
                        '"to_state":"active"}')
        self.assertTrue(parser.projection_active)
        self.assertEqual(parser.startup_ms, 2500)
        parser._consume('{"event":"diag","kind":"session_state_changed","mono_ns":4000000000,'
                        '"to_state":"disconnected"}')
        self.assertFalse(parser.projection_active)
        self.assertEqual(parser.startup_ms, 2500)

    def test_wired_active_line_is_unchanged(self) -> None:
        parser = collect.EventParser()
        parser._consume('{"event":"active","startup_ms":812}')
        self.assertEqual((parser.projection_active, parser.startup_ms), (True, 812))

    def test_wireless_cli_needs_phone_id_and_store(self) -> None:
        self.assertEqual(collect._cli(["--receiver", "r", "--port", "x", "--duration-seconds", "1",
                                       "--transport", "wireless"]), 2)


class ReporterTransportTests(unittest.TestCase):
    def snapshot(self, aoa: bool | None) -> dict:
        return {"usb_functions": ["mtp"], "usb_aoa": aoa, "wifi": {}, "bluetooth_on": True}

    def test_wireless_link_gap(self) -> None:
        gap = report.wireless_link_gap
        self.assertIsNone(gap({"mode": "wired"}))
        self.assertEqual(gap({"mode": "wireless"}), "wireless_link_unrecorded")
        ok = self.snapshot(False)
        self.assertIsNone(gap({"mode": "wireless", "link": {"before": ok, "after": ok}}))
        self.assertEqual(gap({"mode": "wireless", "link": {"before": ok, "after": None}}),
                         "wireless_usb_unknown_after")
        self.assertEqual(gap({"mode": "wireless", "link": {"before": self.snapshot(True), "after": ok}}),
                         "wireless_usb_in_aoa_before")


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
