#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Host task-38 recovery QA runner and fail-closed evidence validator.

Run:   wireless_recovery_qa.py run --build build/host-dev --out EVIDENCE.json
Check: wireless_recovery_qa.py validate EVIDENCE.json

The runner executes the registered ctest scenarios one at a time with
GTEST_OUTPUT, converts their recorded monotonic observations to JSON, then runs
the same validator used by the mutation tests. Blocked is never promoted to
pass, and a green host matrix alone never produces qualification or sign-off.
"""
from __future__ import annotations

import argparse
import datetime as dt
import json
import os
from pathlib import Path
import subprocess
import sys
import xml.etree.ElementTree as ET

from recovery_host_gates import (
    HOST_SCENARIOS, OFFSETS, REQUIRED_OFFSETS, ACTIVATION_SCENARIOS,
    NEW_ACTIVATION_SCENARIOS, AP_OFF_SCENARIOS, validate_host,
)
from recovery_scope_gates import validate_scope
from recovery_device_gates import validate_device
BLOCKED_SCENARIOS = {
    "B01": "real phone lock/unlock via ADB is unavailable; no host surrogate claim",
    "B02": "real Android Auto restart via ADB is unavailable; no host surrogate claim",
    "B03": "real bluetoothctl disconnect/block and Jetson RF/driver behavior are unqualified",
}


def null_reason(scenario: str, field: str) -> str:
    if field == "grace_expiry_observed_ms":
        return "not applicable: no confirmed ACTIVE consumer grace in this scenario"
    if field == "fresh_acquisition_ms":
        return "not applicable: this scenario has no fresh AP acquisition"
    if field == "teardown_started_ms":
        if scenario == "R07":
            return "not applicable: SIGKILL has no graceful helper cleanup request"
        return "not applicable: no graceful helper cleanup request is claimed"
    if field == "old_ap_inactive_ms":
        return "not applicable: this scenario does not claim old-AP shutdown"
    if scenario == "R10":
        return "not applicable: synthetic second peer is rejected before admission"
    return "not applicable: observation is outside this scenario's terminal claim"


def parse_offsets(properties: dict[str, str]) -> dict[str, int | None]:
    result: dict[str, int | None] = {}
    for field in OFFSETS:
        value = properties.get(field)
        try:
            result[field] = int(value) if value is not None else None
        except ValueError:
            result[field] = None
    return result


def scenario_from_cases(scenario: str, cases: list[dict], returncode: int) -> dict:
    for case in cases:
        case["offsets"] = parse_offsets(case["observations"])
        cleanup = case["observations"].get("hard_kill_cleanup", "")
        case["status"] = "fail" if case.pop("failed") or returncode else (
            "blocked" if cleanup.startswith("BLOCKED") else "pass")
    status = "fail" if returncode or not cases or any(c["status"] == "fail" for c in cases) else (
        "blocked" if any(c["status"] == "blocked" for c in cases) else "pass")
    result = {
        "scenario_id": scenario,
        "category": "host",
        "status": status,
        "terminal_state": "; ".join(dict.fromkeys(
            value for case in cases for key in ("terminal_state", "protocol_result", "hard_kill_cleanup")
            if (value := case["observations"].get(key)))) or (
                "host assertions failed" if status == "fail" else "host assertions passed"),
        "protocol_elapsed_ms": None,
        "blocked_reason": next((case["observations"]["hard_kill_cleanup"] for case in cases
                                if case["observations"].get("hard_kill_cleanup", "").startswith("BLOCKED")), None),
        "cases": cases,
        "null_reasons": {},
    }
    for field in OFFSETS:
        values = [case["offsets"][field] for case in cases if case["offsets"][field] is not None]
        result[field] = max(values) if values else None
        if result[field] is None:
            result["null_reasons"][field] = null_reason(scenario, field)
    result["protocol_elapsed_ms"] = result["terminal_recovery_ms"]
    for field in ("old_activation_id", "new_activation_id"):
        values = [int(case["observations"][field]) for case in cases if field in case["observations"]]
        result[field] = values[0] if values else None
    return result


def failed_scenario(scenario: str, reason: str) -> dict:
    result = scenario_from_cases(scenario, [], 1)
    result["terminal_state"] = reason
    result["protocol_elapsed_ms"] = None
    return result


def blocked_scenarios() -> list[dict]:
    return [{
        "scenario_id": scenario,
        "category": "permanently-blocked-device",
        "status": "blocked",
        "terminal_state": "not executed; withdrawn claim recorded",
        "protocol_elapsed_ms": None,
        "blocked_reason": reason,
        **{field: None for field in OFFSETS},
        "null_reasons": {field: "not applicable: permanently blocked device scenario" for field in OFFSETS},
        "old_activation_id": None,
        "new_activation_id": None,
        "cases": [],
    } for scenario, reason in BLOCKED_SCENARIOS.items()]


def base_document(*, full_scope: bool = False) -> dict:
    document = {
        "task": "task-38-wireless-recovery",
        "git_commit": "unknown",
        "timestamp": dt.datetime.now(dt.timezone.utc).isoformat(),
        "compliance_status": "pending-owner-signoff",
        "withdrawn_claims": [
            *(f"{scenario}: {reason}" for scenario, reason in BLOCKED_SCENARIOS.items()),
            "host NM/BlueZ doubles do not qualify real BlueZ signal completeness, RF, driver channel policy, or installed-service behavior",
            "no crash-durable managed-peer/revocation-ledger claim and no full wireless compliance claim",
        ],
        "owner_approval": {
            "decision": "bounded cleanup (option 1)",
            "date": "2026-10-24",
            "v2_7": "serialized-freshness clarification, owner option A, 2026-10-25",
        },
        "reduced_scope_signoff": None,
        "no_ap_leak_verified": False,
        "trust_store_unmodified_on_reconnect": False,
        "hard_kill_cleanup": {"status": "blocked", "reason": BLOCKED_SCENARIOS["B01"]},
        "scenarios": [],
    }
    if full_scope:
        document.update(schema_version=2, qualification_scope="full-task38",
                        device_evidence={}, full_scope_signoff=None)
    return document


def run_matrix(build: Path, out: Path, repo: Path) -> dict:
    logs = (out.parent / f"{out.stem}-logs").resolve()
    logs.mkdir(parents=True, exist_ok=True)
    document = base_document(full_scope=True)
    try:
        document["git_commit"] = subprocess.check_output(
            ["git", "rev-parse", "HEAD"], cwd=repo, text=True).strip()
        document["worktree_dirty"] = bool(subprocess.check_output(
            ["git", "status", "--porcelain"], cwd=repo, text=True).strip())
    except (OSError, subprocess.CalledProcessError):
        document["worktree_dirty"] = None
    host = []
    for scenario in HOST_SCENARIOS:
        xml = logs / f"{scenario}.xml"
        log = logs / f"{scenario}.log"
        xml.unlink(missing_ok=True)
        environment = dict(os.environ, GTEST_OUTPUT=f"xml:{xml}")
        completed = subprocess.run(
            ["ctest", "--test-dir", str(build), "-R", f"^wireless_recovery_{scenario}$", "--output-on-failure"],
            env=environment, capture_output=True, text=True, check=False)
        log.write_text(completed.stdout + completed.stderr)
        cases = []
        if xml.exists():
            for case in ET.parse(xml).getroot().iter("testcase"):
                properties = {prop.attrib["name"]: prop.attrib["value"]
                              for prop in case.findall("./properties/property")}
                cases.append({"name": case.attrib.get("name", "unknown"),
                              "failed": bool(case.findall("failure") or case.findall("error")),
                              "observations": properties})
        host.append(scenario_from_cases(scenario, cases, completed.returncode) if cases
                    else failed_scenario(scenario, "ctest failed or emitted no gtest XML; see scenario log"))
    document["scenarios"] = host + blocked_scenarios()
    trust = [case["observations"].get("trust_store_unmodified") for scenario in host for case in scenario["cases"]]
    document["trust_store_unmodified_on_reconnect"] = bool(trust) and all(value == "true" for value in trust)
    document["no_ap_leak_verified"] = expected_ap_leak(document)
    r07 = next(scenario for scenario in host if scenario["scenario_id"] == "R07")
    document["hard_kill_cleanup"] = {
        "status": r07["status"], "reason": r07["blocked_reason"],
        "terminal_state": r07["terminal_state"],
    }
    if any(scenario["status"] == "fail" for scenario in host):
        document["compliance_status"] = "failed"
    return document


def expected_ap_leak(document: dict) -> bool:
    host = [s for s in document["scenarios"] if s["scenario_id"] in HOST_SCENARIOS]
    return all(s["status"] == "pass" for s in host) and all(
        s["old_ap_inactive_ms"] is not None for s in host if s["scenario_id"] in AP_OFF_SCENARIOS)


def validate(document: dict, evidence_root: Path | None = None) -> list[str]:
    errors = []
    for key in ("task", "git_commit", "timestamp", "compliance_status", "withdrawn_claims",
                "owner_approval", "reduced_scope_signoff", "no_ap_leak_verified",
                "trust_store_unmodified_on_reconnect", "hard_kill_cleanup", "scenarios"):
        if key not in document:
            errors.append(f"missing top-level field {key}")
    if errors:
        return errors
    scenarios = {s.get("scenario_id"): s for s in document["scenarios"]}
    if len(scenarios) != len(document["scenarios"]):
        errors.append("duplicate scenario_id")
    for scenario in (*HOST_SCENARIOS, *BLOCKED_SCENARIOS):
        if scenario not in scenarios:
            errors.append(f"missing scenario {scenario}")
    if errors:
        return errors
    errors.extend(validate_scope(document, base_document()["withdrawn_claims"]))
    if errors:
        return errors
    if document.get("schema_version") == 2:
        errors.extend(validate_device(document, evidence_root or Path.cwd()))
    errors.extend(validate_host(scenarios))
    if any(scenarios[s]["status"] == "fail" for s in HOST_SCENARIOS):
        if document["compliance_status"] != "failed":
            errors.append("failed host scenario requires failed compliance_status")
    elif document["compliance_status"] == "failed" and not any(
            isinstance(entry, dict) and entry.get("status") == "fail"
            for entry in (document["device_evidence"].values()
                          if isinstance(document.get("device_evidence"), dict) else ())):
        errors.append("failed compliance_status has no failed host scenario")
    if document["no_ap_leak_verified"] != expected_ap_leak(document):
        errors.append("unsupported no_ap_leak_verified value")
    all_cases = [case for scenario in HOST_SCENARIOS for case in scenarios[scenario].get("cases", [])]
    expected_trust = bool(all_cases) and all(
        case.get("observations", {}).get("trust_store_unmodified") == "true" for case in all_cases)
    if document["trust_store_unmodified_on_reconnect"] != expected_trust:
        errors.append("trust_store_unmodified_on_reconnect does not match per-case evidence")
    if not document["trust_store_unmodified_on_reconnect"]:
        errors.append("trust_store_unmodified_on_reconnect is not evidenced")
    hard_kill = document["hard_kill_cleanup"]
    r07 = scenarios["R07"]
    if not isinstance(hard_kill, dict) or hard_kill.get("status") != r07["status"]:
        errors.append("hard_kill_cleanup status does not match R07")
    elif r07["status"] == "blocked" and hard_kill.get("reason") != r07.get("blocked_reason"):
        errors.append("hard_kill_cleanup reason does not match R07")
    return errors


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    run = sub.add_parser("run")
    run.add_argument("--build", type=Path, required=True)
    run.add_argument("--out", type=Path, required=True)
    run.add_argument("--repo", type=Path, default=Path(__file__).resolve().parents[2])
    check = sub.add_parser("validate")
    check.add_argument("evidence", type=Path)
    args = parser.parse_args()
    if args.command == "run":
        args.out.parent.mkdir(parents=True, exist_ok=True)
        document = run_matrix(args.build, args.out, args.repo)
        args.out.write_text(json.dumps(document, indent=2) + "\n")
        problems = validate(document, args.out.parent)
    else:
        problems = validate(json.loads(args.evidence.read_text()), args.evidence.parent)
    for problem in problems:
        print(f"FAIL {problem}")
    if problems:
        return 1
    print(f"PASS validated {args.out if args.command == 'run' else args.evidence}")
    return 1 if args.command == "run" and document["compliance_status"] == "failed" else 0


if __name__ == "__main__":
    raise SystemExit(main())
