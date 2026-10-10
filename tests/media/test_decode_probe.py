#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
import json
import os
import subprocess
import sys
import tempfile

def run_cmd(cmd, env=None):
    res = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, env=env, timeout=15)
    return res.returncode, res.stdout, res.stderr

def main():
    if len(sys.argv) < 3:
        print("Usage: test_decode_probe.py <path_to_binary> <path_to_fixture>")
        return 1

    binary = os.path.abspath(sys.argv[1])
    fixture = os.path.abspath(sys.argv[2])

    if not os.path.isfile(binary) or not os.access(binary, os.X_OK):
        print(f"FAIL: binary not executable at {binary}")
        return 1

    if not os.path.isfile(fixture):
        print(f"FAIL: fixture not found at {fixture}")
        return 1

    failures = []

    # Test 1: Help option returns 0
    code, stdout, _ = run_cmd([binary, "--help"])
    if code != 0 or "Usage:" not in stdout:
        failures.append("Test 1: --help failed")
    else:
        print("PASS: Test 1 (--help)")

    # Test 2: Valid smoke run (duration 2s) with host software decoder
    code, stdout, _ = run_cmd([binary, "--decoder", "openh264dec", "--profile", "1280x720@30", "--bitrate", "10M", "--duration", "2", "--fixture", fixture])
    try:
        report = json.loads(stdout)
        if report.get("smoke_pass") is not True:
            failures.append(f"Test 2: smoke run did not pass: {report.get('failure_reason')}")
        elif code != 2 or report.get("pass") is not False or report.get("requested_workload_met") is not False:
            failures.append("Test 2: tiny fixture falsely satisfied requested 10Mbps workload")
        elif report.get("target_qualified") is not False or not (0 < report.get("observed_bitrate_bps", 0) < 100000):
            failures.append("Test 2: fixture bitrate/qualification incorrectly reported")
        elif report.get("latency_samples") != report.get("frames_decoded") or report.get("unmatched_frames") != 0:
            failures.append("Test 2: incomplete PTS/frame correlation")
        elif report.get("dropped_frames") != 0:
            failures.append(f"Test 2: frame drops observed: {report.get('dropped_frames')}")
        elif report.get("p95_latency_ms", 999.0) > 33.0:
            failures.append(f"Test 2: p95 latency exceeded 33ms: {report.get('p95_latency_ms')}")
        elif report.get("cpu_percent", 999.0) > 50.0:
            failures.append(f"Test 2: CPU exceeded 50%: {report.get('cpu_percent')}")
        else:
            print(f"PASS: Test 2 (smoke run: decoder={report.get('decoder')} p95={report.get('p95_latency_ms')}ms cpu={report.get('cpu_percent')}%)")
    except Exception as e:
        failures.append(f"Test 2: invalid JSON: {e}")

    # Test 3: Negative - Malformed profile
    code, stdout, _ = run_cmd([binary, "--profile", "bad_profile", "--fixture", fixture])
    if code == 0:
        failures.append("Test 3: Malformed profile unexpectedly returned 0")
    else:
        print("PASS: Test 3 (malformed profile rejected)")

    # Test 4: Negative - Missing / Nonexistent decoder
    code, stdout, _ = run_cmd([binary, "--decoder", "nonexistent_decoder_element", "--duration", "1", "--fixture", fixture])
    try:
        report = json.loads(stdout)
        if report.get("pass") is not False:
            failures.append("Test 4: nonexistent decoder unexpectedly passed")
        else:
            print("PASS: Test 4 (nonexistent decoder failed typed JSON)")
    except Exception as e:
        failures.append(f"Test 4: invalid JSON: {e}")

    # Test 5: Negative - Invalid / corrupt fixture
    with tempfile.NamedTemporaryFile(suffix=".h264", delete=False) as tf:
        tf.write(b"NOT_A_VALID_H264_STREAM_GARBAGE_BYTES")
        invalid_fixture_path = tf.name

    try:
        code, stdout, _ = run_cmd([binary, "--duration", "1", "--fixture", invalid_fixture_path])
        report = json.loads(stdout)
        if report.get("pass") is not False:
            failures.append("Test 5: invalid fixture unexpectedly passed")
        else:
            print("PASS: Test 5 (invalid fixture failed typed JSON)")
    finally:
        if os.path.exists(invalid_fixture_path):
            os.remove(invalid_fixture_path)

    # Test 6: Isolation test - Hide Jetson nvv4l2decoder or verify software fallback report
    code, stdout, _ = run_cmd([binary, "--profile", "1280x720@30", "--bitrate", "10M", "--duration", "1", "--fixture", fixture])
    report = json.loads(stdout)
    found_nvv4l2 = False
    for elem in report.get("discovered_elements", []):
        if elem.get("name") == "nvv4l2decoder":
            found_nvv4l2 = True
            if elem.get("available") is False:
                print("PASS: Test 6 (nvv4l2decoder absence correctly discovered without assumption)")
    if not found_nvv4l2:
        failures.append("Test 6: nvv4l2decoder not reported in discovery")

    if failures:
        print("\nFailures encountered:")
        for f in failures:
            print(f" - {f}")
        return 2

    print("\nAll decode-probe positive and negative tests PASSED.")
    return 0

if __name__ == "__main__":
    sys.exit(main())
