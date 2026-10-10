#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# /// script
# requires-python = ">=3.12"
# dependencies = []
# ///
"""Task 19 diagnostics JSON privacy check (authoritative validity proof).

Runs the test-only aa_diagnostics_json_probe, parses every emitted line with a
real JSON parser (json.loads) and asserts:
  * each line is valid JSON (fields remain valid JSON),
  * no raw MAC / phone identifier / location / credential appears anywhere,
  * the structured schema (kind, session, mono_ns) and safe version fields are
    preserved,
  * arbitrary strings/unknown names fail closed while typed scalars survive.

Exit: 0 all pass, 1 any failure.
"""
from __future__ import annotations

import json
import os
import subprocess
import sys

# Raw values the probe plants in fixtures; none may appear in serialized output.
RAW_SECRETS = [
    "AA:BB:CC:DD:EE:FF",  # Bluetooth MAC
    "hunter2-secret",  # credential
    "s3cr3t",  # credential in unknown field
    "tok-999",  # token
    "abc.def",  # bearer token
    "123456789012345",  # IMEI
    "37.7749",  # latitude
    "-122.4194",  # longitude
    "unlabelled-secret",  # unlabelled secret on a sensitive key (correction)
    "8F3A22B19C0D77EE",  # unknown opaque identifier (correction)
    "evil",  # reserved-envelope override value (correction)
    "opaque-value",
    "424242",  # numeric credential on a safe telemetry key
]


def run_probe(binary: str) -> tuple[int, str, str]:
    res = subprocess.run(
        [binary], stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=15
    )
    try:
        stdout = res.stdout.decode("utf-8")
    except UnicodeDecodeError as exc:
        return 1, "", f"probe emitted invalid UTF-8 (privacy gap: {exc})"
    return res.returncode, stdout, res.stderr.decode("utf-8", errors="replace")


