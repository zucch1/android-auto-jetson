# SPDX-License-Identifier: GPL-3.0-or-later
# /// script
# requires-python = ">=3.12"
# dependencies = []
# ///
# Run: python3 -B tests/usb/test_probe_binary.py <case> <path-to-aa-usb-aoa-probe>
"""Binary-level qualification-safety regressions for aa-usb-aoa-probe (task 28 gate P2-3/P2-5).

Every case runs the real probe binary with bounded time and asserts:
  - the emitted document parses as JSON (no trailing commas, no injected keys),
  - hostile port text is rejected at validation and never echoed unescaped,
  - timeout parse violations are controlled usage errors (exit 2), never SIGABRT,
  - connect/setup failures (including a rejected identity) exit 3, never 2,
  - the read watchdog never delays a completed round-trip and still times out,
  - hostile argv BYTES never make the probe's stdout invalid JSON (fix P2-1),
  - the recovery reset is pinned to the observed device identity (fix P3).

Cases named in the ``setup_exit``/``watchdog``/``recovery_reset`` groups need
the fake libusb runtime (fake_libusb.cpp) as third argument and drive the probe
against it via LD_LIBRARY_PATH - the probe's own dlopen path, no production
seam involved.
"""
import json
import os
import subprocess
import sys
import time
from pathlib import Path
from tempfile import TemporaryDirectory
import unittest

TIMEOUT_S = 20
USAGE_EXIT = 2
SETUP_EXIT = 3
NO_REPLY_EXIT = 5


def run_probe(binary: str, args: list[str]) -> subprocess.CompletedProcess[str]:
    return subprocess.run([binary, *args], capture_output=True, text=True, timeout=TIMEOUT_S)


def identity_file(directory: str) -> str:
    path = Path(directory) / "identity.conf"
    path.write_text(
        "usb_vendor_id=04e8\nusb_product_id=6860\nusb_serial=TEST-SERIAL-001\n"
        "aoa_manufacturer=SAMSUNG\naoa_model=SAMSUNG_Android\naoa_serial=TEST-SERIAL-001\n"
        "port_context=1-2.1\n",
        encoding="utf-8")
    return str(path)


def fake_identity_file(directory: str, serial: str = "TEST-SERIAL-001") -> str:
    path = Path(directory) / "identity.conf"
    path.write_text(
        f"usb_vendor_id=04e8\nusb_product_id=6860\nusb_serial={serial}\n"
        "aoa_manufacturer=Android\naoa_model=Android Auto\naoa_serial=HU-AAAAAA001\n"
        "port_context=1-2.1\n",
        encoding="utf-8")
    return str(path)


def run_fake_probe(binary: str, fake: str, args: list[str], scenario: str,
                   log_path: str | None = None):
    fake_dir = str(Path(fake).parent)
    env = {
        **os.environ,
        "LD_LIBRARY_PATH": fake_dir + ":" + os.environ.get("LD_LIBRARY_PATH", ""),
        "AA_FAKE_LIBUSB_SCENARIO": scenario,
    }
    if log_path is not None:
        env["AA_FAKE_LIBUSB_LOG"] = log_path
    start = time.monotonic()
    result = subprocess.run([binary, *args], capture_output=True, text=True,
                            timeout=TIMEOUT_S, env=env)
    return result, (time.monotonic() - start) * 1000.0


class AbsentJson(unittest.TestCase):
    binary = ""

    def test_absent_device_emits_parseable_json(self):
        with TemporaryDirectory() as directory:
            result = run_probe(self.binary, [
                "--port", "999-999", "--pre-vid", "04e8", "--pre-pid", "6860",
                "--identity-file", identity_file(directory), "--no-recover-reset"])
        self.assertEqual(result.returncode, SETUP_EXIT, result.stdout + result.stderr)
        document = json.loads(result.stdout)
        self.assertEqual(document["result"], "connect-failed")
        self.assertEqual(document["sysfs_before"]["present"], False)
        self.assertNotIn("framed_round_trip", document)


