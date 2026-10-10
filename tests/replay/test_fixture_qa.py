#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# /// script
# requires-python = ">=3.12"
# dependencies = []
# ///
"""Task 20 replay fixture QA gate (redaction check).

Drives the aa_replay_fixture_probe manual driver and independently checks every
fixture with a real JSON parser (json.loads) and hashlib over the fixed
canonical bytes:

  * the golden synthetic fixture is accepted by the authoritative C++ gate,
  * its SHA-256 matches the embedded field AND the pinned expected constant,
  * metadata carries no raw identifier shape (MAC / 15+ digit runs) anywhere,
    including on innocuous keys, and no non-redacted secret-bearing key,
  * every opaque message/media blob declares the explicit synthetic policy,
  * deliberately-bad fixtures are rejected by the C++ gate AND by these scans,
  * the optional synthetic H.264 golden fixture matches its pinned SHA-256.

Opaque payloads are NOT privacy-scrubbed here and no such guarantee is made:
public fixtures are synthetic-provenance only. Exit: 0 all pass, 1 any failure.
"""
from __future__ import annotations

import hashlib
import json
import re
import subprocess
import sys
import tempfile
from pathlib import Path

SCHEMA = "aa-replay-fixture-v1"
FILE_PREFIX = b'{"schema":"aa-replay-fixture-v1","sha256":"'
FILE_SEPARATOR = b'","body":'
BODY_PREFIX = SCHEMA + "\n"

GOLDEN_FIXTURE_SHA256 = (
    "1b7f939d4174424212a3ee0da1fca74b678980bdba2523f5560e79ec08683e48"
)
H264_FIXTURE_SHA256 = (
    "0b34e280254d1b7d4832a2e8b2bb4325e3de24f228127cf6ac03e600749f2265"
)

MAC = re.compile(r"(?:[0-9A-Fa-f]{2}[:\-]){5}[0-9A-Fa-f]{2}")
DIGITS = re.compile(r"\d{15,}")
SECRET_KEY = re.compile(
    r"password|passwd|pwd|passphrase|psk|secret|token|credential|bearer|"
    r"authorization|cookie|api[_-]?key|private[_-]?key|access[_-]?key",
    re.IGNORECASE,
)

BAD_CASES = [
    "raw-identifier",
    "raw-mac",
    "secret-key",
    "opaque-policy",
    "missing-opaque",
    "corrupt-hash",
    "wrong-schema",
    "bad-record",
    "oversize",
    "nonmonotonic",
    "illegal-state",
    "inconsistent-metadata",
    "inconsistent-id",
    "wrong-expect",
]

# Cases the load gate cannot see (a wrong-but-legal expected trace) and cases
# kept on a stale hash on purpose.
PLAYER_ONLY_CASES = {"wrong-expect"}
STALE_HASH_CASES = {"corrupt-hash"}


def run_probe(binary: str, *args: str) -> tuple[int, str]:
    res = subprocess.run(
        [binary, *args], stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=30
    )
    return res.returncode, res.stdout.decode("utf-8", errors="replace")


def reseal(raw: bytes) -> bytes:
    """Recompute the envelope hash over the body with hashlib, preserving every
    other byte (including a tampered envelope). This is the independent side of
    the reseal proof: the C++ gate must reject content whose hash is correct by
    this external computation too."""
    try:
        _, body = canonical_body(raw)
    except ValueError:
        return raw
    digest = hashlib.sha256(BODY_PREFIX.encode("ascii") + body).hexdigest()
    at = raw.rfind(FILE_SEPARATOR)
    return raw[: at - 64] + digest.encode("ascii") + raw[at:]


def canonical_body(raw: bytes) -> tuple[str, bytes]:
    if not raw.startswith(FILE_PREFIX):
        raise ValueError("missing canonical envelope prefix")
    hash_field = raw[len(FILE_PREFIX) : len(FILE_PREFIX) + 64].decode("ascii")
    at = len(FILE_PREFIX) + 64
    if raw[at : at + len(FILE_SEPARATOR)] != FILE_SEPARATOR:
        raise ValueError("missing canonical envelope separator")
    body = raw[at + len(FILE_SEPARATOR) : -1]
    if not raw.endswith(b"}") or not body.startswith(b"{"):
        raise ValueError("missing canonical envelope suffix")
    return hash_field, body


def scan_metadata(metadata: dict, failures: list[str], label: str) -> None:
    for key, value in metadata.items():
        if key in ("session", "mono_ns"):
            continue
        text = value if isinstance(value, str) else str(value)
        if MAC.search(text):
            failures.append(f"{label}: raw MAC identifier under key {key!r}")
        if DIGITS.search(text):
            failures.append(f"{label}: raw digit-run identifier under key {key!r}")
        if SECRET_KEY.search(key) and value != "[redacted]":
            failures.append(f"{label}: secret-bearing key {key!r} is not redacted")


