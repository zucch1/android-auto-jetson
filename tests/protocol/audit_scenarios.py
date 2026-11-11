#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# /// script
# requires-python = ">=3.12"
# dependencies = []
# ///
# Run: python3 -B tests/protocol/audit_scenarios.py ROOT CASE
"""Exercise protobuf audit guard against private, temporary mutation fixtures.

Tests positive and negative scenarios to verify that the guard detects
any drift, tampering, missing files, duplicate finding IDs, invalid
dispositions, or unexplained deletions.
"""
from __future__ import annotations

import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
from typing import Final

AUDIT_SCRIPT: Final[str] = "tools/protocol/protobuf_audit.py"
INVENTORY_PATH: Final[str] = "docs/evidence/protobuf-audit-inventory.json"
REPORT_PATH: Final[str] = "docs/evidence/protobuf-audit-report.md"


def main() -> int:
    if len(sys.argv) < 3:
        print("Usage: audit_scenarios.py ROOT CASE", file=sys.stderr)
        return 2

    root = Path(sys.argv[1]).resolve()
    case = sys.argv[2]

    # Create isolated temporary fixture with symlinks/copies
    fixture = Path(tempfile.mkdtemp(prefix=f"audit-scenario-{case}-"))
    try:
        # Copy required directory trees
        shutil.copytree(root / "third_party", fixture / "third_party", symlinks=True)
        shutil.copytree(root / "docs", fixture / "docs", symlinks=True)
        (fixture / "tools/protocol").mkdir(parents=True, exist_ok=True)
        shutil.copy2(root / AUDIT_SCRIPT, fixture / AUDIT_SCRIPT)
        shutil.copy2(root / "PROVENANCE.md", fixture / "PROVENANCE.md")

        inventory_file = fixture / INVENTORY_PATH
        expected_error = ""

        # Apply specific scenario mutation
        match case:
            case "baseline":
                expected_error = ""

            case "drift-inventory":
                data = json.loads(inventory_file.read_text(encoding="utf-8"))
                data["summary"]["title"] = "drifted-title"
                inventory_file.write_text(json.dumps(data, indent=2, sort_keys=True) + "\n", encoding="utf-8")
                expected_error = "INVENTORY_DRIFT"

            case "missing-file":
                # Remove an arbitrary AASDK proto file
                target = next((fixture / "third_party/aasdk/protobuf/aap_protobuf").rglob("*.proto"))
                target.unlink()
                expected_error = "MISSING_PROTO_FILE"

            case "tampered-hash":
                # Mutate content of an arbitrary proto file
                target = next((fixture / "third_party/aasdk/protobuf/aap_protobuf").rglob("*.proto"))
                with target.open("ab") as fp:
                    fp.write(b"\n// tampered content\n")
                expected_error = "CONTENT_HASH_MISMATCH"

            case "duplicate-id":
                data = json.loads(inventory_file.read_text(encoding="utf-8"))
                # Duplicate the first finding
                first = dict(data["discrepancy_ledger"][0])
                data["discrepancy_ledger"].append(first)
                inventory_file.write_text(json.dumps(data, indent=2, sort_keys=True) + "\n", encoding="utf-8")
                expected_error = "DUPLICATE_FINDING_ID"

            case "missing-field":
                data = json.loads(inventory_file.read_text(encoding="utf-8"))
                # Remove 'disposition' from first finding
                del data["discrepancy_ledger"][0]["disposition"]
                inventory_file.write_text(json.dumps(data, indent=2, sort_keys=True) + "\n", encoding="utf-8")
                expected_error = "INCOMPLETE_FINDING"

            case "invalid-disposition":
                data = json.loads(inventory_file.read_text(encoding="utf-8"))
                data["discrepancy_ledger"][0]["disposition"] = "unauthorized-disposition-value"
                inventory_file.write_text(json.dumps(data, indent=2, sort_keys=True) + "\n", encoding="utf-8")
                expected_error = "INVALID_DISPOSITION"

            case "unexplained-deletion":
                data = json.loads(inventory_file.read_text(encoding="utf-8"))
                # Remove legacy finding D1-800A-NAME
                data["discrepancy_ledger"] = [
                    d for d in data["discrepancy_ledger"] if d["id"] != "D1-800A-NAME"
                ]
                inventory_file.write_text(json.dumps(data, indent=2, sort_keys=True) + "\n", encoding="utf-8")
                expected_error = "FINDING_DELETED"

            case _:
                print(f"Unknown scenario case: {case}", file=sys.stderr)
                return 2

        result = subprocess.run(
            [sys.executable, "-B", str(fixture / AUDIT_SCRIPT), "--root", str(fixture), "--check"],
            capture_output=True,
            text=True,
            check=False,
        )

        print(f"case={case} exit={result.returncode}")
        if result.stdout:
            print(result.stdout, end="")
        if result.stderr:
            print(result.stderr, end="")

        if expected_error:
            if result.returncode != 1:
                print(f"FAIL: expected returncode 1, got {result.returncode}", file=sys.stderr)
                return 1
            if expected_error not in result.stderr and expected_error not in result.stdout:
                print(f"FAIL: expected error '{expected_error}' not found in output", file=sys.stderr)
                return 1
        else:
            if result.returncode != 0:
                print(f"FAIL: baseline expected returncode 0, got {result.returncode}", file=sys.stderr)
                return 1

        print(f"scenario-test: PASS case={case}")
        return 0

    finally:
        shutil.rmtree(fixture, ignore_errors=True)


if __name__ == "__main__":
    raise SystemExit(main())