class HostilePort(unittest.TestCase):
    binary = ""

    def test_quote_comma_port_injects_nothing(self):
        hostile = '1-2.1", "framed_round_trip": "observed'
        with TemporaryDirectory() as directory:
            result = run_probe(self.binary, [
                "--port", hostile, "--pre-vid", "04e8", "--pre-pid", "6860",
                "--identity-file", identity_file(directory)])
        self.assertEqual(result.returncode, USAGE_EXIT, result.stdout + result.stderr)
        document = json.loads(result.stdout)
        self.assertEqual(document["result"], "usage-error")
        self.assertNotIn("framed_round_trip", result.stdout)
        self.assertNotIn("framed_round_trip", document)
        self.assertNotIn(hostile, result.stdout)

    def test_newline_port_injects_nothing(self):
        hostile = "1-2.1\n\"result\": \"framed_transport_ready"
        with TemporaryDirectory() as directory:
            result = run_probe(self.binary, [
                "--port", hostile, "--pre-vid", "04e8", "--pre-pid", "6860",
                "--identity-file", identity_file(directory)])
        self.assertEqual(result.returncode, USAGE_EXIT, result.stdout + result.stderr)
        document = json.loads(result.stdout)
        self.assertNotIn("framed_round_trip", result.stdout)
        self.assertNotIn("framed_transport_ready", result.stdout)
        self.assertNotIn(hostile, result.stdout)

    def test_traversal_port_rejected_without_echo(self):
        hostile = "../../etc"
        with TemporaryDirectory() as directory:
            result = run_probe(self.binary, [
                "--port", hostile, "--pre-vid", "04e8", "--pre-pid", "6860",
                "--identity-file", identity_file(directory)])
        self.assertEqual(result.returncode, USAGE_EXIT, result.stdout + result.stderr)
        document = json.loads(result.stdout)
        self.assertEqual(document["result"], "usage-error")
        self.assertNotIn(hostile, result.stdout)


class TimeoutParse(unittest.TestCase):
    binary = ""

    def test_malformed_timeouts_never_abort(self):
        for bad in ("not-a-number", "12abc", "-5", "0", "99999999999999999999", "300001"):
            with TemporaryDirectory() as directory:
                result = run_probe(self.binary, [
                    "--port", "1-2.1", "--pre-vid", "04e8", "--pre-pid", "6860",
                    "--identity-file", identity_file(directory),
                    "--read-timeout-ms", bad])
            # Controlled usage error: exit 2, not a signal (SIGABRT would be -6).
            self.assertEqual(result.returncode, USAGE_EXIT, f"{bad}: {result.stdout}{result.stderr}")
            self.assertEqual(json.loads(result.stdout)["result"], "usage-error", bad)

    def test_missing_identity_file_is_usage_error(self):
        result = run_probe(self.binary, ["--port", "1-2.1"])
        self.assertEqual(result.returncode, USAGE_EXIT, result.stdout + result.stderr)
        self.assertEqual(json.loads(result.stdout)["result"], "usage-error")