def main() -> int:
    if len(sys.argv) < 2:
        print("Usage: test_json.py <path_to_probe_binary>")
        return 1
    binary = os.path.abspath(sys.argv[1])
    if not os.path.isfile(binary) or not os.access(binary, os.X_OK):
        print(f"FAIL: probe binary not executable at {binary}")
        return 1

    code, stdout, stderr = run_probe(binary)
    if code != 0:
        print(f"FAIL: probe exited {code}: {stderr}")
        return 1

    lines = [line for line in stdout.splitlines() if line.strip()]
    failures: list[str] = []

    # Test 1: every line is valid JSON and free of raw control bytes.
    parsed: list[dict] = []
    for idx, line in enumerate(lines):
        if any(ord(ch) < 0x20 for ch in line):
            failures.append(f"Test 1: line {idx} contains a raw control byte")
        try:
            obj = json.loads(line)
        except json.JSONDecodeError as exc:  # pragma: no cover - failure path
            failures.append(f"Test 1: line {idx} is not valid JSON: {exc}")
            continue
        if not isinstance(obj, dict):
            failures.append(f"Test 1: line {idx} is not a JSON object")
            continue
        parsed.append(obj)
    if len(parsed) != len(lines):
        failures.append("Test 1: not every line parsed as a JSON object")
    else:
        print(f"PASS: Test 1 ({len(lines)} lines are valid JSON)")

    # Test 2: no raw secret/MAC/location/credential appears in any output line.
    all_output = stdout
    leaked = [s for s in RAW_SECRETS if s in all_output]
    if leaked:
        failures.append(f"Test 2: raw values leaked into JSON: {leaked}")
    else:
        print("PASS: Test 2 (no raw MAC/identifier/location/credential in output)")

    # Test 3: structured schema and safe version fields survive.
    schema = next((o for o in parsed if o.get("kind") == "session_state_changed"), None)
    if schema is None:
        failures.append("Test 3: no session_state_changed schema event")
    else:
        if schema.get("session") != 42:
            failures.append(f"Test 3: session id wrong: {schema.get('session')}")
        if schema.get("mono_ns") != 123456789:
            failures.append(f"Test 3: mono_ns wrong: {schema.get('mono_ns')}")
        if schema.get("from_state") != "discovering" or schema.get("to_state") != "connecting":
            failures.append("Test 3: state transition fields wrong")
        if schema.get("target_version") != "jetson-r39.2.1":
            failures.append(f"Test 3: target_version wrong: {schema.get('target_version')}")
        if "Android 16 AA 17.7.663654" not in schema.get("phone_version", ""):
            failures.append(f"Test 3: phone_version wrong: {schema.get('phone_version')}")
        if not str(schema.get("phone_mac", "")).startswith("id-"):
            failures.append(f"Test 3: phone_mac not pseudonymized: {schema.get('phone_mac')}")
        print("PASS: Test 3 (structured schema + versions preserved)")

    # Test 4: freeform text cannot escape via a known key; typed scalars survive.
    esc = next((o for o in parsed if o.get("kind") == "decode_stats"), None)
    if esc is None:
        failures.append("Test 4: no escaping fixture found")
    else:
        if esc.get("reason") != "[redacted]":
            failures.append("Test 4: freeform reason was not redacted")
        if esc.get("frames") != 18000 or esc.get("p95_latency_ms") != 33.5:
            failures.append("Test 4: numeric fields wrong")
        if esc.get("dropped") is not False:
            failures.append("Test 4: boolean field wrong")
        print("PASS: Test 4 (freeform text redacted + typed scalars)")

    # Test 5: credential/location fields are explicitly redacted.
    leak_obj = next((o for o in parsed if o.get("kind") == "helper_call"), None)
    loc_obj = next((o for o in parsed if o.get("kind") == "transport_stats"), None)
    if leak_obj is not None and leak_obj.get("psk") != "[redacted]":
        failures.append(f"Test 5: typed credential not redacted: {leak_obj.get('psk')}")
    if loc_obj is not None and loc_obj.get("gps") != "[redacted]":
        failures.append(f"Test 5: typed location not redacted: {loc_obj.get('gps')}")
    print("PASS: Test 5 (typed credential/location redacted)")

    # Test 6: unknown names drop, reserved keys cannot override, invalid text redacts.
    gap_idx = next(
        (i for i, o in enumerate(parsed) if o.get("kind") == "reconnect"), None
    )
    gap_line = next((ln for ln in lines if '"kind":"reconnect"' in ln), None)
    if gap_idx is None or gap_line is None:
        failures.append("Test 6: no correction gap fixture found")
    else:
        gap = parsed[gap_idx]
        line = gap_line
        for reserved in ('"kind":', '"session":', '"mono_ns":'):
            if line.count(reserved) != 1:
                failures.append(f"Test 6: reserved key {reserved} not unique: {line}")
        if gap.get("session") != 9 or gap.get("mono_ns") != 7:
            failures.append(f"Test 6: envelope values wrong: {gap.get('session')}")
        if gap.get("password") != "[redacted]":
            failures.append(f"Test 6: sensitive key not redacted: {gap.get('password')}")
        if "opaque_id" in gap or "lat" in gap:
            failures.append("Test 6: unknown key was emitted")
        if gap.get("gps") != "[redacted]":
            failures.append(f"Test 6: numeric location not redacted: {gap.get('lat')}")
        reason = gap.get("reason", "")
        if reason != "[redacted]":
            failures.append(f"Test 6: invalid freeform reason survived: {reason!r}")
        if "\xff" in reason:
            failures.append("Test 6: raw invalid byte survived into JSON")
        print("PASS: Test 6 (unknown names dropped + reserved envelope + invalid text redacted)")

    # Test 7: opaque strings and sensitive numbers fail closed on known keys.
    opaque = next((o for o in parsed if o.get("kind") == "pairing_decided"), None)
    if opaque is None:
        failures.append("Test 7: no opaque-secret fixture")
    else:
        for key in ("reason", "status", "version", "frames"):
            if opaque.get(key) != "[redacted]":
                failures.append(f"Test 7: opaque/sensitive value survived on {key}")
        if "8F3A22B19C0D77EE" in opaque:
            failures.append("Test 7: opaque key survived")

    if failures:
        print("FAILURES:")
        for f in failures:
            print(f"  - {f}")
        return 1
    print("PASS: Test 7 (opaque strings/keys and sensitive numeric telemetry hidden)")
    print("ALL PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
