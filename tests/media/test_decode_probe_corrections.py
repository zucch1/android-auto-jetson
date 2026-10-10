#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import time


def test_argument_errors_when_numeric_input_is_invalid(binary: str, fixture: str) -> None:
    for argument in ("--duration=1junk", "--profile=184467440737095516160x720@30", "--bitrate=10junk"):
        result = subprocess.run([binary, argument, "--fixture", fixture], capture_output=True, text=True, timeout=3)
        report = json.loads(result.stdout)
        assert result.returncode == 1 and report["pass"] is False
    print("PASS: controlled numeric argument errors")


def test_json_when_missing_fixture_has_control_bytes(binary: str, fixture: str) -> None:
    del fixture
    result = subprocess.run([binary, "--decoder", "openh264dec", "--duration", "1", "--fixture", 'missing"\\\n\t.h264'],
                            capture_output=True, text=True, timeout=3)
    report = json.loads(result.stdout)
    assert result.returncode == 2 and 'missing"\\\n\t.h264' in report["failure_reason"]
    print("PASS: escaped missing-fixture JSON")


def test_classification_when_forcing_known_and_unknown_names(binary: str, fixture: str) -> None:
    for name, hardware in (("nvh264dec", True), ("invented_nv_decoder", False)):
        result = subprocess.run([binary, "--decoder", name, "--duration", "1", "--fixture", fixture],
                                capture_output=True, text=True, timeout=12)
        report = json.loads(result.stdout)
        assert report["discovered_elements"][0]["is_hardware"] is hardware
        assert report["target_qualified"] is False
    print("PASS: explicit factory classification")


def test_resolution_when_request_differs_from_decoded_frames(binary: str, fixture: str) -> None:
    result = subprocess.run([binary, "--decoder", "openh264dec", "--duration", "1", "--profile", "640x480@30", "--fixture", fixture],
                            capture_output=True, text=True, timeout=12)
    report = json.loads(result.stdout)
    assert result.returncode == 2 and report["smoke_pass"] is False
    assert (report["observed_width"], report["observed_height"]) == (1280, 720)
    assert (report["requested_width"], report["requested_height"]) == (640, 480)
    print("PASS: observed resolution is not requested resolution")


def test_bus_error_when_access_unit_payload_is_corrupt(binary: str, fixture: str) -> None:
    del fixture
    with tempfile.TemporaryDirectory(prefix="decode-probe-corrupt-") as directory:
        path = Path(directory) / "corrupt.h264"
        path.write_bytes(bytes.fromhex("0000000109f000000001654455"))
        result = subprocess.run([binary, "--decoder", "openh264dec", "--duration", "1", "--fixture", str(path)],
                                capture_output=True, text=True, timeout=12)
    report = json.loads(result.stdout)
    assert result.returncode == 2 and report["pass"] is False
    assert report["frames_decoded"] == 0 and report["failure_reason"]
    assert report["duration_s"] < 3
    print("PASS: corrupt AUD payload fails promptly")


def test_deadline_when_worker_is_stopped(binary: str, fixture: str) -> None:
    started = time.monotonic()
    with subprocess.Popen([binary, "--decoder", "openh264dec", "--duration", "1", "--fixture", fixture],
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True) as process:
        children = Path(f"/proc/{process.pid}/task/{process.pid}/children")
        worker = None
        while time.monotonic() - started < 2 and process.poll() is None:
            pids = children.read_text().split()
            if pids:
                worker = int(pids[0])
                os.kill(worker, signal.SIGSTOP)
                break
            time.sleep(0.01)
        assert worker is not None, "probe has no isolated worker"
        try:
            stdout, stderr = process.communicate(timeout=12)
        finally:
            if process.poll() is None:
                os.kill(worker, signal.SIGKILL)
                process.kill()
                process.communicate(timeout=3)
    report = json.loads(stdout)
    assert process.returncode == 2, stderr
    assert report["pass"] is False and report["frames_decoded"] == 0
    assert 8 <= time.monotonic() - started < 12
    assert not Path(f"/proc/{worker}").exists()
    print("PASS: stopped worker killed/reaped at bounded deadline")


def test_report_failure_when_output_is_unwritable(binary: str, fixture: str) -> None:
    with tempfile.TemporaryDirectory(prefix="decode-probe-output-") as directory:
        result = subprocess.run([binary, "--decoder", "openh264dec", "--duration", "1", "--fixture", fixture, "--output", directory],
                                capture_output=True, text=True, timeout=12)
    report = json.loads(result.stdout)
    assert result.returncode == 2 and report["pass"] is False
    assert report["failure_reason"] == "Failed to write output JSON"
    print("PASS: report write failure propagates to status and JSON")


def main() -> int:
    binary, fixture = sys.argv[1:3]
    for test in (test_argument_errors_when_numeric_input_is_invalid,
                 test_json_when_missing_fixture_has_control_bytes,
                 test_classification_when_forcing_known_and_unknown_names,
                 test_resolution_when_request_differs_from_decoded_frames,
                 test_bus_error_when_access_unit_payload_is_corrupt,
                 test_report_failure_when_output_is_unwritable,
                 test_deadline_when_worker_is_stopped):
        test(binary, fixture)
    return 0


if __name__ == "__main__":
    sys.exit(main())