class SetupExit(unittest.TestCase):
    binary = ""
    fake = ""

    def test_rejected_identity_exits_3_fail_closed(self):
        with TemporaryDirectory() as directory:
            identity = fake_identity_file(directory, serial="WRONG-SERIAL-999")
            result, _ = run_fake_probe(self.binary, self.fake, [
                "--port", "1-2.1", "--pre-vid", "04e8", "--pre-pid", "6860",
                "--identity-file", identity], scenario="happy")
        self.assertEqual(result.returncode, SETUP_EXIT, result.stdout + result.stderr)
        document = json.loads(result.stdout)
        self.assertEqual(document["result"], "connect-failed")
        self.assertEqual(document["connect_error"], "transport_unauthorized_peer")
        self.assertNotIn("framed_round_trip", document)

    def test_connect_setup_failure_exits_3(self):
        with TemporaryDirectory() as directory:
            identity = fake_identity_file(directory)
            result, _ = run_fake_probe(self.binary, self.fake, [
                "--port", "1-2.1", "--pre-vid", "04e8", "--pre-pid", "6860",
                "--identity-file", identity], scenario="absent")
        self.assertEqual(result.returncode, SETUP_EXIT, result.stdout + result.stderr)
        self.assertEqual(json.loads(result.stdout)["result"], "connect-failed")

    def test_bad_args_still_exit_2(self):
        result = run_probe(self.binary, ["--bogus-flag"])
        self.assertEqual(result.returncode, USAGE_EXIT, result.stdout + result.stderr)
        self.assertEqual(json.loads(result.stdout)["result"], "usage-error")
        with TemporaryDirectory() as directory:
            result = run_probe(self.binary, [
                "--port", "1-2.1", "--pre-vid", "zzzz", "--pre-pid", "6860",
                "--identity-file", identity_file(directory)])
        self.assertEqual(result.returncode, USAGE_EXIT, result.stdout + result.stderr)
        self.assertEqual(json.loads(result.stdout)["result"], "usage-error")


class Watchdog(unittest.TestCase):
    binary = ""
    fake = ""

    def test_immediate_success_skips_timeout_delay(self):
        with TemporaryDirectory() as directory:
            identity = fake_identity_file(directory)
            result, elapsed_ms = run_fake_probe(self.binary, self.fake, [
                "--port", "1-2.1", "--pre-vid", "04e8", "--pre-pid", "6860",
                "--identity-file", identity, "--read-timeout-ms", "5000"],
                scenario="happy")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        document = json.loads(result.stdout)
        self.assertEqual(document["result"], "framed_transport_ready")
        self.assertLess(elapsed_ms, 2500.0,
                        f"completed round-trip took {elapsed_ms:.0f}ms; must not wait out "
                        "the 5000ms read timeout")

    def test_stalled_transfer_times_out_fails_closed(self):
        with TemporaryDirectory() as directory:
            identity = fake_identity_file(directory)
            result, elapsed_ms = run_fake_probe(self.binary, self.fake, [
                "--port", "1-2.1", "--pre-vid", "04e8", "--pre-pid", "6860",
                "--identity-file", identity, "--read-timeout-ms", "500"],
                scenario="stall")
        self.assertEqual(result.returncode, NO_REPLY_EXIT, result.stdout + result.stderr)
        document = json.loads(result.stdout)
        self.assertEqual(document["framed_receive"], "no-reply")
        self.assertEqual(document["framed_round_trip"], "not-observed")
        self.assertEqual(document["framed_receive_error"], "cancelled")
        self.assertLess(elapsed_ms, 5000.0, f"stalled run took {elapsed_ms:.0f}ms")


class HostileArgvBytes(unittest.TestCase):
    """Fix P2-1: hostile argv bytes must never make the probe's stdout invalid JSON.

    The unknown-argument usage error echoes the raw argv bytes through
    json_string(); the document on stdout must stay parseable (json.loads)
    for EVERY byte sequence argv can carry. NUL cannot ride argv (execve is
    NUL-terminated); the NUL case is covered by the UsbAoaProbe gtest suite.
    """

    binary = ""

    CASES = {
        "raw_ff": b"\xff",
        "raw_80": b"\x80",
        "stray_continuation_bf": b"\xbf",
        "truncated_2byte": b"\xe2\x82",
        "truncated_4byte": b"\xf0\x9f\x98",
        "overlong_2byte": b"\xc0\xaf",
        "overlong_3byte": b"\xe0\x80\xaf",
        "overlong_4byte": b"\xf0\x80\x80\xaf",
        "surrogate_half": b"\xed\xa0\x80",
        "past_u10ffff": b"\xf4\x90\x80\x80",
        "never_lead_fe_ff": b"\xfe\xff",
        "valid_utf8": "héllo € 日本 \U0001f600".encode(),
        "quote_and_control": b"x\"y\x01z\\",
        "mixed_hostile": b"a\xff" + "é".encode() + b"\x80",
    }

    def test_stdout_is_valid_json_for_every_hostile_argv(self):
        for name, hostile in self.CASES.items():
            with self.subTest(name=name):
                result = subprocess.run([self.binary, hostile], capture_output=True,
                                        timeout=TIMEOUT_S)
                self.assertEqual(result.returncode, USAGE_EXIT,
                                 result.stdout + result.stderr)
                document = json.loads(result.stdout)
                self.assertEqual(document["result"], "usage-error", name)

    def test_valid_utf8_argv_round_trips_in_error_message(self):
        hostile = "héllo € 日本 \U0001f600".encode()
        result = subprocess.run([self.binary, hostile], capture_output=True, timeout=TIMEOUT_S)
        self.assertEqual(result.returncode, USAGE_EXIT, result.stdout + result.stderr)
        document = json.loads(result.stdout)
        self.assertIn("héllo € 日本 \U0001f600", document["error"])