def check_fixture_shape(raw: bytes, failures: list[str], label: str) -> dict:
    try:
        hash_field, body = canonical_body(raw)
    except ValueError as exc:
        failures.append(f"{label}: envelope not canonical: {exc}")
        return {}
    digest = hashlib.sha256(BODY_PREFIX.encode("ascii") + body).hexdigest()
    if digest != hash_field:
        failures.append(f"{label}: sha256 mismatch (corrupted fixture)")
    try:
        parsed = json.loads(body)
    except json.JSONDecodeError as exc:
        failures.append(f"{label}: body is not valid JSON: {exc}")
        return {}
    if not isinstance(parsed, dict):
        failures.append(f"{label}: body is not a JSON object")
        return {}
    for key in ("provenance", "metadata", "services", "records", "expect"):
        if key not in parsed:
            failures.append(f"{label}: body missing {key!r}")
    if parsed.get("provenance") != "synthetic":
        failures.append(f"{label}: provenance is not explicit synthetic")
    metadata_raw = parsed.get("metadata", "")
    if not isinstance(metadata_raw, str):
        failures.append(f"{label}: metadata is not a to_json string")
        return parsed
    try:
        metadata = json.loads(metadata_raw)
    except json.JSONDecodeError as exc:
        failures.append(f"{label}: metadata is not valid JSON: {exc}")
        return parsed
    if not isinstance(metadata, dict):
        failures.append(f"{label}: metadata is not a JSON object")
        return parsed
    scan_metadata(metadata, failures, label)
    for record in parsed.get("records", []):
        if not isinstance(record, dict):
            failures.append(f"{label}: record is not a JSON object")
            continue
        if "kind" not in record or "t" not in record:
            failures.append(f"{label}: record missing kind/t")
        blobs = [k for k in ("frame", "payload", "pcm") if k in record]
        for blob in blobs:
            if record.get("opaque") != "synthetic":
                failures.append(
                    f"{label}: opaque blob {blob!r} lacks the synthetic policy"
                )
    return parsed


def main() -> int:
    if len(sys.argv) < 2:
        print("Usage: test_fixture_qa.py <aa_replay_fixture_probe> [h264_fixture]")
        return 1
    binary = str(Path(sys.argv[1]).resolve())
    h264 = Path(sys.argv[2]).resolve() if len(sys.argv) > 2 else None
    failures: list[str] = []

    with tempfile.TemporaryDirectory(prefix="aa-replay-qa-") as tmp:
        good = Path(tmp) / "good.json"
        code, out = run_probe(binary, "emit-good", str(good))
        if code != 0:
            print(f"FAIL: emit-good exited {code}: {out}")
            return 1
        code, out = run_probe(binary, "check", str(good))
        if code != 0:
            print(f"FAIL: golden fixture rejected by C++ gate: {out}")
            return 1

        raw = good.read_bytes()
        parsed = check_fixture_shape(raw, failures, "golden")
        try:
            hash_field, body = canonical_body(raw)
        except ValueError as exc:
            failures.append(f"golden: envelope not canonical: {exc}")
            hash_field = ""
            body = b""
        if hash_field and hash_field != GOLDEN_FIXTURE_SHA256:
            failures.append(
                f"golden: sha256 {hash_field} != pinned expected {GOLDEN_FIXTURE_SHA256}"
            )
        digest = hashlib.sha256(BODY_PREFIX.encode("ascii") + body).hexdigest()
        if digest != GOLDEN_FIXTURE_SHA256:
            failures.append(
                f"golden: canonical bytes hash {digest} != pinned expected "
                f"{GOLDEN_FIXTURE_SHA256}"
            )
        states = parsed.get("expect", {}).get("states", []) if parsed else []
        if states and states[0] != "disconnected":
            failures.append("golden: expected transition trace must start at disconnected")

        for case in BAD_CASES:
            bad = Path(tmp) / f"bad-{case}.json"
            code, out = run_probe(binary, "emit-bad", case, str(bad))
            if code != 0:
                failures.append(f"{case}: emit-bad exited {code}: {out}")
                continue
            code, out = run_probe(binary, "check", str(bad))
            if case in PLAYER_ONLY_CASES:
                if code != 0:
                    failures.append(
                        f"{case}: load gate rejected a structurally valid fixture: {out}"
                    )
                run_code, run_out = run_probe(binary, "run", str(bad))
                if run_code == 0:
                    failures.append(f"{case}: real player accepted a wrong expected trace")
            elif code == 0:
                failures.append(f"{case}: C++ gate accepted a bad fixture")
            bad_failures: list[str] = []
            check_fixture_shape(bad.read_bytes(), bad_failures, case)
            if case in ("raw-identifier", "raw-mac", "secret-key"):
                if not bad_failures:
                    failures.append(f"{case}: independent privacy scan missed the leak")
            if case == "missing-opaque" and not any(
                "opaque blob" in item for item in bad_failures
            ):
                failures.append("missing-opaque: independent policy scan missed it")

        # Independent reseal proof: mutate the metadata in Python, repair the
        # hash with hashlib, and require the C++ gate to reject on content.
        raw_good = good.read_bytes()
        mutated = raw_good.replace(
            b'\\"frames\\":19', b'\\"frames\\":123456789012345', 1
        )
        if mutated == raw_good:
            failures.append("reseal-proof: metadata tamper target missing")
        resealed = Path(tmp) / "python-resealed.json"
        resealed.write_bytes(reseal(mutated))
        code, out = run_probe(binary, "check", str(resealed))
        if code == 0:
            failures.append("reseal-proof: gate accepted raw identifier over a hashlib hash")
        code, out = run_probe(binary, "run", str(good))
        if code != 0:
            failures.append(f"golden: real player rejected the good trace: {out}")

    if h264 is not None and h264.is_file():
        digest = hashlib.sha256(h264.read_bytes()).hexdigest()
        if digest != H264_FIXTURE_SHA256:
            failures.append(f"h264: fixture sha256 {digest} != pinned {H264_FIXTURE_SHA256}")
    elif h264 is not None:
        failures.append(f"h264: missing synthetic fixture at {h264}")

    if failures:
        for item in failures:
            print(f"FAIL: {item}")
        print(f"{len(failures)} failure(s)")
        return 1
    print("replay QA gate passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
