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
  - timeout parse violations are controlled usage errors (exit 2), never SIGABRT.
"""
import json
import subprocess
import sys
from pathlib import Path
from tempfile import TemporaryDirectory
import unittest

TIMEOUT_S = 20
USAGE_EXIT = 2


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


class AbsentJson(unittest.TestCase):
    binary = ""

    def test_absent_device_emits_parseable_json(self):
        with TemporaryDirectory() as directory:
            result = run_probe(self.binary, [
                "--port", "999-999", "--pre-vid", "04e8", "--pre-pid", "6860",
                "--identity-file", identity_file(directory), "--no-recover-reset"])
        self.assertEqual(result.returncode, USAGE_EXIT, result.stdout + result.stderr)
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


if __name__ == "__main__":
    if len(sys.argv) < 3:
        raise SystemExit("usage: test_probe_binary.py <case> <probe-binary>")
    groups = {"absent_json": AbsentJson, "hostile_port": HostilePort, "timeout_parse": TimeoutParse}
    selected = groups[sys.argv[1]]
    selected.binary = sys.argv[2]
    suite = unittest.defaultTestLoader.loadTestsFromTestCase(selected)
    sys.exit(not unittest.TextTestRunner(verbosity=2).run(suite).wasSuccessful())