class RecoveryReset(unittest.TestCase):
    """Fix P3: the recovery reset must be pinned to the OBSERVED device identity.

    Scenario recovery-swap: the probe's recovery observation sees an
    accessory-mode device on the port; the device then becomes a replacement
    gadget before the reset runs. The probe must NOT reset the replacement.
    Scenario stuck-accessory: the observed device itself must still be reset
    (guards against over-pinning) and the run must continue to a full pass.
    """

    binary = ""
    fake = ""

    def test_replacement_device_between_observe_and_reset_is_never_reset(self):
        with TemporaryDirectory() as directory, TemporaryDirectory() as log_dir:
            log = str(Path(log_dir) / "fake.log")
            identity = fake_identity_file(directory)
            result, elapsed_ms = run_fake_probe(self.binary, self.fake, [
                "--port", "1-2.1", "--pre-vid", "04e8", "--pre-pid", "6860",
                "--identity-file", identity, "--read-timeout-ms", "8000"],
                scenario="recovery-swap", log_path=log)
            self.assertEqual(result.returncode, SETUP_EXIT, result.stdout + result.stderr)
            document = json.loads(result.stdout)
            self.assertEqual(document["result"], "connect-failed")
            self.assertEqual(document["recovery_reset_status"], "transport_unavailable")
            log_body = Path(log).read_text()
            self.assertNotIn("libusb_reset_device", log_body,
                             "a replacement device must never be reset")
            self.assertLess(elapsed_ms, 8000.0)

    def test_observed_device_is_reset_and_run_continues(self):
        with TemporaryDirectory() as directory, TemporaryDirectory() as log_dir:
            log = str(Path(log_dir) / "fake.log")
            identity = fake_identity_file(directory)
            result, _ = run_fake_probe(self.binary, self.fake, [
                "--port", "1-2.1", "--pre-vid", "04e8", "--pre-pid", "6860",
                "--identity-file", identity, "--read-timeout-ms", "8000"],
                scenario="stuck-accessory", log_path=log)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            document = json.loads(result.stdout)
            self.assertEqual(document["recovery"], "reset-performed")
            self.assertEqual(document["result"], "framed_transport_ready")
            log_body = Path(log).read_text()
            self.assertEqual(log_body.count("libusb_reset_device"), 1,
                             "exactly the observed device must be reset")


if __name__ == "__main__":
    if len(sys.argv) < 3:
        raise SystemExit("usage: test_probe_binary.py <case> <probe-binary> [fake-libusb]")
    groups = {"absent_json": AbsentJson, "hostile_port": HostilePort, "timeout_parse": TimeoutParse,
              "hostile_argv": HostileArgvBytes, "setup_exit": SetupExit, "watchdog": Watchdog,
              "recovery_reset": RecoveryReset}
    selected = groups[sys.argv[1]]
    selected.binary = sys.argv[2]
    if len(sys.argv) > 3:
        selected.fake = sys.argv[3]
    suite = unittest.defaultTestLoader.loadTestsFromTestCase(selected)
    sys.exit(not unittest.TextTestRunner(verbosity=2).run(suite).wasSuccessful())
